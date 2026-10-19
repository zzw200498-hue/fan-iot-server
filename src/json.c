#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * 实现思路：直接找 "key" 字符串 → 找它后面的冒号 → 跳过空格读数值。
 * 只处理扁平 JSON，够本项目用；复杂嵌套就该上真正的解析库了。
 */

static const char *find_value_pos(const char *json, const char *key)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (p == NULL)
    {
        return NULL;
    }

    /* 从 key 之后找冒号 */
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL)
    {
        return NULL;
    }

    /* 跳过冒号后的空格，停在数值开头 */
    p = p + 1;
    while (*p == ' ' || *p == '\t')
    {
        p = p + 1;
    }
    return p;
}

int json_get_double(const char *json, const char *key, double *out)
{
    const char *p = find_value_pos(json, key);
    if (p == NULL)
    {
        return 0;
    }
    char *end = NULL;
    *out = strtod(p, &end);
    if (end == p)
    {
        return 0;   /* 没解析出任何数字 */
    }
    return 1;
}

int json_get_int(const char *json, const char *key, int *out)
{
    const char *p = find_value_pos(json, key);
    if (p == NULL)
    {
        return 0;
    }
    char *end = NULL;
    *out = (int)strtol(p, &end, 10);
    if (end == p)
    {
        return 0;
    }
    return 1;
}
