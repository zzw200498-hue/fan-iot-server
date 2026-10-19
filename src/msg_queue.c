#include "msg_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * 实现要点：
 * - push 端（网络线程）永不阻塞：队列满直接丢消息并告警，
 *   保证网络接收不被数据库的快慢拖垮
 * - pop 端（工作线程）在空队列时睡在条件变量上，
 *   push 每放一条消息就 signal 唤醒一次
 * - shutdown 置位后，睡着的消费者被唤醒并发现队列为空，随即退出
 */

void msg_queue_init(msg_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->lock = malloc(sizeof(pthread_mutex_t));
    q->not_empty = malloc(sizeof(pthread_cond_t));
    pthread_mutex_init((pthread_mutex_t *)q->lock, NULL);
    pthread_cond_init((pthread_cond_t *)q->not_empty, NULL);
}

void msg_queue_push(msg_queue_t *q, const char *msg)
{
    pthread_mutex_lock((pthread_mutex_t *)q->lock);

    if (q->count == MSG_QUEUE_CAPACITY)
    {
        /* 队列满：丢弃本条，保护网络线程不阻塞。正常运行时不会发生 */
        fprintf(stderr, "[queue] 队列已满，丢弃消息: %s\n", msg);
        pthread_mutex_unlock((pthread_mutex_t *)q->lock);
        return;
    }

    q->items[q->head] = strdup(msg);
    if (q->items[q->head] == NULL)
    {
        pthread_mutex_unlock((pthread_mutex_t *)q->lock);
        return;
    }
    q->head = (q->head + 1) % MSG_QUEUE_CAPACITY;
    q->count = q->count + 1;

    pthread_mutex_unlock((pthread_mutex_t *)q->lock);
    pthread_cond_signal((pthread_cond_t *)q->not_empty);
}

int msg_queue_pop(msg_queue_t *q, char *out, size_t out_size)
{
    pthread_mutex_lock((pthread_mutex_t *)q->lock);

    /* 用 while 而不是 if：防止虚假唤醒，醒来后必须再确认一次条件 */
    while (q->count == 0 && q->shutdown == 0)
    {
        pthread_cond_wait((pthread_cond_t *)q->not_empty, (pthread_mutex_t *)q->lock);
    }

    if (q->count == 0)
    {
        /* shutdown 且无剩余消息：让消费者退出 */
        pthread_mutex_unlock((pthread_mutex_t *)q->lock);
        return 0;
    }

    char *msg = q->items[q->tail];
    q->tail = (q->tail + 1) % MSG_QUEUE_CAPACITY;
    q->count = q->count - 1;

    pthread_mutex_unlock((pthread_mutex_t *)q->lock);

    snprintf(out, out_size, "%s", msg);
    free(msg);
    return 1;
}

void msg_queue_shutdown(msg_queue_t *q)
{
    pthread_mutex_lock((pthread_mutex_t *)q->lock);
    q->shutdown = 1;
    pthread_mutex_unlock((pthread_mutex_t *)q->lock);
    /* 广播唤醒所有睡着的消费者，让它们看到 shutdown 标志 */
    pthread_cond_broadcast((pthread_cond_t *)q->not_empty);
}
