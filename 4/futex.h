#pragma once

#include <stdint.h>
#include <time.h>

/// Wrapper for `syscall(SYS_futex, uaddr, FUTEX_WAKE_PRIVATE, wake_count, 0...)`
long futexp_wake(uint32_t *uaddr, uint32_t wake_count);

/// Wrapper for `syscall(SYS_futex, uaddr, FUTEX_WAIT_PRIVATE, val, 0...)`
long futexp_wait(uint32_t *uaddr, uint32_t val);

/// Wrapper for `syscall(SYS_futex, uaddr, FUTEX_WAIT_PRIVATE, val, timeout, 0...)`
long futexp_timed_wait(uint32_t *uaddr, uint32_t val, const struct timespec *timeout);

/**
 * Regardless of the current futex value, waits for it to become `val`. If it's already
 * `val`, return immediately.
 * No spurious wakups, even for `EINTR`.
 *
 * Use with care: although the value check is totally ordered, no userspace locks are taken,
 * so nothing prevents the value from changing again.
 */
long futexp_wait_for(uint32_t *uaddr, uint32_t wait_for);

/**
 * Analogous to `futexp_wait_for` but (unless `timeout` is `NULL`) uses `timeout` for the
 * operation.
 * No spurious wakups, even for `EINTR`.
 */
long futexp_timed_wait_for(uint32_t *uaddr, uint32_t wait_for, const struct timespec *timeout);
