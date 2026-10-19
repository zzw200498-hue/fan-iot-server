#include "http.h"
#include "tcp_server.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * 简易 HTTP 服务：阻塞 socket + poll 超时，够演示用。
 * 每个请求处理完就 close，不做 keep-alive——简单可靠优先。
 * 真要上生产应换 libmicrohttpd 之类的库，这是刻意的取舍。
 */

static volatile int g_http_running = 0;
static int g_http_listen_fd = -1;

static void send_response(int fd, const char *content_type, const char *body)
{
    char header[256];
    size_t body_len = strlen(body);

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     content_type, body_len);
    send(fd, header, (size_t)n, MSG_NOSIGNAL);
    send(fd, body, body_len, MSG_NOSIGNAL);
}

/* 内置网页：每 3 秒自动拉一次 /api/history 刷新表格，加三个控制按钮 */
static const char *INDEX_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>温控风扇监控</title>"
    "<style>body{font-family:sans-serif;margin:2em}"
    "table{border-collapse:collapse}td,th{border:1px solid #999;padding:4px 12px}"
    "button{margin-right:8px;padding:6px 16px}</style></head><body>"
    "<h2>温控风扇监控台</h2>"
    "<div>远程控制：<button onclick=\"cmd('on')\">强制开</button>"
    "<button onclick=\"cmd('off')\">强制关</button>"
    "<button onclick=\"cmd('auto')\">恢复自动</button>"
    "<span id='result'></span></div>"
    "<table id='tb'><tr><th>时间</th><th>温度</th><th>湿度</th><th>风扇%</th></tr></table>"
    "<script>"
    "function cmd(s){fetch('/api/fan?set='+s).then(r=>r.text()).then(t=>{"
    "document.getElementById('result').textContent=' 已发送: '+s;});}"
    "function refresh(){fetch('/api/history?n=15').then(r=>r.json()).then(d=>{"
    "let tb=document.getElementById('tb');"
    "while(tb.rows.length>1){tb.deleteRow(1);}"
    "for(let i=d.samples.length-1;i>=0;i--){let s=d.samples[i];"
    "let row=tb.insertRow(1);"
    "row.insertCell(0).textContent=new Date(s.ts*1000).toLocaleString();"
    "row.insertCell(1).textContent=s.temp.toFixed(1);"
    "row.insertCell(2).textContent=s.humi.toFixed(1);"
    "row.insertCell(3).textContent=s.fan;}});}"
    "setInterval(refresh,3000);refresh();"
    "</script></body></html>";

static void handle_request(int fd, const char *request)
{
    char method[16] = {0};
    char path[256] = {0};

    /* 只解析请求行：GET /path HTTP/1.1 */
    if (sscanf(request, "%15s %255s", method, path) != 2)
    {
        send_response(fd, "text/plain", "bad request");
        return;
    }

    if (strcmp(method, "GET") != 0)
    {
        send_response(fd, "text/plain", "method not allowed");
        return;
    }

    if (strncmp(path, "/api/history", 12) == 0)
    {
        /* 默认取最近 10 条；带 ?n=20 就取 20 条 */
        int n = 10;
        const char *q = strstr(path, "n=");
        if (q != NULL)
        {
            n = atoi(q + 2);
            if (n < 1)
            {
                n = 1;
            }
            if (n > 500)
            {
                n = 500;
            }
        }
        char body[16384];
        db_query_recent(n, body, sizeof(body));
        send_response(fd, "application/json", body);
        return;
    }

    if (strncmp(path, "/api/fan", 8) == 0)
    {
        const char *cmd = "";
        if (strstr(path, "set=on") != NULL)
        {
            cmd = "FAN=ON\r\n";
        }
        else if (strstr(path, "set=off") != NULL)
        {
            cmd = "FAN=OFF\r\n";
        }
        else if (strstr(path, "set=auto") != NULL)
        {
            cmd = "FAN=AUTO\r\n";
        }

        if (cmd[0] == '\0')
        {
            send_response(fd, "application/json", "{\"error\":\"bad set\"}");
            return;
        }
        tcp_server_send_to_esp(cmd);
        send_response(fd, "application/json", "{\"ok\":true}");
        return;
    }

    if (strncmp(path, "/api", 4) == 0 || strcmp(path, "/") == 0)
    {
        send_response(fd, "text/html", INDEX_HTML);
        return;
    }

    send_response(fd, "text/plain", "not found");
}

void http_server_stop(void)
{
    g_http_running = 0;
    /* close 监听 fd 让阻塞在 poll 的线程立刻醒来退出 */
    if (g_http_listen_fd != -1)
    {
        close(g_http_listen_fd);
        g_http_listen_fd = -1;
    }
}

void *http_thread(void *arg)
{
    int port = *(int *)arg;

    signal(SIGPIPE, SIG_IGN);       /* 客户端提前断开时别让进程被杀 */

    g_http_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_http_listen_fd < 0)
    {
        perror("[http] socket");
        return NULL;
    }

    int reuse = 1;
    setsockopt(g_http_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(g_http_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("[http] bind");
        return NULL;
    }
    if (listen(g_http_listen_fd, 8) < 0)
    {
        perror("[http] listen");
        return NULL;
    }

    printf("[http] 监听端口 %d  (浏览器打开 http://<本机IP>:%d)\n", port, port);
    g_http_running = 1;

    while (g_http_running)
    {
        /* poll 带超时：既不空转，又能在退出时及时醒来 */
        struct pollfd pfd;
        pfd.fd = g_http_listen_fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0)
        {
            continue;
        }

        int fd = accept(g_http_listen_fd, NULL, NULL);
        if (fd < 0)
        {
            continue;
        }

        /* 读请求头（固定 4KB，浏览器普通请求足够） */
        char request[4096];
        memset(request, 0, sizeof(request));
        size_t total = 0;
        while (total < sizeof(request) - 1)
        {
            ssize_t n = recv(fd, request + total, sizeof(request) - 1 - total, 0);
            if (n <= 0)
            {
                break;
            }
            total = total + (size_t)n;
            /* 请求头以空行结束，收到就够用了 */
            if (strstr(request, "\r\n\r\n") != NULL)
            {
                break;
            }
        }

        handle_request(fd, request);
        close(fd);
    }

    printf("[http] 退出\n");
    return NULL;
}
