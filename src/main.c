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
    tcp_server_run(NULL);

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
