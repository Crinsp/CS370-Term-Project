#include "queue.h"

#include <string.h>

void hub_queue_init(struct hub_queue *q)
{
    memset(q, 0, sizeof(*q));
}

int hub_queue_push(struct hub_queue *q, const struct cs370_record *r)
{
    int dropped = 0;

    if (q->len == HUB_QUEUE_CAP) {
        hub_queue_pop(q);
        q->dropped++;
        dropped = 1;
    }
    q->buf[(q->head + q->len) % HUB_QUEUE_CAP] = *r;
    q->len++;
    return dropped;
}

const struct cs370_record *hub_queue_peek(const struct hub_queue *q)
{
    return q->len ? &q->buf[q->head] : NULL;
}

void hub_queue_pop(struct hub_queue *q)
{
    if (q->len == 0)
        return;
    q->head = (q->head + 1) % HUB_QUEUE_CAP;
    q->len--;
}
