#include "sbc_queue.h"
#include <string.h>

void dockbridge_sbc_clear(db_sbc_queue *queue) {
    queue->head = queue->count = queue->dropped = 0;
}

static unsigned frame_length(const uint8_t *header) {
    unsigned mode = (header[1] >> 2) & 3;
    unsigned blocks = 4 * (((header[1] >> 4) & 3) + 1);
    unsigned subbands = (header[1] & 1) ? 8 : 4;
    unsigned bitpool = header[2];
    if (header[0] != 0x9c || mode < 2 || bitpool < 2 || bitpool > 53) return 0;
    unsigned audio_bits = blocks * bitpool + (mode == 3 ? subbands : 0);
    return 4 + subbands + (audio_bits + 7) / 8;
}

int dockbridge_sbc_push(db_sbc_queue *queue, const uint8_t *payload, size_t size, unsigned count) {
    uint8_t lengths[15];
    if (!count || count > sizeof(lengths)) return 0;
    size_t offset = 0;
    // Validate the whole payload before modifying the queue.
    for (unsigned i = 0; i < count; ++i) {
        if (size - offset < 4) return 0;
        unsigned length = frame_length(payload + offset);
        if (!length || length > DB_SBC_MAX_FRAME || length > size - offset) return 0;
        lengths[i] = length;
        offset += length;
    }
    if (offset != size) return 0;

    offset = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (queue->count == DB_SBC_CAPACITY) {
            dockbridge_sbc_pop(queue);
            ++queue->dropped;
        }
        db_sbc_frame *frame = &queue->frames[(queue->head + queue->count) % DB_SBC_CAPACITY];
        frame->length = lengths[i];
        memcpy(frame->bytes, payload + offset, frame->length);
        offset += frame->length;
        ++queue->count;
    }
    return 1;
}

db_sbc_frame *dockbridge_sbc_pop(db_sbc_queue *queue) {
    if (!queue->count) return NULL;
    db_sbc_frame *frame = &queue->frames[queue->head];
    queue->head = (queue->head + 1) % DB_SBC_CAPACITY;
    --queue->count;
    return frame;
}
