#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

#include "futex.h"

void *f(void *ftx) {
    *(unsigned int *)ftx = 3;
    int res = futexp_wake(ftx, INT_MAX);
    printf("Woke: %d\n", res);
    sleep(1);
    *(unsigned int *)ftx = 1;
    res = futexp_wake(ftx, INT_MAX);
    printf("Woke: %d\n", res);
    return NULL;
}

int main() {
    pthread_t thread;
    unsigned int ftx = 0;
    pthread_create(&thread, NULL, f, &ftx);
    long res = futexp_timed_wait_for(&ftx, 1, &(struct timespec){2});
    puts("Awake");
    printf("%ld\n%d\n", res, errno);
    pthread_join(thread, NULL);
}
