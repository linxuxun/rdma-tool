#include "rdma_server.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

rc_code_t server_bind_and_listen(rdma_conn_t *conn, const char *ip, int port) {
    if (!conn || !conn->listen_id) return RC_INVALID;

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = inet_addr(ip)
    };

    if (rdma_bind_addr(conn->listen_id, (struct sockaddr *)&addr) != 0) {
        LOG_ERROR("rdma_bind_addr: %s", strerror(errno));
        return RC_RDMA_FAIL;
    }
    if (rdma_listen(conn->listen_id, 10) != 0) {
        LOG_ERROR("rdma_listen: %s", strerror(errno));
        return RC_RDMA_FAIL;
    }

    LOG_INFO("Server listening on %s:%d", ip, port);
    return RC_OK;
}

rc_code_t server_accept(rdma_conn_t *conn) {
    if (!conn) return RC_INVALID;

    struct rdma_cm_event *event = NULL;
    rc_code_t rc;

    if (rdma_get_cm_event(conn->ec, &event) != 0) {
        LOG_ERROR("rdma_get_cm_event: %s", strerror(errno));
        return RC_EVENT_FAIL;
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        LOG_ERROR("Unexpected event: %s (%d)",
                  rdma_event_str(event->event), event->event);
        rdma_ack_cm_event(event);
        return RC_INVALID;
    }

    conn->cm_id = event->id;
    conn->cm_id->context = conn;
    rdma_ack_cm_event(event);

    LOG_INFO("Connection request received, building data plane...");

    rc = create_pd(conn);            if (rc != RC_OK) goto fail;
    rc = create_comp_channel(conn);  if (rc != RC_OK) goto fail;
    rc = create_cq(conn);            if (rc != RC_OK) goto fail;
    rc = create_mr(conn);            if (rc != RC_OK) goto fail;
    rc = create_qp(conn);            if (rc != RC_OK) goto fail;
    rc = req_notify_cq(conn);        if (rc != RC_OK) goto fail;
    rc = post_recv(conn);            if (rc != RC_OK) goto fail;

    if (rdma_accept(conn->cm_id, NULL) != 0) {
        LOG_ERROR("rdma_accept: %s", strerror(errno));
        rc = RC_RDMA_FAIL; goto fail;
    }

    if (rdma_get_cm_event(conn->ec, &event) != 0) {
        LOG_ERROR("rdma_get_cm_event (establish)"); rc = RC_EVENT_FAIL; goto fail;
    }
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        LOG_ERROR("Unexpected event: %s (%d)",
                  rdma_event_str(event->event), event->event);
        rdma_ack_cm_event(event); rc = RC_INVALID; goto fail;
    }
    rdma_ack_cm_event(event);

    conn->state = CONN_STATE_CONNECTED;
    LOG_INFO("Connection established");

    /* 交换 MR 信息 */
    rc = exchange_peer_info(conn);
    if (rc != RC_OK) goto fail;

    return RC_OK;

fail:
    if (conn->cm_id && conn->cm_id->qp) rdma_destroy_qp(conn->cm_id);
    if (conn->send_mr) ibv_dereg_mr(conn->send_mr);
    if (conn->recv_mr) ibv_dereg_mr(conn->recv_mr);
    free(conn->send_buf); free(conn->recv_buf);
    conn->send_buf = NULL; conn->recv_buf = NULL;
    if (conn->cq)           ibv_destroy_cq(conn->cq);
    if (conn->comp_channel)  ibv_destroy_comp_channel(conn->comp_channel);
    if (conn->pd)           ibv_dealloc_pd(conn->pd);
    conn->send_mr = NULL; conn->recv_mr = NULL;
    conn->cq = NULL; conn->comp_channel = NULL; conn->pd = NULL;
    return rc;
}

rc_code_t run_server(const char *ip, int port) {
    rdma_conn_t conn;
    rc_code_t rc;

    rdma_conn_init(&conn, true);

    rc = create_event_channel(&conn);     if (rc != RC_OK) goto done;
    rc = create_listen_id(&conn);        if (rc != RC_OK) goto done;
    rc = server_bind_and_listen(&conn, ip, port);
                                         if (rc != RC_OK) goto done;
    rc = server_accept(&conn);            if (rc != RC_OK) goto done;
    rc = init_epoll(&conn);               if (rc != RC_OK) goto done;

    /* 服务端事件循环（被动接收消息） */
    event_loop(&conn);

done:
    rdma_conn_destroy(&conn);
    return rc;
}
