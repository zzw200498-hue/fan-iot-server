#include "tcp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>

/*
 * 实现要点：
 * - epoll 水平触发模式，逻辑直观：哪个 fd 有数据就读哪个
 * - 每个 fd 的接收数据留在各自的缓冲区里，按花括号配对切出
 *   完整 JSON 对象再回调——解决 TCP 粘包/半包问题
 * - 第一个连上来的客户端视作 ESP8266，下行命令发给它
 * - 收发全程不碰数据库、不做解析耗时操作（这些在工作线程做）
 */

#define MAX_CLIENTS 16
#define CONN_BUF_SIZE 4096

static int g_listen_fd = -1;
static int g_epoll_fd = -1;
static volatile int g_running = 0;

/* 每个客户端的接收缓冲（下标 = fd，fd 最大 1024，这里只关心前 MAX_CLIENTS 个） */
static char g_conn_buf[MAX_CLIENTS][CONN_BUF_SIZE];
static size_t g_conn_len[MAX_CLIENTS];

/* 下行通道：ESP 连接的 fd，加锁保护（HTTP 线程会写） */
static pthread_mutex_t g_esp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_esp_fd = -1;

static tcp_data_cb g_cb = NULL;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_client(int fd)
{
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    g_conn_len[fd] = 0;
    pthread_mutex_lock(&g_esp_lock);
    if (g_esp_fd == fd)
    {
        g_esp_fd = -1;
        printf("[tcp] ESP8266 断开连接\n");
    }
    pthread_mutex_unlock(&g_esp_lock);
    printf("[tcp] 客户端断开: fd=%d\n", fd);
}

static void accept_new_client(void)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept(g_listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0)
    {
        return;
    }
    if (fd >= MAX_CLIENTS)
    {
        fprintf(stderr, "[tcp] fd=%d 超出客户端表容量，拒绝\n", fd);
        close(fd);
        return;
    }

    set_nonblocking(fd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    g_conn_len[fd] = 0;

    printf("[tcp] 新连接: %s fd=%d\n", inet_ntoa(addr.sin_addr), fd);

    /* 第一个连上来的当作 ESP8266（下行命令发给它） */
    pthread_mutex_lock(&g_esp_lock);
    if (g_esp_fd == -1)
    {
        g_esp_fd = fd;
        printf("[tcp] 已标记为 ESP8266 下行通道\n");
    }
    pthread_mutex_unlock(&g_esp_lock);
}

/*
 * 从缓冲区里切出完整的 JSON 对象（花括号配对），
 * 每切出一条就回调上层，并把已消费的字节从缓冲区移除。
 * 剩余不完整的数据留在缓冲区等下一次 recv。
 */
static void extract_json_objects(int fd, tcp_data_cb cb)
{
    char *buf = g_conn_buf[fd];
    size_t len = g_conn_len[fd];

    while (1)
    {
        char *start = memchr(buf, '{', len);
        if (start == NULL)
        {
            /* 没有任何 '{'：全是垃圾字节（比如 ESP 的 AT 回显），清空 */
            len = 0;
            break;
        }

        /* 从 '{' 开始数花括号配对 */
        int depth = 0;
        char *p = start;
        char *end = NULL;
        while (p < buf + len)
        {
            if (*p == '{')
            {
                depth = depth + 1;
            }
            else if (*p == '}')
            {
                depth = depth - 1;
                if (depth == 0)
                {
                    end = p;
                    break;
                }
            }
            p = p + 1;
        }

        if (end == NULL)
        {
            /* 半包：'{' 有了但 '}' 还没到，留到下次 */
            break;
        }

        /* 切出完整对象，回调上层 */
        size_t obj_len = (size_t)(end - start) + 1;
        char obj[512];
        if (obj_len < sizeof(obj))
        {
            memcpy(obj, start, obj_len);
            obj[obj_len] = '\0';
            cb("esp8266", obj);
        }

        /* 移除已消费部分（含对象前面的垃圾字节） */
        size_t consumed = (size_t)(end - buf) + 1;
        memmove(buf, end + 1, len - consumed);
        len = len - consumed;
    }

    g_conn_len[fd] = len;
}

static void handle_client_data(int fd)
{
    char tmp[1024];

    while (1)
    {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0)
        {
            if (g_conn_len[fd] + (size_t)n >= CONN_BUF_SIZE)
            {
                fprintf(stderr, "[tcp] fd=%d 缓冲区将满，丢弃残留数据\n", fd);
                g_conn_len[fd] = 0;
            }
            memcpy(g_conn_buf[fd] + g_conn_len[fd], tmp, (size_t)n);
            g_conn_len[fd] = g_conn_len[fd] + (size_t)n;
            extract_json_objects(fd, g_cb);
            /* 非阻塞 fd 可能一次事件里有多批数据，读到 EAGAIN 为止 */
            continue;
        }
        if (n == 0)
        {
            /* 对端正常关闭 */
            close_client(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return;     /* 本批读完了 */
        }
        if (errno == EINTR)
        {
            continue;
        }
        close_client(fd);
        return;
    }
}

int tcp_server_init(int port)
{
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
    {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return -1;
    }
    if (listen(g_listen_fd, 8) < 0)
    {
        perror("listen");
        return -1;
    }

    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0)
    {
        perror("epoll_create1");
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_listen_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0)
    {
        perror("epoll_ctl");
        return -1;
    }

    printf("[tcp] 监听端口 %d\n", port);
    return 0;
}

void tcp_server_run(tcp_data_cb cb)
{
    struct epoll_event events[16];

    g_running = 1;
    g_cb = cb;

    while (g_running)
    {
        /* 500ms 超时一次，方便检查 g_running 标志及时退出 */
        int n = epoll_wait(g_epoll_fd, events, 16, 500);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            if (fd == g_listen_fd)
            {
                accept_new_client();
            }
            else
            {
                handle_client_data(fd);
            }
        }
    }

    printf("[tcp] 事件循环退出\n");
}

void tcp_server_send_to_esp(const char *data)
{
    pthread_mutex_lock(&g_esp_lock);
    if (g_esp_fd == -1)
    {
        fprintf(stderr, "[tcp] ESP 未连接，下行丢弃: %s\n", data);
        pthread_mutex_unlock(&g_esp_lock);
        return;
    }
    ssize_t n = send(g_esp_fd, data, strlen(data), MSG_NOSIGNAL);
    if (n < 0)
    {
        perror("[tcp] 下行发送失败");
    }
    else
    {
        printf("[tcp] 下行已发送: %s\n", data);
    }
    pthread_mutex_unlock(&g_esp_lock);
}

void tcp_server_stop(void)
{
    g_running = 0;
}
