#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>

#include "circular_queue.h"
#include "futex.h"
#include "thread_pool.h"

/**
 * Possible states of `thread_task`. Only the following transitions are
 * possible _under normal operation_ (see below):
 * TASK_STATE_CREATED       -> TASK_STATE_PUSHED,
 * TASK_STATE_PUSHED        -> TASK_STATE_PUSHED_GHOST,
 * TASK_STATE_PUSHED        -> TASK_STATE_RUNNING,
 * TASK_STATE_PUSHED_GHOST  -> TASK_STATE_RUNNING_GHOST,
 * TASK_STATE_RUNNING       -> TASK_STATE_RUNNING_GHOST,
 * TASK_STATE_RUNNING       -> TASK_STATE_FINISHED,
 * TASK_STATE_RUNNING_GHOST -> TASK_STATE_FINISHED.
 *
 * Note that the directed graph formed by these states and transitions is acyclic, which
 * allows for implementation of some operations as a sequence of atomic operations without
 * locks.
 *
 * Another possible transition is
 * TASK_STATE_FINISHED -> TASK_STATE_PUSHED
 * but it's up to the library's user to ensure that this transition does not happen while
 * any thread pool function is working (except for `thread_pool_push_task`, which is the
 * function to perform this transition).
 * So, thread pool's code may assume that once a task is finished it won't change its state.
 */
enum task_state {
    TASK_STATE_CREATED,
    TASK_STATE_PUSHED,
    TASK_STATE_PUSHED_GHOST,
    TASK_STATE_RUNNING,
    TASK_STATE_RUNNING_GHOST,
    TASK_STATE_FINISHED,
};

struct thread_task {
    thread_task_f function;
    void *arg;
    void *ret;

    /**
     * Current task state. Can be used as a futex.
     * On every change (except for when intialized via `thread_task_new`), `FUTEX_WAKE_PRIVATE`
     * shall be performed for `INT_MAX` waiters.
     *
     * Must be assigned and fetched using `__atomic_*` functions with acquire and release memory
     * orders, respectively, to ensure consistent state transitions.
     */
    enum task_state state;
};

struct thread_pool {
    size_t tmax;
    pthread_t *threads;

    pthread_mutex_t queue_lock;
    /// Tasks queue. Protected with `queue_lock`
    struct circular_queue queue;

    /// The number of spawned threads. Protected with `queue_lock`
    size_t spawned_count;
    /// The number of free workers. Protected with `queue_lock`
    size_t free_count;
};

