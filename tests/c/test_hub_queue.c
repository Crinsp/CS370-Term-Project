#include <stdio.h>
#include <string.h>

#include "queue.h"
#include "record.h"

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while (0)

static struct cs370_record rec(uint32_t seq)
{
    struct cs370_record r;
    memset(&r, 0, sizeof(r));
    r.seq = seq;
    r.source = CS370_SOURCE_LIGHT;
    r.lux = (float)seq;
    cs370_record_seal(&r);
    return r;
}

static struct hub_queue q; /* 8 KiB: keep it off the stack */

int main(void)
{
    struct cs370_record r;

    hub_queue_init(&q);
    CHECK(hub_queue_peek(&q) == NULL);
    hub_queue_pop(&q); /* pop on empty is a no-op */
    CHECK(q.len == 0 && q.dropped == 0);

    /* FIFO order. */
    for (uint32_t i = 0; i < 3; i++) {
        r = rec(i);
        CHECK(hub_queue_push(&q, &r) == 0);
    }
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(hub_queue_peek(&q) && hub_queue_peek(&q)->seq == i);
        hub_queue_pop(&q);
    }
    CHECK(hub_queue_peek(&q) == NULL);

    /* Fill exactly to capacity: nothing dropped. head is not 0 here, so the
     * ring also wraps. */
    for (uint32_t i = 0; i < HUB_QUEUE_CAP; i++) {
        r = rec(i);
        CHECK(hub_queue_push(&q, &r) == 0);
    }
    CHECK(q.len == HUB_QUEUE_CAP && q.dropped == 0);

    /* 44 more: each push drops the oldest, so the newest 256 remain. */
    for (uint32_t i = HUB_QUEUE_CAP; i < HUB_QUEUE_CAP + 44; i++) {
        r = rec(i);
        CHECK(hub_queue_push(&q, &r) == 1);
    }
    CHECK(q.len == HUB_QUEUE_CAP && q.dropped == 44);
    for (uint32_t i = 44; i < HUB_QUEUE_CAP + 44; i++) {
        const struct cs370_record *p = hub_queue_peek(&q);
        CHECK(p && p->seq == i && cs370_record_valid(p));
        hub_queue_pop(&q);
    }
    CHECK(hub_queue_peek(&q) == NULL && q.dropped == 44);

    if (failures == 0)
        puts("test_hub_queue: ok");
    return failures ? 1 : 0;
}
