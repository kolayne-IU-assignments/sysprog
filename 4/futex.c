#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "futex.h"

long futexp_wake(uint32_t *uaddr, uint32_t wake_count) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE_PRIVATE, wake_count, /* ignored: */ 0, 0, 0);
}

long futexp_wait(uint32_t *uaddr, uint32_t val) {
    return futexp_timed_wait(uaddr, val, NULL);
}

long futexp_timed_wait(uint32_t *uaddr, uint32_t val, const struct timespec *timeout) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT_PRIVATE, val, timeout, /* ignored: */ 0, 0);
}

long futexp_wait_for(uint32_t *uaddr, uint32_t wait_for) {
    return futexp_timed_wait_for(uaddr, wait_for, NULL);
}

inline static struct timespec timespec_diff(const struct timespec a, const struct timespec b, bool *underflow) {
    struct timespec diff = { .tv_sec = a.tv_sec - b.tv_sec, .tv_nsec = a.tv_nsec - b.tv_nsec };
    if (diff.tv_nsec < 0) {
        diff.tv_nsec += 1000*1000*1000;
        if (diff.tv_sec == 0)
            *underflow = true;
        diff.tv_sec--;
    }
    return diff;
}

long futexp_timed_wait_for(uint32_t *uaddr, uint32_t wait_for, const struct timespec *timeout) {
    struct timespec remaining, start, now;

    if (timeout) {
        remaining = *timeout;
        int err = clock_gettime(CLOCK_MONOTONIC, &start);
        assert(!err);
    }

    while (1) {
        /*
         * Note: the barrier I desire is LoadLoad+StoreLoad:
         * If the current value of futex is already the desired one, I want all the related work to
         * (by the setter) to have been finished.
         * As there's no such barrier, have no other option but to totally order.
         */
        uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_SEQ_CST);
        if (cur == wait_for)
            return 0;

        if (timeout) {
            int err = clock_gettime(CLOCK_MONOTONIC, &now);
            assert(!err);

            bool underflow = false;
            /* `now` is not earlier than `start`, thus underflow must not occur in the first subtraction */
            remaining = timespec_diff(*timeout, timespec_diff(now, start, NULL), &underflow);
            if (underflow) {
                /* Time is up. Emulate futex's behavior. */
                errno = ETIMEDOUT;
                return -1;
            }
        }

        long ret = futexp_timed_wait(uaddr, cur, timeout ? &remaining : NULL);
        if (ret == 0) {
            /* Some change to the futex. Continue to see if it's the desired change. */
            continue;
        } else if (errno == EAGAIN || errno == EINTR) {
            /* Either `cur` is outdated or we were interrupted with a signal. Continue waiting. */
            continue;
        } else {
            /* Caller's error (possibly timeout) */
            return ret;
        }
    }
}