static inline bool atomic_cex_state(struct thread_task *task, enum task_state old, enum task_state new) {
    /*
     * Success memory order is acquire+release because I want the task to have fully transitioned to
     * the `old` state before I can see it and I want the state to change to `new` before any further
     * actions are taken.
     * Failure memory order is relaxed because the unexpected old state is not reported and no actions
     * are taken based on it.
     */
    bool succ = __atomic_compare_exchange_n(&task->state, &old, new, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    if (succ)  /* If successfully exchanged, wake up waiters! */
        (void)futexp_wake(&task->state, INT_MAX);
    return succ;
}

int
thread_pool_new(int max_thread_count, struct thread_pool **poolp)
{
    // For some reason this `_Static_assert` won't properly compile if on the global level
    _Static_assert(sizeof (enum task_state) == sizeof (uint32_t),
            "`enum task_state` must have the same size as `uint32_t` to be used as a futex");


    if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    *poolp = malloc(sizeof (struct thread_pool));
    assert(*poolp);

    struct thread_pool *pool = *poolp;

    pool->tmax = max_thread_count;
    pool->threads = malloc(max_thread_count * sizeof pool->threads[0]);
    assert(pool->threads);

    int err = pthread_mutex_init(&pool->queue_lock, NULL);
    assert(!err);
    err = circular_queue_init(&pool->queue);
    assert(!err);  // OOM only

    pool->spawned_count = pool->free_count = 0;

    return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
    int err = pthread_mutex_lock(&pool->queue_lock);
    assert(!err);
    /* Note: `pool->spawned_count` and `pool->free_count` are also protected with `.queue_lock` */
    int fail = circular_queue_size(&pool->queue) + (pool->spawned_count - pool->free_count);
    err = pthread_mutex_unlock(&pool->queue_lock);
    assert(!err);
    if (fail)
        return TPOOL_ERR_HAS_TASKS;

    circular_queue_destroy(&pool->queue);
    err = pthread_mutex_destroy(&pool->queue_lock);
    assert(!err);

    for (size_t i = 0; i < pool->spawned_count; ++i) {
        err = pthread_cancel(pool->threads[i]);
        assert(!err);
    }
    for (size_t i = 0; i < pool->spawned_count; ++i) {
        err = pthread_join(pool->threads[i], NULL);
        assert(!err);
    }

    free(pool->threads);

    free(pool);

    return 0;
}

__attribute__((pure))
int
thread_pool_thread_count(const struct thread_pool *pool)
{
    return pool->spawned_count;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
    int err = pthread_mutex_lock(&pool->queue_lock);
    assert(!err);

    int ret;

    if (circular_queue_size(&pool->queue) >= TPOOL_MAX_TASKS) {
        ret = TPOOL_ERR_TOO_MANY_TASKS;
    } else {
        if (atomic_cex_state(task, TASK_STATE_CREATED, TASK_STATE_PUSHED)) {
            /* Success. Pushed task for the first time. */
            ret = 0;
            err = circular_queue_push(&pool->queue, task);
            assert(!err);  // OOM only
        } else if (atomic_cex_state(task, TASK_STATE_FINISHED, TASK_STATE_PUSHED)) {
            /* Success. Repushed the task. It's up to the user to ensure that it was joined earlier. */
            ret = 0;
            err = circular_queue_push(&pool->queue, task);
            assert(!err);  // OOM only
        } else {
            ret = TPOOL_ERR_INVALID_REPUSH;
        }

        if (pool->free_count == 0 && pool->spawned_count < pool->tmax) {
            // TODO: spawn new child
        }
    }

    err = pthread_mutex_unlock(&pool->queue_lock);
    assert(!err);
    return ret;
}

int
thread_task_new(struct thread_task **taskp, thread_task_f function, void *arg)
{
    *taskp = malloc(sizeof (struct thread_task));
    assert(*taskp);

    struct thread_task *task = *taskp;

    task->function = function;
    task->arg = arg;
    // task->ret: uninitialized;

    /*
     * `task->state` is initialized last with memory order release: when accessed after task
     * creation, all the above operations must will have been completed.
     */
    __atomic_store_n(&task->state, TASK_STATE_CREATED, __ATOMIC_RELEASE);
    /* No need to wake up anyone as the task just got initialized, no one is waiting yet. */

    return 0;
}

int
thread_task_delete(struct thread_task *task)
{
    enum task_state state = __atomic_load_n(&task->state, __ATOMIC_ACQUIRE);
    if (state == TASK_STATE_CREATED || state == TASK_STATE_FINISHED) {
        free(task);
        return 0;
    } else {
        return TPOOL_ERR_TASK_IN_POOL;
    }
}

bool
thread_task_is_finished(const struct thread_task *task)
{
    /*
     * When control is returned to the caller, in case of success, the caller expects all
     * the finishing operations to have completed, so the relaxed memory order is not
     * sufficient. Note that acquire is enough, as the state is set with release.
     */
    return __atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_STATE_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
    /*
     * As `task->state` is not protected by any mutex (and might change at any time), the only thing
     * caller may rely on here is that if this function returned `true`, then the task has started its
     * execution (although, perhaps, has already finished by the moment the caller can do anything).
     *
     * Therefore, need to ensure everything happening before the task gets into the running state is
     * finished, thus the memory order.
     */
    return __atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_STATE_RUNNING;
    /*
     * Note that `TASK_STATE_RUNING_GHOST` also corresponds to a running task but ghost tasks must never
     * be addressed - the behavior is undefined.
     */
}

int
thread_task_join(struct thread_task *task, void **result) {
    if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_STATE_CREATED)
        return TPOOL_ERR_TASK_NOT_PUSHED;

    futexp_wait_for(&task->state, TASK_STATE_FINISHED);

    /* No need to synchronize after the task has finished */
    *result = task->ret;

    return 0;
}

#ifdef NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
    /* IMPLEMENT THIS FUNCTION */
    (void)task;
    (void)timeout;
    (void)result;
    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif

#ifdef NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
    /* IMPLEMENT THIS FUNCTION */
    (void)task;
    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif
