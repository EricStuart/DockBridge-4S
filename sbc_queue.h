#ifndef DOCKBRIDGE_SBC_QUEUE_H
#define DOCKBRIDGE_SBC_QUEUE_H

#include <stddef.h>
#include <stdint.h>

enum { DB_SBC_MAX_FRAME = 120, DB_SBC_CAPACITY = 160, DB_SBC_PREFILL = 72 };

typedef struct {
    uint8_t length;
    uint8_t bytes[DB_SBC_MAX_FRAME];
} db_sbc_frame;

typedef struct {
    unsigned head, count, dropped;
    db_sbc_frame frames[DB_SBC_CAPACITY];
} db_sbc_queue;

void dockbridge_sbc_clear(db_sbc_queue *queue);
int dockbridge_sbc_push(db_sbc_queue *queue, const uint8_t *payload, size_t size, unsigned count);
db_sbc_frame *dockbridge_sbc_pop(db_sbc_queue *queue);

#endif
