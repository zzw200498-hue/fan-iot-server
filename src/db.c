#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

/*
 * 实现要点：
 * - worker 和 HTTP 两个线程各开各的连接（SQLite 不允许跨线程共享同一个连接）
 * - WAL 模式：写不阻塞读，读不阻塞写
 * - busy_timeout：万一撞上写锁，等 1 秒再报错，而不是立刻失败
 */

static sqlite3 *g_db = NULL;

int db_open(const char *path)
{
    if (sqlite3_open(path, &g_db) != SQLITE_OK)
    {
        fprintf(stderr, "[db] 打开失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_busy_timeout(g_db, 1000);
    char *err = NULL;
    if (sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[db] WAL 设置失败: %s\n", err);
        sqlite3_free(err);
    }

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS samples ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts   INTEGER NOT NULL,"          /* Unix 时间戳（秒） */
        "  temp REAL NOT NULL,"
        "  humi REAL NOT NULL,"
        "  fan  INTEGER NOT NULL"
        ");";
    if (sqlite3_exec(g_db, create_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[db] 建表失败: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    printf("[db] 已打开 %s\n", path);
    return 0;
}

int db_insert_sample(double temp, double humi, int fan)
{
    const char *sql = "INSERT INTO samples(ts, temp, humi, fan) VALUES(strftime('%s','now'), ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[db] prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_double(stmt, 1, temp);
    sqlite3_bind_double(stmt, 2, humi);
    sqlite3_bind_int(stmt, 3, fan);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "[db] 插入失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/* sqlite3_exec 的回调：把每一行拼成 JSON 对象追加到 buf 末尾 */
static int query_row_cb(void *arg, int cols, char **values, char **names)
{
    char *buf = ((char **)arg)[0];
    size_t *used = &((size_t *)arg)[1]; /* 不太优雅但少传一个结构体 */
    size_t cap = ((size_t *)arg)[2];

    (void)names;
    if (cols < 4)
    {
        return 0;
    }

    /* 超过缓冲 3/4 就停止追加，由调用方判断收尾 */
    if (*used > cap * 3 / 4)
    {
        return 0;
    }

    int n = snprintf(buf + *used, cap - *used,
                     "{\"ts\":%s,\"temp\":%s,\"humi\":%s,\"fan\":%s},",
                     values[0], values[1], values[2], values[3]);
    if (n > 0)
    {
        *used = *used + (size_t)n;
    }
    return 0;
}

void db_query_recent(int n, char *out, size_t out_size)
{
    char sql[128];

    snprintf(sql, sizeof(sql),
             "SELECT ts, temp, humi, fan FROM samples ORDER BY id DESC LIMIT %d;", n);

    /* arg 布局：[0]=buf 指针, [1]=已用长度, [2]=总容量 —— 传给回调用 */
    char *arg[3];
    arg[0] = out;
    ((size_t *)arg)[1] = 0;
    ((size_t *)arg)[2] = out_size;

    snprintf(out, out_size, "{\"samples\":[");

    char *err = NULL;
    if (sqlite3_exec(g_db, sql, query_row_cb, arg, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[db] 查询失败: %s\n", err);
        sqlite3_free(err);
    }

    /* 去掉末尾多余的逗号，再闭合数组 */
    size_t used = ((size_t *)arg)[1];
    if (used > 0 && out[used - 1] == ',')
    {
        used = used - 1;
    }
    snprintf(out + used, out_size - used, "]}");
}
