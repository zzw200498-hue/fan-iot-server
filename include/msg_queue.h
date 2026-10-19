#ifndef __MSG_QUEUE_H
#define __MSG_QUEUE_H

/*
 * msg_queue —— 线程安全的有界消息队列（生产者-消费者管道的中间环节）
 *
 * 生产者：网络线程（收到一条 JSON 就推入）
 * 消费者：工作线程（取出一条就解析、落库）
 *
 * 规则：
 * - 队列满时新消息被丢弃（网络线程不能被数据库拖慢）
 * - 消费者在队列空时用条件变量睡眠，不空转
 */
#include <stddef.h>

#define MSG_QUEUE_CAPACITY 64
#define MSG_MAX_LEN 256

typedef struct {
    char *items[MSG_QUEUE_CAPACITY];    /* 存放 malloc 出来的字符串 */
    int head;                           /* 写位置 */
    int tail;                           /* 读位置 */
    int count;                          /* 当前条数 */
    int shutdown;                       /* 1 = 通知消费者退出 */
    void *lock;                         /* pthread_mutex_t */
    void *not_empty;                    /* pthread_cond_t */
} msg_queue_t;

void msg_queue_init(msg_queue_t *q);
void msg_queue_push(msg_queue_t *q, const char *msg);
int  msg_queue_pop(msg_queue_t *q, char *out, size_t out_size);
void msg_queue_shutdown(msg_queue_t *q);

#endif
