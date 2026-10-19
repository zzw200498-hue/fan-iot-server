#ifndef __DB_H
#define __DB_H

#include <stddef.h>

/*
 * db —— SQLite 封装
 *
 * 表结构：
 *   samples(id INTEGER PRIMARY KEY, ts INTEGER, temp REAL, humi REAL, fan INTEGER)
 *
 * 并发说明：worker 线程和 HTTP 线程各自持有独立的数据库连接，
 * 用 busy_timeout 避免写冲突，并用 WAL 模式提高并发读性能。
 */

int db_open(const char *path);
void db_close(void);

/* 插入一条采样记录，成功返回 0 */
int db_insert_sample(double temp, double humi, int fan);

/* 查询最近 n 条记录，结果以 JSON 数组写入 out（调用方保证 out 足够大） */
void db_query_recent(int n, char *out, size_t out_size);

#endif
