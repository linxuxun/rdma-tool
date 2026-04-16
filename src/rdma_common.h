#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

/*==============================================================================
 * 常量配置
 *============================================================================*/
#define BUFFER_SIZE     4096
#define MAX_WR          100
#define MAX_EPOLL_EVENTS 64
#define DEFAULT_PORT    12345
#define DEFAULT_IP      "0.0.0.0"
#define DEFAULT_SERVER  "127.0.0.1"
#define MAX_MSG_SIZE    (BUFFER_SIZE - 32)

/*==============================================================================
 * 日志级别
 *============================================================================*/
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

extern log_level_t g_log_level;

#define LOG_DEBUG(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __VA_ARGS__)

/*==============================================================================
 * 连接状态 & 消息类型
 *============================================================================*/
typedef enum {
    CONN_STATE_INIT,
    CONN_STATE_CONNECTING,
    CONN_STATE_CONNECTED,
    CONN_STATE_DISCONNECTED
} conn_state_t;

/*==============================================================================
 * 应用层消息类型（用于 send_buf 交互）
 *============================================================================*/
typedef enum {
    /* 控制消息 */
    MSG_TYPE_NONE        = 0,
    MSG_TYPE_PEER_INFO   = 1,  /* 交换 lkey/addr/rkey */
    MSG_TYPE_TEXT        = 2,  /* 普通文本消息 */

    /* RDMA 操作请求（收到后自动执行） */
    MSG_TYPE_RDMA_WRITE  = 3,  /* 请对我执行 RDMA Write */
    MSG_TYPE_RDMA_READ   = 4,  /* 请对我执行 RDMA Read  */

    /* RDMA 操作结果通知 */
    MSG_TYPE_RDMA_RESULT = 5   /* RDMA Write/Read 完成通知 */
} msg_type_t;

/*==============================================================================
 * 消息头（紧接在 send_buf/recv_buf 头部）
 *============================================================================*/
typedef struct __attribute__((packed)) {
    msg_type_t type;
    uint32_t   len;        /* 正文长度 */
    uint64_t   remote_addr;/* RDMA Write/Read 目标地址 */
    uint32_t   remote_rkey;/* RDMA 操作的对端 rkey */
    char       payload[MAX_MSG_SIZE];
} msg_header_t;

_Static_assert(sizeof(msg_header_t) <= BUFFER_SIZE, "msg_header_t too large");

/*==============================================================================
 * RDMA 连接上下文
 *============================================================================*/
typedef struct rdma_conn {
    /* 控制平面 */
    struct rdma_event_channel *ec;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;

    /* 数据平面 */
    struct ibv_pd         *pd;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq         *cq;
    struct ibv_qp         *qp;
    struct ibv_mr         *send_mr;
    struct ibv_mr         *recv_mr;

    /* 缓冲区（recv_buf 兼作消息接收） */
    char *send_buf;
    char *recv_buf;

    /* 交换后的对端信息 */
    uint64_t peer_addr;
    uint32_t peer_rkey;
    uint32_t peer_lkey;
    bool     peer_ready;   /* 对端 lkey/addr 是否已收到 */

    /* 状态 */
    bool          is_server;
    conn_state_t  state;
    volatile sig_atomic_t running;
    uint64_t      send_count;
    uint64_t      recv_count;

    /* epoll */
    int           epoll_fd;
} rdma_conn_t;

/*==============================================================================
 * 错误码
 *============================================================================*/
typedef enum {
    RC_OK          =  0,
    RC_INVALID     = -1,
    RC_MALLOC_FAIL = -2,
    RC_RDMA_FAIL   = -3,
    RC_IBV_FAIL    = -4,
    RC_EVENT_FAIL  = -5,
    RC_TIMEOUT     = -6,
    RC_PEER_NOT_READY = -7
} rc_code_t;

/*==============================================================================
 * 函数原型
 *============================================================================*/

/* 日志 */
void       log_msg(log_level_t level, const char *fmt, ...);
const char *rc_to_str(rc_code_t rc);

/* 初始化 / 销毁 */
rc_code_t  rdma_conn_init(rdma_conn_t *conn, bool is_server);
void       rdma_conn_destroy(rdma_conn_t *conn);

/* 资源创建（按正确顺序） */
rc_code_t  create_event_channel(rdma_conn_t *conn);
rc_code_t  create_listen_id(rdma_conn_t *conn);
rc_code_t  create_cm_id(rdma_conn_t *conn);
rc_code_t  create_pd(rdma_conn_t *conn);
rc_code_t  create_comp_channel(rdma_conn_t *conn);
rc_code_t  create_cq(rdma_conn_t *conn);
rc_code_t  create_mr(rdma_conn_t *conn);
rc_code_t  create_qp(rdma_conn_t *conn);
rc_code_t  req_notify_cq(rdma_conn_t *conn);
rc_code_t  post_recv(rdma_conn_t *conn);

/* 交换对端 MR 信息（连接建立后调用一次） */
rc_code_t  exchange_peer_info(rdma_conn_t *conn);

/* RDMA 单边操作 */
rc_code_t  do_rdma_write(rdma_conn_t *conn,
                          uint64_t remote_addr, uint32_t remote_rkey,
                          const void *local_data, size_t len);
rc_code_t  do_rdma_read(rdma_conn_t *conn,
                         uint64_t remote_addr, uint32_t remote_rkey,
                         void *local_buf, size_t len);

/* 事件循环 */
rc_code_t  init_epoll(rdma_conn_t *conn);
void       event_loop(rdma_conn_t *conn);

/* CQ 轮询 */
int        poll_cq(rdma_conn_t *conn);

/* 应用层消息发送（带消息头） */
rc_code_t  send_msg(rdma_conn_t *conn, msg_type_t type,
                    const void *payload, size_t len);
rc_code_t  send_text(rdma_conn_t *conn, const char *text);

/* 解析 recv_buf 中的消息并处理 */
void       handle_recv_buffer(rdma_conn_t *conn);

#endif /* RDMA_COMMON_H */
