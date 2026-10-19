#ifndef __JSON_H
#define __JSON_H

#include <stddef.h>

/*
 * json —— 极简 JSON 字段提取器
 *
 * 只支持从扁平对象里取数值字段（本项目上报的格式就是扁平的）：
 *   {"temp":26.7,"humi":77.0,"fan":40}
 * 不做完整 JSON 解析——教学项目里自己写比引入 cJSON 更能讲清楚原理。
 */

/* 从 json 里取 key 对应的浮点值，成功返回 1，找不到返回 0 */
int json_get_double(const char *json, const char *key, double *out);

/* 从 json 里取 key 对应的整数值，成功返回 1，找不到返回 0 */
int json_get_int(const char *json, const char *key, int *out);

#endif
