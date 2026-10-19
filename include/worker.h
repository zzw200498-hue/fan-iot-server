#ifndef __WORKER_H
#define __WORKER_H

/*
 * worker —— 消费者线程
 *
 * 循环：从消息队列取 JSON → 提取字段 → 写入 SQLite → 打印日志。
 * 队列 shutdown 后自然退出。
 */

void *worker_thread(void *arg);

#endif
