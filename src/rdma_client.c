#include "rdma_client.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

static struct rdma_cm_event *wait_cm_event(rdma_conn_t *conn,
                                            enum rdma_cm_event_type expected,
                                            int timeout_ms) {
    struct rdma_cm_event *event = NULL;
    struct pollfd pfd = { .fd = conn->ec->fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        if (ret == 0) LOG_WARN("CM event timeout: %s", rdma_event_str(expected));
        else          LOG_ERROR("poll: %s", strerror(errno));
        return NULL;
    }
    if (rdma_get_cm_event(conn->ec, &event) != 0) {
        LOG_ERROR("rdma_get_cm_event: %s", strerror(errno));
        return NULL;
    }
    if (event->event != expected) {
        LOG_ERROR("Unexpected event: got %s (%d), want %s (%d)",
                  rdma_event_str(event->event), event->event,
                  rdma_event_str(expected), expected);
        rdma_ack_cm_event(event);
        return NULL;
    }
    return event;
}

#define CLEANUP_RESOURCES() do {                               \
    if (conn->cm_id && conn->cm_id->qp) rdma_destroy_qp(conn->cm_id); \
    if (conn->send_mr) ibv_dereg_mr(conn->send_mr);           \
    if (conn->recv_mr) ibv_dereg_mr(conn->recv_mr);           \
    free(conn->send_buf); free(conn->recv_buf);                 \
    if (conn->cq)           ibv_destroy_cq(conn->cq);         \
    if (conn->comp_channel)  ibv_destroy_comp_channel(conn->comp_channel); \
    if (conn->pd)           ibv_dealloc_pd(conn->pd);        \
    conn->send_mr = NULL; conn->recv_mr = NULL;               \
    conn->send_buf = NULL; conn->recv_buf = NULL;             \
    conn->cq = NULL; conn->comp_channel = NULL; conn->pd = NULL; \
    if (conn->cm_id) { rdma_destroy_id(conn->cm_id); conn->cm_id = NULL; } \
} while (0)

rc_code_t client_connect(rdma_conn_t *conn, const char *ip, int port) {
    if (!conn) return RC_INVALID;

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = inet_addr(ip)
    };
    rc_code_t rc;
    struct rdma_cm_event *event = NULL;

    conn->state = CONN_STATE_CONNECTING;

    rc = create_cm_id(conn);             if (rc != RC_OK) return rc;
    rc = create_pd(conn);                 if (rc != RC_OK) goto fail;
    rc = create_comp_channel(conn);       if (rc != RC_OK) goto fail;
    rc = create_cq(conn);                 if (rc != RC_OK) goto fail;
    rc = create_mr(conn);                 if (rc != RC_OK) goto fail;
    rc = create_qp(conn);                 if (rc != RC_OK) goto fail;
    rc = req_notify_cq(conn);             if (rc != RC_OK) goto fail;
    rc = post_recv(conn);                 if (rc != RC_OK) goto fail;

    /* 地址解析 */
    if (rdma_resolve_addr(conn->cm_id, NULL, (struct sockaddr *)&addr, 2000) != 0) {
        LOG_ERROR("rdma_resolve_addr: %s", strerror(errno)); rc = RC_RDMA_FAIL; goto fail;
    }
    event = wait_cm_event(conn, RDMA_CM_EVENT_ADDR_RESOLVED, 3000);
    if (!event) { rc = RC_TIMEOUT; goto fail; }
    rdma_ack_cm_event(event); event = NULL;

    /* 路由解析 */
    if (rdma_resolve_route(conn->cm_id, 2000) != 0) {
        LOG_ERROR("rdma_resolve_route: %s", strerror(errno)); rc = RC_RDMA_FAIL; goto fail;
    }
    event = wait_cm_event(conn, RDMA_CM_EVENT_ROUTE_RESOLVED, 3000);
    if (!event) { rc = RC_TIMEOUT; goto fail; }
    rdma_ack_cm_event(event); event = NULL;

    /* 连接 */
    if (rdma_connect(conn->cm_id, NULL) != 0) {
        LOG_ERROR("rdma_connect: %s", strerror(errno)); rc = RC_RDMA_FAIL; goto fail;
    }
    event = wait_cm_event(conn, RDMA_CM_EVENT_ESTABLISHED, 5000);
    if (!event) { rc = RC_TIMEOUT; goto fail; }
    rdma_ack_cm_event(event); event = NULL;

    conn->state = CONN_STATE_CONNECTED;
    LOG_INFO("Connected to %s:%d", ip, port);

    /* 交换 MR 信息 */
    rc = exchange_peer_info(conn);
    if (rc != RC_OK) goto fail;

    return RC_OK;

