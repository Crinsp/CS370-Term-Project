#ifndef CS370_HUB_QUEUE_H
#define CS370_HUB_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "record.h"

/* Bounded FIFO of records waiting for the store (docs/DESIGN.md section 1:
 * 256 records, about 100 s at 2.5 rec/s). When full, pushing drops the
 * oldest record and counts it. No allocation. */
#define HUB_QUEUE_CAP 256

struct hub_queue {
    struct cs370_record buf[HUB_QUEUE_CAP];
    size_t head;      /* index of the oldest record */
    size_t len;
    uint64_t dropped; /* total records dropped on overflow */
};

void hub_queue_init(struct hub_queue *q);

/* Appends r. Returns 1 if the oldest record was dropped to make room, else 0. */
int hub_queue_push(struct hub_queue *q, const struct cs370_record *r);

/* Oldest record, or NULL if empty. */
const struct cs370_record *hub_queue_peek(const struct hub_queue *q);

/* Removes the oldest record. No-op if empty. */
void hub_queue_pop(struct hub_queue *q);

#endif
