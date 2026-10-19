#include "worker.h"
#include "msg_queue.h"
#include "json.h"
#include "db.h"
#include <stdio.h>
#include <string.h>

/*
 * 消费者线程：从队列取 JSON → 解析 → 落库。
 * 全部的多线程知识点（互斥锁、条件变量、虚假唤醒、优雅退出）
 * 都在 msg_queue.c 里，这一层只关心业务。
 */

extern msg_queue_t g_queue;     /* main.c 里定义 */

void *worker_thread(void *arg)
{
    (void)arg;
    char msg[MSG_MAX_LEN];

    printf("[worker] 启动\n");

    while (1)
    {
        if (msg_queue_pop(&g_queue, msg, sizeof(msg)) == 0)
        {
            break;      /* 队列已 shutdown 且无剩余消息 */
        }

        double temp = 0;
        double humi = 0;
        int fan = 0;

        int ok_temp = json_get_double(msg, "temp", &temp);
        int ok_humi = json_get_double(msg, "humi", &humi);
        json_get_int(msg, "fan", &fan);     /* fan 解析失败就保持 0，不阻塞入库 */

        if (ok_temp == 0 || ok_humi == 0)
        {
            fprintf(stderr, "[worker] 无法解析的消息: %s\n", msg);
            continue;
        }

        if (db_insert_sample(temp, humi, fan) != 0)
        {
            continue;   /* 落库失败不退出，下一条继续 */
        }

        printf("[worker] 入库: temp=%.1f humi=%.1f fan=%d\n", temp, humi, fan);
    }

    printf("[worker] 退出\n");
    return NULL;
}
