#pragma once

/**
 * Implements a circular queue. Stores values of type `void *`.
 */
struct circular_queue {
    /*
     * Actual memory allocated to `data` is `(sizeof (void *)) * dcapacity`, maximum number of
     * elements that can be stored in a queue is `dcapacity-1` (one element is reserved for
     * empty/full disambiguation).
     *
     * `head` points at the first element in the queue`, `tail` points one index beyond last
     * element in the queue. For example, `head == tail` means the queue is empty,
     * `(tail + 1) % dcapacity == head` means the queue is full (next push will cause reallocation).
     */

    size_t dcapacity;
    size_t head, tail;
    void **data;
};


// The only error this function may return is OOM
unsigned char circular_queue_init(struct circular_queue *queue);

void circular_queue_destroy(struct circular_queue *queue);

void *circular_queue_pop(struct circular_queue *queue);

// The only error this function may return is OOM
unsigned char circular_queue_push(struct circular_queue *queue, void *val);

//void circular_queue_unpush(struct circular_queue *queue);

size_t circular_queue_capacity(const struct circular_queue *queue) __attribute__((pure));

size_t circular_queue_size(const struct circular_queue *queue) __attribute__((pure));
