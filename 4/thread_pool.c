#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <math.h>

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
 * TASK_STATE_RUNNING       -> TASK_STATE_COMPLETED,
 * TASK_STATE_RUNNING_GHOST -> TASK_STATE_COMPLETED (and the task is freed outright),
 * TASK_STATE_COMPLETED     -> TASK_STATED_JOINED.
 *
 * Note that the directed graph formed by these states and transitions is acyclic, which
 * allows for implementation of some operations as a sequence of atomic operations without
 * locks.
 *
 * Another possible transition is
 * TASK_STATE_JOINED -> TASK_STATE_CREATED
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
    TASK_STATE_COMPLETED,
    TASK_STATE_JOINED,
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
    /// Signalled (with taken `queue_lock`) when new tasks are pushed into the queue
    pthread_cond_t queue_push_cond;
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

static void deferred_mutex_unlock(void *mutexv) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)mutexv;
    int err = pthread_mutex_unlock(mutex);
    assert(!err);
}

static void *thread_pool_worker(void *poolv) {
    struct thread_pool *pool = (struct thread_pool *)poolv;

    struct thread_task *task = NULL;

    /*
     * The worker will run forever until canceled with `pthread_cancel`.
     * Note that the cancelation signal can only be sent by `thread_pool_delete` after it acquires the
     * `pool->queue_lock` and makes sure all spawned workers are free. That can only be true when the worker
     * is waiting for the next task in the `pthread_cond_wait` below, and that's the only place where the
     * cancelation might be attempted (regardless of the thread's cancelation type (asynchronous/deferred)).
     */
    while (1) {
        int err = pthread_mutex_lock(&pool->queue_lock);
        assert(!err);

        /*
         * Lock/unlock the mutex via a thread-cancelation clean-up handler - to maintain a proper lock state
         * when the worker is canceled on pool deletion.
         */
        pthread_cleanup_push(deferred_mutex_unlock, &pool->queue_lock);

        /*
         * Declare the previous task as finished only after taking the mutex. Otherwise,
         * there's a race condition between when I report I'm done with the task
         * and when I report I'm a free worker (it sometimes prevents the pool
         * from being deleted).
         */
        if (task) {
            /*
             * Warning: the checks order is important: task can turn from RUNNING to RUNNING_GHOST,
             * but not vice versa.
             */
            if (atomic_cex_state(task, TASK_STATE_RUNNING, TASK_STATE_COMPLETED)) {
                /* Success. Nothing else to do. */
            } else if (atomic_cex_state(task, TASK_STATE_RUNNING_GHOST, TASK_STATE_JOINED)) {
                /* A detached task has finished. Declare it joined and destroy. */
                err = thread_task_delete(task);
                assert(!err);  /* Error indicates that a deatched task was repushed (which is UB) */
            } else {
                assert(false);  /* A task that I was performing is not in a running state */
            }
        }

        /*
         * Now that I'm done with the previous task, I am free.
         * Note: no need to synchronize on `pool->free_count`, as it is protected with `pool->queue_lock`
         */
        ++pool->free_count;
        while (circular_queue_size(&pool->queue) == 0) {
            err = pthread_cond_wait(&pool->queue_push_cond, &pool->queue_lock);
            assert(!err);
        }
        --pool->free_count;

        task = (struct thread_task *)circular_queue_pop(&pool->queue);
        pthread_cleanup_pop(1);  /* Unlock the mutex */

        /*
         * Warning: the order of condition checks is important. Task can turn from PUSHED to
         * PUSHED_GHOST, but not vice versa.
         */
        bool ok = atomic_cex_state(task, TASK_STATE_PUSHED, TASK_STATE_RUNNING) ||
            atomic_cex_state(task, TASK_STATE_PUSHED_GHOST, TASK_STATE_RUNNING_GHOST);
        assert(ok);  /* Task popped from queue must have been pushed */

        /* Run the task on this iteration of the loop, but declare as finished on the next one */
        task->ret = task->function(task->arg);
    }

    assert(false);  // Unreachable
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
    err = pthread_cond_init(&pool->queue_push_cond, NULL);
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

    /* Should join all workers before destroying any resources it is using */
    for (size_t i = 0; i < pool->spawned_count; ++i) {
        err = pthread_cancel(pool->threads[i]);
        assert(!err);
    }
    for (size_t i = 0; i < pool->spawned_count; ++i) {
        err = pthread_join(pool->threads[i], NULL);
        assert(!err);
    }
    free(pool->threads);

    circular_queue_destroy(&pool->queue);
    err = pthread_cond_destroy(&pool->queue_push_cond);
    assert(!err);
    err = pthread_mutex_destroy(&pool->queue_lock);
    assert(!err);

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
        } else if (atomic_cex_state(task, TASK_STATE_JOINED, TASK_STATE_PUSHED)) {
            /* Success. Repushed the task. It's up to the user to ensure that it was joined earlier. */
            ret = 0;
            err = circular_queue_push(&pool->queue, task);
            assert(!err);  // OOM only
        } else {
            ret = TPOOL_ERR_INVALID_REPUSH;
        }

        if (pool->free_count == 0 && pool->spawned_count < pool->tmax) {
            err = pthread_create(&pool->threads[pool->spawned_count++], NULL,
                    thread_pool_worker, (void *)pool);
            assert(!err);  /* Unable to spawn new thread */
        }
    }

    err = pthread_cond_signal(&pool->queue_push_cond);
    assert(!err);
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
    if (state == TASK_STATE_CREATED || state == TASK_STATE_JOINED) {
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
    return __atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_STATE_COMPLETED;
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
    /*
     * Relaxed memory order is sufficient here. If the task is being pushed in parallel
     * and it's already pushed but I receive an old state, the behavior is as if I
     * was scheduled to run before the task got pushed;
     * If the task is not yet pushed but I receive the pushed state, there is no problem
     * in subscribing to the state early.
     */
    if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_STATE_CREATED)
        return TPOOL_ERR_TASK_NOT_PUSHED;

    int err = futexp_wait_for(&task->state, TASK_STATE_COMPLETED);
    assert(!err);

    bool succ = atomic_cex_state(task, TASK_STATE_COMPLETED, TASK_STATE_JOINED);
    assert(succ);  /* Task must not transition once completed until joined. */

    /* No need to synchronize after the task has finished */
    *result = task->ret;

    return 0;
}

