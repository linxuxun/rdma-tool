#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include "rdma_common.h"
#include "rdma_server.h"
#include "rdma_client.h"

/*==============================================================================
 * 全局退出标志
 *============================================================================*/
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/*==============================================================================
 * 帮助
 *============================================================================*/
static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  -s, --server       Run as server (default: client)\n");
    printf("  -i, --ip <addr>    Bind/listen IP (default: 0.0.0.0 server, 127.0.0.1 client)\n");
    printf("  -p, --port <port>  Port (default: %d)\n", DEFAULT_PORT);
    printf("  -l, --log <level>  Log: debug|info|warn|error (default: info)\n");
    printf("  -h, --help         Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -s -i 0.0.0.0 -p 12345          # Server\n", prog);
    printf("  %s -c -i 127.0.0.1 -p 12345        # Client\n", prog);
    printf("  %s -c -i 127.0.0.1 -p 12345 -l debug\n", prog);
}

int main(int argc, char *argv[]) {
    int is_server = 0;
    const char *ip = NULL;
    int port = DEFAULT_PORT;

    static struct option long_opts[] = {
        {"server", no_argument,       0, 's'},
        {"client", no_argument,       0, 'c'},
        {"ip",     required_argument, 0, 'i'},
        {"port",   required_argument, 0, 'p'},
        {"log",    required_argument, 0, 'l'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "sci:p:l:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': is_server = 1; break;
            case 'c': is_server = 0; break;
            case 'i': ip = optarg;  break;
            case 'p': port = atoi(optarg); break;
            case 'l':
                if      (!strcmp(optarg, "debug")) g_log_level = LOG_DEBUG;
                else if (!strcmp(optarg, "warn"))  g_log_level = LOG_WARN;
                else if (!strcmp(optarg, "error")) g_log_level = LOG_ERROR;
                else                               g_log_level = LOG_INFO;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    if (!ip)
        ip = is_server ? DEFAULT_IP : DEFAULT_SERVER;

    /* 信号处理 */
    struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    LOG_INFO("RDMA %s starting — IP=%s Port=%d",
             is_server ? "Server" : "Client", ip, port);

    rc_code_t rc;
    if (is_server) {
        rc = run_server(ip, port);
    } else {
        rc = run_client(ip, port);
    }

    if (rc != RC_OK) {
        LOG_ERROR("Exited with error: %s (rc=%d)", rc_to_str(rc), rc);
        return 1;
    }

    LOG_INFO("Exited cleanly");
    return 0;
}
