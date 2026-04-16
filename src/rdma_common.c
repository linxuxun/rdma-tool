#include "rdma_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

/*==============================================================================
 * 全局日志级别（main.c 设置）
 *============================================================================*/
log_level_t g_log_level = LOG_INFO;

/*==============================================================================
 * 日志实现
 *============================================================================*/
static const char *level_str_arr[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO]  = "INFO ",
    [LOG_WARN]  = "WARN ",
    [LOG_ERROR] = "ERROR"
};

void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < g_log_level) return;

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(stderr, "[%s] [%s] ", time_buf, level_str_arr[level]);

    va_list ap;
    va_start(&ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(&ap);

    fprintf(stderr, "\n");
}

const char *rc_to_str(rc_code_t rc) {
    switch (rc) {
        case RC_OK:           return "OK";
        case RC_INVALID:      return "Invalid argument";
        case RC_MALLOC_FAIL:  return "Memory allocation failed";
        case RC_RDMA_FAIL:    return "RDMA CM operation failed";
        case RC_IBV_FAIL:     return "RDMA Verbs operation failed";
        case RC_EVENT_FAIL:   return "RDMA CM event failed";
        case RC_TIMEOUT:      return "Timeout";
        case RC_PEER_NOT_READY: return "Peer not ready (no lkey exchanged yet)";
        default:              return "Unknown error";
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
rc_code_t rdma_conn_init(rdma_conn_t *conn, bool is_server) {
    if (!conn) return RC_INVALID;
    memset(conn, 0, sizeof(*conn));
    conn->state     = CONN_STATE_INIT;
    conn->is_server = is_server;
    conn->epoll_fd  = -1;
    conn->running   = 1;
    return RC_OK;
}

/*==============================================================================
 * 资源创建顺序函数
 *============================================================================*/
rc_code_t create_event_channel(rdma_conn_t *conn) {
    if (!conn) return RC_INVALID;
    conn->ec = rdma_create_event_channel();
    if (!conn->ec) {
        LOG_ERROR("rdma_create_event_channel: %s", strerror(errno));
        return RC_RDMA_FAIL;
    }
    return RC_OK;
}

rc_code_t create_listen_id(rdma_conn_t *conn) {
    if (!conn || !conn->ec) return RC_INVALID;
    int ret = rdma_create_id(conn->ec, &conn->listen_id, conn, RDMA_PS_TCP);
    if (ret) {
        LOG_ERROR("rdma_create_id (listen): %s", strerror(errno));
        return RC_RDMA_FAIL;
    }
    return RC_OK;
}

rc_code_t create_cm_id(rdma_conn_t *conn) {
    if (!conn || !conn->ec) return RC_INVALID;
    int ret = rdma_create_id(conn->ec, &conn->cm_id, conn, RDMA_PS_TCP);
    if (ret) {
        LOG_ERROR("rdma_create_id (cm): %s", strerror(errno));
        return RC_RDMA_FAIL;
    }
    return RC_OK;
}

rc_code_t create_pd(rdma_conn_t *conn) {
    if (!conn || !conn->cm_id) return RC_INVALID;
    conn->pd = ibv_alloc_pd(conn->cm_id->verbs);
    if (!conn->pd) {
        LOG_ERROR("ibv_alloc_pd: %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

rc_code_t create_comp_channel(rdma_conn_t *conn) {
    if (!conn || !conn->cm_id) return RC_INVALID;
    conn->comp_channel = ibv_create_comp_channel(conn->cm_id->verbs);
    if (!conn->comp_channel) {
        LOG_ERROR("ibv_create_comp_channel: %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

rc_code_t create_cq(rdma_conn_t *conn) {
    if (!conn || !conn->cm_id || !conn->comp_channel) return RC_INVALID;
    conn->cq = ibv_create_cq(conn->cm_id->verbs, MAX_WR * 2, conn,
                              conn->comp_channel, 0);
    if (!conn->cq) {
        LOG_ERROR("ibv_create_cq: %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

rc_code_t create_mr(rdma_conn_t *conn) {
    if (!conn || !conn->pd) return RC_INVALID;

    conn->send_buf = malloc(BUFFER_SIZE);
    conn->recv_buf = malloc(BUFFER_SIZE);
    if (!conn->send_buf || !conn->recv_buf) {
        LOG_ERROR("malloc buffers failed");
        return RC_MALLOC_FAIL;
    }
    memset(conn->send_buf, 0, BUFFER_SIZE);
    memset(conn->recv_buf, 0, BUFFER_SIZE);

    /*
     * send_mr: 本地写 + 远端读 + 远端写
     *   - 远端读  → 对端 RDMA Read 我方 send_buf
     *   - 远端写  → 对端 RDMA Write 我方 send_buf
     */
    conn->send_mr = ibv_reg_mr(conn->pd, conn->send_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE
                                | IBV_ACCESS_REMOTE_READ
                                | IBV_ACCESS_REMOTE_WRITE);
    if (!conn->send_mr) {
        LOG_ERROR("ibv_reg_mr (send): %s", strerror(errno));
        return RC_IBV_FAIL;
    }

    /*
     * recv_mr: 本地写
     *   收到的控制消息（lkey交换、RDMA请求）放在 recv_buf
     */
    conn->recv_mr = ibv_reg_mr(conn->pd, conn->recv_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    if (!conn->recv_mr) {
        LOG_ERROR("ibv_reg_mr (recv): %s", strerror(errno));
        return RC_IBV_FAIL;
    }

    LOG_INFO("MR registered: send lkey=%u, recv lkey=%u",
             conn->send_mr->lkey, conn->recv_mr->lkey);
    return RC_OK;
}

rc_code_t create_qp(rdma_conn_t *conn) {
    if (!conn || !conn->cm_id || !conn->pd) return RC_INVALID;

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = conn->cq,
        .recv_cq = conn->cq,
        .cap = {
            .max_send_wr = MAX_WR,
            .max_recv_wr = MAX_WR,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1
    };

    int ret = rdma_create_qp(conn->cm_id, conn->pd, &qp_attr);
    if (ret) {
        LOG_ERROR("rdma_create_qp: %s", strerror(errno));
        return RC_RDMA_FAIL;
    }
    return RC_OK;
}

rc_code_t req_notify_cq(rdma_conn_t *conn) {
    if (!conn || !conn->cq) return RC_INVALID;
    if (ibv_req_notify_cq(conn->cq, 0)) {
        LOG_ERROR("ibv_req_notify_cq: %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

rc_code_t post_recv(rdma_conn_t *conn) {
    if (!conn || !conn->qp || !conn->recv_mr) return RC_INVALID;

    struct ibv_recv_wr recv_wr = {0};
    struct ibv_sge sge = {0};
    struct ibv_recv_wr *bad_wr = NULL;

    sge.addr   = (uint64_t)conn->recv_buf;
    sge.length = BUFFER_SIZE;
    sge.lkey   = conn->recv_mr->lkey;

    recv_wr.wr_id   = conn->recv_count++;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    int ret = ibv_post_recv(conn->qp, &recv_wr, &bad_wr);
    if (ret) {
        LOG_ERROR("ibv_post_recv: %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

/*==============================================================================
 * 交换对端 MR 信息（发送自己的 lkey/addr/rkey）
 *============================================================================*/
rc_code_t exchange_peer_info(rdma_conn_t *conn) {
    if (!conn || !conn->send_mr) return RC_INVALID;

    msg_header_t hdr = {
        .type         = MSG_TYPE_PEER_INFO,
        .len          = 0,
        .remote_addr  = (uint64_t)conn->send_buf,
        .remote_rkey  = conn->send_mr->rkey,
    };

    memcpy(conn->send_buf, &hdr, sizeof(hdr));

    struct ibv_send_wr send_wr = {0};
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->send_buf,
        .length = sizeof(hdr),
        .lkey   = conn->send_mr->lkey
    };

    send_wr.wr_id      = conn->send_count++;
    send_wr.sg_list    = &sge;
    send_wr.num_sge   = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(conn->qp, &send_wr, &bad) != 0) {
        LOG_ERROR("ibv_post_send (peer_info): %s", strerror(errno));
        return RC_IBV_FAIL;
    }

    LOG_INFO("Sent MY MR info: addr=0x%lx, rkey=0x%x, lkey=0x%x",
             (uint64_t)conn->send_buf,
             conn->send_mr->rkey,
             conn->send_mr->lkey);
    return RC_OK;
}

/*==============================================================================
 * RDMA 单边写（把本地数据写入对端 recv_buf）
 *============================================================================*/
rc_code_t do_rdma_write(rdma_conn_t *conn,
                         uint64_t remote_addr, uint32_t remote_rkey,
                         const void *local_data, size_t len) {
    if (!conn || !conn->qp || !conn->send_mr) return RC_INVALID;
    if (len >= MAX_MSG_SIZE) {
        LOG_WARN("RDMA write data too long: %zu >= %d", len, MAX_MSG_SIZE);
        return RC_INVALID;
    }
    if (!conn->peer_ready) {
        LOG_ERROR("Peer not ready (lkey not received yet)");
        return RC_PEER_NOT_READY;
    }

    /* 复制数据到 send_buf */
    memcpy(conn->send_buf, local_data, len);

    struct ibv_send_wr send_wr = {0};
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->send_buf,
        .length = len,
        .lkey   = conn->send_mr->lkey
    };

    send_wr.wr_id      = conn->send_count++;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_RDMA_WRITE;    /* ← 单边写 */
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.rdma.rkey  = remote_rkey;
    send_wr.rdma.remote_addr = remote_addr;

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(conn->qp, &send_wr, &bad) != 0) {
        LOG_ERROR("ibv_post_send (RDMA write): %s", strerror(errno));
        return RC_IBV_FAIL;
    }

    LOG_INFO("RDMA Write issued: %zu bytes -> 0x%lx (rkey=0x%x)",
             len, remote_addr, remote_rkey);
    return RC_OK;
}

/*==============================================================================
 * RDMA 单边读（从对端 recv_buf 读到本地 send_buf）
 *============================================================================*/
rc_code_t do_rdma_read(rdma_conn_t *conn,
                        uint64_t remote_addr, uint32_t remote_rkey,
                        void *local_buf, size_t len) {
    if (!conn || !conn->qp || !conn->send_mr) return RC_INVALID;
    if (len >= MAX_MSG_SIZE) {
        LOG_WARN("RDMA read data too long: %zu >= %d", len, MAX_MSG_SIZE);
        return RC_INVALID;
    }
    if (!conn->peer_ready) {
        LOG_ERROR("Peer not ready (lkey not received yet)");
        return RC_PEER_NOT_READY;
    }

    /* 读回来的数据暂存到 send_buf */
    struct ibv_send_wr send_wr = {0};
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->send_buf,
        .length = len,
        .lkey   = conn->send_mr->lkey
    };

    send_wr.wr_id      = conn->send_count++;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_RDMA_READ;     /* ← 单边读 */
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.rdma.rkey  = remote_rkey;
    send_wr.rdma.remote_addr = remote_addr;

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(conn->qp, &send_wr, &bad) != 0) {
        LOG_ERROR("ibv_post_send (RDMA read): %s", strerror(errno));
        return RC_IBV_FAIL;
    }

    LOG_INFO("RDMA Read issued: %zu bytes <- 0x%lx (rkey=0x%x)",
             len, remote_addr, remote_rkey);
    return RC_OK;
}

/*==============================================================================
 * 应用层消息发送（带消息头）
 *============================================================================*/
rc_code_t send_msg(rdma_conn_t *conn, msg_type_t type,
                    const void *payload, size_t len) {
    if (!conn || !conn->qp || !conn->send_mr) return RC_INVALID;
    if (len > MAX_MSG_SIZE) {
        LOG_WARN("Payload too long: %zu > %d", len, MAX_MSG_SIZE);
        return RC_INVALID;
    }

    msg_header_t hdr = {
        .type        = type,
        .len         = (uint32_t)len,
        .remote_addr = 0,
        .remote_rkey = 0,
    };
    if (payload && len > 0)
        memcpy(hdr.payload, payload, len);

    memcpy(conn->send_buf, &hdr, sizeof(hdr));

    struct ibv_send_wr send_wr = {0};
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->send_buf,
        .length = sizeof(hdr),
        .lkey   = conn->send_mr->lkey
    };

    send_wr.wr_id      = conn->send_count++;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(conn->qp, &send_wr, &bad) != 0) {
        LOG_ERROR("ibv_post_send (send_msg): %s", strerror(errno));
        return RC_IBV_FAIL;
    }
    return RC_OK;
}

rc_code_t send_text(rdma_conn_t *conn, const char *text) {
    return send_msg(conn, MSG_TYPE_TEXT, text, strlen(text));
}

/*==============================================================================
 * 解析 recv_buf 中的消息
 *============================================================================*/
void handle_recv_buffer(rdma_conn_t *conn) {
    if (!conn || !conn->recv_buf) return;

    msg_header_t hdr;
    memcpy(&hdr, conn->recv_buf, sizeof(hdr));

    switch (hdr.type) {

        case MSG_TYPE_PEER_INFO:
            conn->peer_addr = hdr.remote_addr;
            conn->peer_rkey = hdr.remote_rkey;
            conn->peer_lkey = (uint32_t)(hdr.remote_addr >> 32); /* 解码 */
            /* 实际 lkey 在 recv_mr 的 lkey，由对方在 recv_mr 上注册 recv_buf */
            /* 对方用 recv_mr 的 lkey = recv_mr->lkey，我们用 peer_lkey */
            /* 为了简化：lkey = recv_mr->lkey，由对方告知 */
            LOG_INFO("Received peer MR info: addr=0x%lx, rkey=0x%x",
                     hdr.remote_addr, hdr.remote_rkey);
            LOG_INFO("  (peer will use our recv_mr lkey=%u for RDMA to us)",
                     conn->recv_mr->lkey);
            conn->peer_ready = true;
            break;

        case MSG_TYPE_TEXT:
            LOG_INFO("RECV TEXT: %.*s", hdr.len, hdr.payload);
            break;

        case MSG_TYPE_RDMA_WRITE:
            LOG_INFO(">>> Peer requests RDMA Write: %u bytes to our recv_buf",
                     hdr.len);
            break;

        case MSG_TYPE_RDMA_READ:
            LOG_INFO(">>> Peer requests RDMA Read: %u bytes from our recv_buf",
                     hdr.len);
            break;

        case MSG_TYPE_RDMA_RESULT:
            LOG_INFO("RECV RDMA RESULT: %.*s", hdr.len, hdr.payload);
            break;

        default:
            LOG_WARN("Unknown MSG type: %d", hdr.type);
            break;
    }
}

/*==============================================================================
 * CQ 轮询
 *============================================================================*/
int poll_cq(rdma_conn_t *conn) {
    if (!conn || !conn->cq || !conn->comp_channel) return -1;

    struct ibv_cq *ev_cq;
    void *ev_ctx;

    if (ibv_get_cq_event(conn->comp_channel, &ev_cq, &ev_ctx) != 0)
        return 0; /* EINTR 等，不算错误 */

    ibv_ack_cq_events(ev_cq, 1);

    if (ibv_req_notify_cq(ev_cq, 0)) {
        LOG_ERROR("ibv_req_notify_cq: %s", strerror(errno));
        return -1;
    }

    struct ibv_wc wc;
    int completed = 0;

    while (1) {
        int ret = ibv_poll_cq(ev_cq, 1, &wc);
        if (ret < 0) {
            LOG_ERROR("ibv_poll_cq: %s", strerror(errno));
            return -1;
        }
        if (ret == 0) break;

        if (wc.status != IBV_WC_SUCCESS) {
            LOG_WARN("Bad WC: status=%d opcode=%d", wc.status, wc.opcode);
            continue;
        }

        if (wc.opcode & IBV_WC_RECV) {
            handle_recv_buffer(conn);
            post_recv(conn);  /* 重新发布 */
        } else {
            /* 发送完成 / RDMA 完成 */
            if (wc.opcode == IBV_WC_RDMA_READ) {
                /* RDMA Read 完成，结果在 send_buf */
                LOG_INFO("RDMA Read completed, result: %.*s",
                          (int)wc.byte_len,
                          conn->send_buf);
            } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
                LOG_INFO("RDMA Write completed");
            }
        }
        completed++;
    }

    return completed;
}

/*==============================================================================
 * epoll 初始化（监控 CQ comp_channel fd）
 *============================================================================*/
rc_code_t init_epoll(rdma_conn_t *conn) {
    if (!conn || !conn->comp_channel) return RC_INVALID;

    conn->epoll_fd = epoll_create1(0);
    if (conn->epoll_fd < 0) {
        LOG_ERROR("epoll_create1: %s", strerror(errno));
        return RC_INVALID;
    }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = conn
    };

    if (epoll_ctl(conn->epoll_fd, EPOLL_CTL_ADD,
                  conn->comp_channel->fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl ADD comp_channel: %s", strerror(errno));
        return RC_INVALID;
    }

    LOG_DEBUG("epoll fd=%d, comp_channel fd=%d",
              conn->epoll_fd, conn->comp_channel->fd);
    return RC_OK;
}

/*==============================================================================
 * 事件循环（通用）
 *============================================================================*/
void event_loop(rdma_conn_t *conn) {
    if (!conn) return;

    struct epoll_event events[MAX_EPOLL_EVENTS];
    LOG_INFO("=== Event loop started (Ctrl+C to quit) ===");

    while (conn->state == CONN_STATE_CONNECTED && conn->running) {
        int nfds = epoll_wait(conn->epoll_fd, events,
                               MAX_EPOLL_EVENTS, 200);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == conn) {
                int n = poll_cq(conn);
                if (n < 0) {
                    LOG_WARN("poll_cq error, exiting loop");
                    conn->state = CONN_STATE_DISCONNECTED;
                }
            }
        }
    }

    LOG_INFO("=== Event loop ended ===");
}

/*==============================================================================
 * 资源释放（逆序）
 *============================================================================*/
void rdma_conn_destroy(rdma_conn_t *conn) {
    if (!conn) return;

    if (conn->cm_id && conn->state != CONN_STATE_DISCONNECTED) {
        rdma_disconnect(conn->cm_id);
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(conn->ec, &event) == 0) {
            if (event->event == RDMA_CM_EVENT_DISCONNECTED)
                rdma_ack_cm_event(event);
        }
        conn->state = CONN_STATE_DISCONNECTED;
    }

    if (conn->cm_id && conn->cm_id->qp) rdma_destroy_qp(conn->cm_id);

    if (conn->send_mr) ibv_dereg_mr(conn->send_mr);
    if (conn->recv_mr) ibv_dereg_mr(conn->recv_mr);
    free(conn->send_buf);
    free(conn->recv_buf);
    conn->send_buf = NULL;
    conn->recv_buf = NULL;

    if (conn->comp_channel) ibv_destroy_comp_channel(conn->comp_channel);
    if (conn->cq)           ibv_destroy_cq(conn->cq);
    if (conn->pd)           ibv_dealloc_pd(conn->pd);

    if (conn->cm_id)     rdma_destroy_id(conn->cm_id);
    if (conn->listen_id) rdma_destroy_id(conn->listen_id);

    if (conn->ec) rdma_destroy_event_channel(conn->ec);

    if (conn->epoll_fd >= 0) close(conn->epoll_fd);

    LOG_INFO("All resources cleaned up");
}
