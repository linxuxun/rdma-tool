#ifndef RDMA_CLIENT_H
#define RDMA_CLIENT_H

#include "rdma_common.h"

/* 客户端：连接到服务器 */
rc_code_t client_connect(rdma_conn_t *conn, const char *ip, int port);

/* 客户端主流程 */
rc_code_t run_client(const char *ip, int port);

#endif
