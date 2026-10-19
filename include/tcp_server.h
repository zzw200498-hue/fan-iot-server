#ifndef __TCP_SERVER_H
#define __TCP_SERVER_H

#include <stddef.h>

/*
 * tcp_server —— epoll 网络层
 *
 * 职责：监听 TCP 端口，接收 ESP8266 上报的数据，
 * 把完整的 JSON 对象通过回调交给上层；同时提供下行发送接口。
 *
 * 关键设计：
 * - 第一个连上来的客户端被当作 ESP8266，下行命令只发给它
 * - 单条连接的数据可能粘包/半包（TCP 是字节流），内部按
 *   花括号配对切分出完整 JSON 对象再回调
 */

typedef void (*tcp_data_cb)(const char *client_ip, const char *json_obj);

/* 创建监听 socket（不阻塞，真正的事件循环在 tcp_server_run 里） */
int tcp_server_init(int port);

/* epoll 事件主循环，阻塞运行直到 tcp_server_stop 被调用 */
void tcp_server_run(tcp_data_cb cb);

/* 向 ESP8266 所在连接发送下行数据（线程安全，供 HTTP 线程调用） */
void tcp_server_send_to_esp(const char *data);

/* 请求停止事件循环（信号处理函数里调用） */
void tcp_server_stop(void);

#endif
