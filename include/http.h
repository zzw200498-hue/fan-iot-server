#ifndef __HTTP_H
#define __HTTP_H

/*
 * http —— 简易 HTTP 查询/控制接口（独立线程，端口默认 8080）
 *
 * 路由：
 *   GET /                       → 内置网页（自动刷新的温度表格 + 风扇控制按钮）
 *   GET /api/history?n=10       → 最近 n 条记录，JSON 数组
 *   GET /api/fan?set=on         → 下发 FAN=ON 给 ESP8266
 *   GET /api/fan?set=off        → 下发 FAN=OFF
 *   GET /api/fan?set=auto       → 下发 FAN=AUTO
 */

void *http_thread(void *arg);
void http_server_stop(void);

#endif
