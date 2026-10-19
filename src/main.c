#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include "msg_queue.h"
#include "tcp_server.h"
#include "worker.h"
#include "http.h"
#include "db.h"

/*
 * 线程模型（谁负责什么，一张图说清）：
 *
 *   主线程        epoll 事件循环：收 ESP 上报 → 完整 JSON 推入队列
 *   worker 线程   消费队列：解析 JSON → 写 SQLite
 *   http 线程     浏览器查历史（读库）/ 下发命令（写 ESP 连接）
 *
 * 线程之间只通过两种东西交互：
 *   1. msg_queue（互斥锁 + 条件变量）—— 网络线程 → 工作线程
 *   2. SQLite（每线程独立连接 + WAL）—— 工作线程写、HTTP 线程读
 */

msg_queue_t g_queue;

/*
 * 信号处理：Ctrl+C(SIGINT) 或 kill(SIGTERM) 时只做两件
 * 异步信号安全的事——置停止标志、关监听 fd（close 是安全的），
 * 让两个阻塞中的线程自己醒来，清理工作留在 main 的正常流程里做。
 */
static void on_stop_signal(int sig)
{
    (void)sig;
    tcp_server_stop();
    http_server_stop();
}

/*
 * 网络层回调：每收到一条完整的 JSON 上报（已按花括号配对切好）就走这里。
 * 只做一件事——推入消息队列，解析和落库交给工作线程，
 * 保证网络线程永远不做耗时操作。
 */
static void on_esp_data(const char *client_ip, const char *json_obj)
{
    (void)client_ip;
    msg_queue_push(&g_queue, json_obj);
}

static void print_usage(const char *prog)
{
    printf("用法: %s [tcp端口] [http端口] [数据库文件]\n", prog);
    printf("默认: %s 9999 8080 fan.db\n", prog);
}

int main(int argc, char **argv)
{
    int tcp_port = 9999;
    int http_port = 8080;
    const char *db_path = "fan.db";

    if (argc >= 2)
    {
        if (strcmp(argv[1], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        tcp_port = atoi(argv[1]);
    }
    if (argc >= 3)
    {
        http_port = atoi(argv[2]);
    }
    if (argc >= 4)
    {
        db_path = argv[3];
    }

    /* SIGPIPE：向已断开的 socket 写数据时默认会杀死进程，必须忽略 */
    signal(SIGPIPE, SIG_IGN);

    /* Ctrl+C / kill：触发优雅退出而不是被默认行为直接杀死 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    msg_queue_init(&g_queue);

    if (db_open(db_path) != 0)
    {
        return 1;
    }
    if (tcp_server_init(tcp_port) != 0)
    {
        return 1;
    }

    pthread_t worker_tid;
    pthread_t http_tid;
    pthread_create(&worker_tid, NULL, worker_thread, NULL);
    pthread_create(&http_tid, NULL, http_thread, &http_port);

    printf("=== fan-iot-server 启动 ===\n");
    printf("  TCP  数据端口: %d  (ESP8266 连这里)\n", tcp_port);
    printf("  HTTP 控制端口: %d  (浏览器连这里)\n", http_port);
    printf("  按 Ctrl+C 退出\n");

    /* 主线程进入 epoll 事件循环，阻塞到 Ctrl+C */
    tcp_server_run(on_esp_data);

    /* ---- 优雅退出：按反序关停各层 ---- */
    printf("正在退出...\n");
    http_server_stop();
    msg_queue_shutdown(&g_queue);       /* 唤醒并通知 worker 退出 */
    pthread_join(worker_tid, NULL);
    pthread_join(http_tid, NULL);
    db_close();

    printf("=== 已退出 ===\n");
    return 0;
}