fail:
    CLEANUP_RESOURCES();
    return rc;
}

/* 从 stdin 解析命令并执行 */
static void interactive_loop(rdma_conn_t *conn) {
    char line[512];

    LOG_INFO("=== Commands ===");
    LOG_INFO("  text <msg>           - Send a text message");
    LOG_INFO("  write <data>          - RDMA Write <data> to peer");
    LOG_INFO("  read                 - RDMA Read from peer recv_buf");
    LOG_INFO("  info                 - Show peer MR info");
    LOG_INFO("  quit                 - Disconnect");
    LOG_INFO("=====================");

    while (conn->state == CONN_STATE_CONNECTED && conn->running) {
        struct epoll_event evts[2];
        int nfds = epoll_wait(conn->epoll_fd, evts, 2, 200);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait: %s", strerror(errno)); break;
        }

        for (int i = 0; i < nfds; i++) {
            if (evts[i].data.ptr == conn) {
                int n = poll_cq(conn);
                if (n < 0) { conn->state = CONN_STATE_DISCONNECTED; break; }
            }
        }

        /* 读取命令 */
        ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        line[n] = '\0';
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        /* 解析命令 */
        if (strcmp(line, "quit") == 0) {
            LOG_INFO("Disconnecting...");
            break;
        } else if (strcmp(line, "info") == 0) {
            LOG_INFO("--- Peer Info ---");
            LOG_INFO("  peer_ready = %s", conn->peer_ready ? "YES" : "NO");
            LOG_INFO("  peer_addr  = 0x%lx", conn->peer_addr);
            LOG_INFO("  peer_rkey  = 0x%x", conn->peer_rkey);
            LOG_INFO("  peer_lkey  = 0x%x", conn->peer_lkey);
            LOG_INFO("--- Local MR ---");
            LOG_INFO("  send_buf   = %p", (void *)conn->send_buf);
            LOG_INFO("  send_mr    : lkey=0x%x rkey=0x%x",
                     conn->send_mr->lkey, conn->send_mr->rkey);
            LOG_INFO("  recv_mr    : lkey=0x%x", conn->recv_mr->lkey);

        } else if (strncmp(line, "write ", 6) == 0) {
            const char *data = line + 6;
            if (!conn->peer_ready) {
                LOG_WARN("Peer not ready (wait for MSG_TYPE_PEER_INFO)");
                continue;
            }
            /* RDMA Write: 把数据写入 peer 的 recv_buf */
            rc_code_t rc = do_rdma_write(conn, conn->peer_addr,
                                         conn->peer_rkey,
                                         data, strlen(data));
            if (rc != RC_OK) LOG_ERROR("RDMA Write failed: %s", rc_to_str(rc));

        } else if (strncmp(line, "read", 3) == 0) {
            if (!conn->peer_ready) {
                LOG_WARN("Peer not ready");
                continue;
            }
            /* RDMA Read: 从 peer 的 recv_buf 读到本地 send_buf */
            rc_code_t rc = do_rdma_read(conn, conn->peer_addr,
                                         conn->peer_rkey,
                                         conn->send_buf, MAX_MSG_SIZE);
            if (rc != RC_OK) LOG_ERROR("RDMA Read failed: %s", rc_to_str(rc));
            else LOG_INFO("RDMA Read issued (check completion for result)");

        } else if (strncmp(line, "text ", 5) == 0) {
            const char *msg = line + 5;
            rc_code_t rc = send_text(conn, msg);
            if (rc != RC_OK) LOG_ERROR("send_text failed: %s", rc_to_str(rc));

        } else {
            LOG_INFO("Unknown command. Type 'info', 'write <data>', 'read', 'text <msg>', or 'quit'");
        }
    }
}

rc_code_t run_client(const char *ip, int port) {
    rdma_conn_t conn;
    rc_code_t rc;

    rdma_conn_init(&conn, false);

    rc = create_event_channel(&conn);
    if (rc != RC_OK) goto done;

    rc = client_connect(&conn, ip, port);
    if (rc != RC_OK) goto done;

    rc = init_epoll(&conn);
    if (rc != RC_OK) goto done;

    /* 设置 stdin 非阻塞 */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    interactive_loop(&conn);

    fcntl(STDIN_FILENO, F_SETFL, flags);

done:
    rdma_conn_destroy(&conn);
    return rc;
}
