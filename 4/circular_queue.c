#include <stdlib.h>
#include <string.h>

#include "circular_queue.h"

enum {
    CQ_ERR_NO_MEM = 1,
};

// The only error that may be reported is OOM
static unsigned char circular_queue_realloc(struct circular_queue *queue, size_t new_capacity) {
    void **new = (void **)malloc(new_capacity * sizeof (void *));
    if (!new)
        return CQ_ERR_NO_MEM;

    if (queue->head <= queue->tail) {
        memcpy(new + queue->head, queue->data + queue->head, (queue->tail - queue->head) * sizeof (void *));
    } else {
        memcpy(new + queue->head, queue->data + queue->head, (queue->dcapacity - queue->head) * sizeof (void *));
        memcpy(new, queue->data, queue->tail * sizeof (void *));
    }
    free(queue->data);
    queue->data = new;

    queue->dcapacity = new_capacity;
    return 0;
}

// The only error that may be reported is OOM
unsigned char circular_queue_init(struct circular_queue *queue) {
    queue->dcapacity = 0;
    queue->head = queue->tail = 0;
    queue->data = NULL;
    return circular_queue_realloc(queue, 8);  // 8: default initial capacity
}

void circular_queue_destroy(struct circular_queue *queue) {
    free(queue->data);
}

#define NEXT(queue, field) (((queue)->field)+1) % ((queue)->dcapacity)

void *circular_queue_pop(struct circular_queue *queue) {
    void *res = queue->data[queue->head];
    queue->head = NEXT(queue, head);
    return res;
}

// The only error that may be reported is OOM
unsigned char circular_queue_push(struct circular_queue *queue, void *val) {
    if (NEXT(queue, tail) == queue->head) {
        int err = circular_queue_realloc(queue, queue->dcapacity * 2);
        if (err)
            return err;
    }

    queue->data[queue->tail] = val;
    queue->tail = NEXT(queue, tail);
    return 0;
}

/*
void circular_queue_unpush(struct circular_queue *queue) {
    --queue->tail;
    if (queue->tail < 0)
        queue->tail += queue->dcapacity;
}
*/

__attribute__((pure))
size_t circular_queue_capacity(const struct circular_queue *queue) {
    return queue->dcapacity - 1;
}

__attribute__((pure))
size_t circular_queue_size(const struct circular_queue *queue) {
    if (queue->head <= queue->tail)
        return queue->tail - queue->head;
    return queue->dcapacity - queue->head + queue->tail;
}