#ifdef NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
    /* Relaxed memory order is sufficient here. See `thread_task_join` */
    if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) == TASK_STATE_CREATED)
        return TPOOL_ERR_TASK_NOT_PUSHED;

    struct timespec ttm = {};
    struct timespec *ttmp = &ttm;
#ifdef INFINITY
    if (timeout == INFINITY)
        ttmp = NULL;
    else
#endif
    if (timeout == DBL_MAX)
        ttmp = NULL;
    else if (timeout > 0) {
        ttm.tv_sec = (time_t)timeout;
        ttm.tv_nsec = (long)((timeout - ttm.tv_sec) * 1e9);
    }

    int err = futexp_timed_wait_for(&task->state, TASK_STATE_COMPLETED, ttmp);
    if (err != 0) {
        assert(errno == ETIMEDOUT);
        return TPOOL_ERR_TIMEOUT;
    }

    bool succ = atomic_cex_state(task, TASK_STATE_COMPLETED, TASK_STATE_JOINED);
    assert(succ);  /* Task must not transition once completed until joined. */

    /* No need to synchronize once the task has finished */
    *result = task->ret;

    return 0;
}

#endif

#ifdef NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
    /* Warning: the checks order is important */
    if (__atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_STATE_CREATED) {
        return TPOOL_ERR_TASK_NOT_PUSHED;
    } else if (atomic_cex_state(task, TASK_STATE_PUSHED, TASK_STATE_PUSHED_GHOST)) {
        return 0;
    } else if (atomic_cex_state(task, TASK_STATE_RUNNING, TASK_STATE_RUNNING_GHOST)) {
        return 0;
    } else if (atomic_cex_state(task, TASK_STATE_COMPLETED, TASK_STATE_JOINED)) {
        thread_task_delete(task);
        return 0;
    } else {
        assert(false);  // Other states/transitions are impossible
    }
}

#endif
