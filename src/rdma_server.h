#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

#include "rdma_common.h"
#include <arpa/inet.h>

/*==============================================================================
 * 服务端入口
 *============================================================================*/

/* 服务端：绑定并监听 */
rc_code_t server_bind_and_listen(rdma_conn_t *conn,
                                  const char *ip, int port);

/* 服务端：等待并接受一个连接（会创建所有数据平面资源） */
rc_code_t server_accept(rdma_conn_t *conn);

/* 服务端主流程 */
rc_code_t run_server(const char *ip, int port);

#endif /* RDMA_SERVER_H */
