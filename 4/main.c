#include <stdio.h>

#include "circular_queue.h"

int main() {
    // Simple test
    struct circular_queue q;
    circular_queue_init(&q);
    for (char i = 0; i < 12; ++i) {
        (void)circular_queue_push(&q, (void *)i);
        printf("Sz: %ld, cp: %ld\n", circular_queue_size(&q), circular_queue_capacity(&q));
    }
    for (char i = 0; i < 12; ++i) {
        printf("%hhd\n", (char)circular_queue_pop(&q));
    }
    circular_queue_destroy(&q);
}
