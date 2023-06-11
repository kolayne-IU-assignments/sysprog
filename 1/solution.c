#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libcoro.h"

#define SWAP(type, a, b) { type swap_tmp = (a); (a) = (b); (b) = swap_tmp; }

inline static struct timespec must_clock_monotonic() {
    struct timespec res;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &res)) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        exit(1);  // If failed, terminate
    }
    return res;
}

inline static struct timespec timespec_from_double (double sec) {
    long long tv_sec = sec;
    struct timespec res = { .tv_sec = tv_sec, .tv_nsec = (sec - tv_sec) * 1e9 };
    return res;
}

inline static struct timespec timespec_add(const struct timespec a, const struct timespec b) {
    unsigned long long nsec = a.tv_nsec + b.tv_nsec;
    struct timespec sum = { .tv_sec = a.tv_sec + b.tv_sec, .tv_nsec = nsec };
    if (nsec > 1000 * 1000 * 1000) {
        sum.tv_sec--;
        sum.tv_nsec = nsec - 1000*1000*1000;
    }
    return sum;
}

inline static struct timespec timespec_diff(const struct timespec a, const struct timespec b) {
    struct timespec diff = { .tv_sec = a.tv_sec - b.tv_sec, .tv_nsec = a.tv_nsec - b.tv_nsec };
    if (diff.tv_nsec < 0) {
        diff.tv_nsec += 1000*1000*1000;
        diff.tv_sec--;
    }
    return diff;
}

inline static bool timespec_less(const struct timespec a, const struct timespec b) {
    if (a.tv_sec == b.tv_sec)
        return a.tv_nsec < b.tv_nsec;
    return a.tv_sec < b.tv_sec;
}

struct timespec coro_yield_timered() {
    struct timespec start = must_clock_monotonic();
    coro_yield();
    struct timespec stop = must_clock_monotonic();
    return timespec_diff(stop, start);
}

/**
 * Merge function. Can be used both for merging two already sorted arrays, when given subsort=false,
 * and for performing a complete merge sort (O(N*logN)) of an array, when given subsort=true.
 *
 * If subsort=true, the original arrays is reordered somehow (not necessarily sorted).
 * If subsort=false, the original arrays is unmodified.
 *
 * WARNING: requires that libcoro has been initialized with `coro_sched_init()` before `merge` is called.
 *
 * Returns a `struct timespec` that represents the total time spent sleeping in `coro_yield`.
 */
struct timespec merge(int *out, int *from1, int len1, int *from2, int len2,
        bool subsort, struct timespec latency) {

    // Used to determine when to switch
    struct timespec next_switch = timespec_add(must_clock_monotonic(), latency);

    // Used to accumulate the waiting time (for the return value)
    struct timespec wait_time = {0};

    if (subsort) {
        if (len1 > 1) {
            int *tmp = malloc(sizeof (int) * len1);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return wait_time;
            }
            struct timespec slept = merge(tmp, from1, len1/2, from1 + len1/2, len1 - len1/2, subsort, latency);
            wait_time = timespec_add(wait_time, slept);
            memcpy(from1, tmp, sizeof (int) * len1);
            free(tmp);
        }
        if (len2 > 1) {
            int *tmp = malloc(sizeof (int) * len2);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return wait_time;
            }
            struct timespec slept = merge(tmp, from2, len2/2, from2 + len2/2, len2 - len2/2, subsort, latency);
            wait_time = timespec_add(wait_time, slept);
            memcpy(from2, tmp, sizeof (int) * len2);
            free(tmp);
        }
    }

    const int *i = from1, *j = from2;
    while (i < from1 + len1 && j < from2 + len2) {
        int mn;
        if (*i < *j)
            mn = *i++;
        else
            mn = *j++;

        *out++ = mn;

        // If it's time to, yield, adding the wait time to `wait_time` and updating `next_switch`
        if (timespec_less(next_switch, /*now*/must_clock_monotonic())) {
            wait_time = timespec_add(wait_time, coro_yield_timered());
            next_switch = timespec_add(must_clock_monotonic(), latency);
        }
    }

    while (i < from1 + len1) {
        *out++ = *i++;
        // If it's time to, yield, adding the wait time to `wait_time` and updating `next_switch`
        if (timespec_less(next_switch, /*now*/must_clock_monotonic())) {
            wait_time = timespec_add(wait_time, coro_yield_timered());
            next_switch = timespec_add(must_clock_monotonic(), latency);
        }
    }
    while (j < from2 + len2) {
        *out++ = *j++;
        // If it's time to, yield, adding the wait time to `wait_time` and updating `next_switch`
        if (timespec_less(next_switch, /*now*/must_clock_monotonic())) {
            wait_time = timespec_add(wait_time, coro_yield_timered());
            next_switch = timespec_add(must_clock_monotonic(), latency);
        }
    }

    return wait_time;
}


struct sort_file_inp {
    int worker_id;
    // `filename` set to `NULL` represents that the worker is waiting for a file;
    // `filename` set to `(char *)-1` represents a worker in an invalid state (uninitialized / terminated);
    // other values of `filename` should be treated naturally.
    char *filename;
    struct timespec latency;

    int *array;
    int arr_size;
};

struct sort_file_res {
    int worker_id;
    int switch_count;
    struct timespec time_spent;
};

static long long
sort_file(void *data)
{
    struct timespec start = must_clock_monotonic();

    struct sort_file_inp *dnp = (struct sort_file_inp *)data;

    (void)fprintf(stderr, "Worker %d has entered sort_file()\n", dnp->worker_id);

    dnp->filename = NULL;  // Initialized.

    while (1) {
        coro_yield();

        if (dnp->filename == NULL) {
            // Still not assigned, which means there are no files left. Nothing to be done
            (void)fprintf(stderr, "Worker %d didn't receive a file. Terminating\n", dnp->worker_id);
            dnp->filename = (char *)-1;  // Termination indication
            break;
        } else {
            (void)fprintf(stderr, "Worker %d got file %s. Starting the work\n", dnp->worker_id, dnp->filename);
        }

        FILE *f = fopen(dnp->filename, "r");
        if (f == NULL) {
            perror("fopen of input file");
            return -1;
        }

        // The previous values of array and arr_size were taken by distributor
        int *unsorted = NULL;
        int arr_size = 0;
        int arr_idx = 0;

        int val;
        while(1 == fscanf(f, "%d", &val)) {
            if (arr_idx >= arr_size) {
                arr_size = 1 + arr_size * 2;
                unsorted = reallocarray(unsorted, sizeof (int), arr_size);
                if (unsorted == NULL) {
                    perror("reallocarray");
                    return -1;
                }
            }
            unsorted[arr_idx++] = val;
        }

        (void)fprintf(stderr, "Worker %d has read %d numbers\n", dnp->worker_id, arr_idx);

        dnp->array = malloc(sizeof (int) * arr_idx);
        if (dnp->array == NULL) {
            perror("malloc for new dnp->sorted array");
            return -1;
        }

        // Truncate the unread numbers. There is no memory safety problem: libc still remembers
        // the actual amount of memory to free.
        dnp->arr_size = arr_idx;

        struct timespec wait_time = merge(dnp->array, unsorted, arr_idx, NULL, 0, true, dnp->latency);

        free(unsorted);

        // Shift start time as if there was no waiting
        start = timespec_add(start, wait_time);

        (void)fprintf(stderr, "Worker %d has finished processing %s\n", dnp->worker_id, dnp->filename);
        dnp->filename = NULL;  // Signal that I want the next file
    }

    struct timespec stop = must_clock_monotonic();

    struct sort_file_res *res = malloc(sizeof (struct sort_file_res));
    if (res == NULL) {
        perror("malloc for struct sort_file_res");
        return -1;
    }

    res->worker_id = dnp->worker_id;
    res->switch_count = coro_switch_count(coro_this());
    res->time_spent = timespec_diff(stop, start);
    return (long long)res;
}


struct distributor_inp {
    struct sort_file_inp *dnps;
    int workers_count;

    char **filenames;
    size_t files_count;

    // At least `files_count` elements must be allocated for `resulting_arrays*`
    int **resulting_arrays;
    int *resulting_arrays_sizes;
};

long long distributor(void *data) {
    /*
     * The idea is as follows. My (patched) version of libcoro guarantees that the coroutines are
     * executed in the same order, wrapping (i.e. round robin manner where coroutines never only
     * join or quit, never swap). If a coroutine returns, the control is first handed to the
     * scheduler, then passed onto the former-next of the returned coroutine.
     *
     * When a worker coroutine is done processing a file, it signals so by setting its
     * `input->filename` to `NULL` and gives control to the next coroutine. When the yields wrap,
     * the execution reaches the distributor coroutine (this one), which sets the `filename`s for
     * all workers which are ready. The arrays produced by the worker coroutines are collected by
     * `distributor` to its input's `->resulting_arrays`.
     *
     * When the distributor is out of files, it continues to collect the sorted arrays, counting
     * each worker getting free. As they are not given new files, the free workers will terminate,
     * **setting `->filename` to `-1`** (that is to make it simpler to distinguish between the
     * coroutines which just requested a new file from the ones that already terminated without
     * getting one previously).
     * The distributor counts the number of terminated workers and, when they are all finished,
     * returns `0LL`.
     */

    struct distributor_inp *input = (struct distributor_inp *)data;

    int alive_workers_count = input->workers_count;

    size_t next_file = 0;
    size_t next_res = 0;

    while (alive_workers_count) {
        for (int i = 0; i < input->workers_count; ++i) {
            // 0. Find a worker that is in a valid state and wants a file
            if (input->dnps[i].filename == NULL) {
                // 1. If there is a new result (i.e. not the very first round of worker), store it
                if (input->dnps[i].array != NULL) {
                    input->resulting_arrays[next_res] = input->dnps[i].array;
                    input->resulting_arrays_sizes[next_res] = input->dnps[i].arr_size;
                    ++next_res;
                }

                // 2. If there is a file to process, give it to the worker, otherwise
                // consider it terminated.
                if (next_file < input->files_count) {
                    input->dnps[i].filename = input->filenames[next_file++];
                } else {
                    alive_workers_count--;
                }
            }
        }

        coro_yield();
    }

    return 0;
}


void output_arr(const int *arr, int size);

int
main(int argc, char **argv)
{
    _Static_assert (sizeof (long long) == sizeof (void *), "`long long` is expected to be pointer-sized: "
            "coroutine functions can't return pointers");

    if (argc <= 3) {
        fputs("Too few command-line arguments\n", stderr);
        return 1;
    }

    int files_count = argc - 3;

    char *parse_check;
    long workers_count = strtol(argv[2], &parse_check, 10);
    if (*parse_check) {
        fputs("Error: the second command-line argument must be an integer workers count", stderr);
        return 3;
    }

    double target_latency_sec = strtod(argv[1], &parse_check);
    if (*parse_check) {
        fputs("Error: the first command-line argument must be a floating-point target latency value", stderr);
        return 2;
    }
    double latency_sec = target_latency_sec / workers_count;
    printf("Each worker will be given the %f latency\n", latency_sec);

    struct timespec latency = timespec_from_double(latency_sec);

    /* Initialize our coroutine global cooperative scheduler. */
    coro_sched_init();

    struct sort_file_inp inputs[workers_count];
    for (int i = 0; i < workers_count; ++i) {
        inputs[i].filename = (char *)-1;  // Worker is in invalid state: not yet initialized
        inputs[i].worker_id = i;  // Only used for logging
        inputs[i].latency = latency;
        inputs[i].array = NULL;
        inputs[i].arr_size = 0;
        coro_new(sort_file, (void *)&inputs[i]);
    }

    int *resulting_arrays[files_count];
    int resulting_arrays_sizes[files_count];

    struct distributor_inp distr_inp = { .dnps = inputs, .workers_count = workers_count,
        .filenames = argv + 3, .files_count = files_count, .resulting_arrays = resulting_arrays,
        .resulting_arrays_sizes = resulting_arrays_sizes};

    coro_new(distributor, (void *)&distr_inp);

    /* Wait for all the coroutines to end. */
    struct coro *c;
    while ((c = coro_sched_wait()) != NULL) {
        long long status = coro_status(c);
        if (status == -1) {
            (void)printf("Error: a coroutine terminated with an error\n");
        } else if (status == 0) {
            (void)printf("Distributor has terminated\n");
        } else {
            struct sort_file_res *res = (struct sort_file_res *)status;
            (void)printf("Coroutine %d finished in %lld.%.9ld seconds with %d switches\n",
                    res->worker_id, (long long)res->time_spent.tv_sec, res->time_spent.tv_nsec,
                    res->switch_count);
            free(res);
        }
        coro_delete(c);
    }

    // Total merge. The arrays are added one by one to the previously merged part
    // (poined to by sorted2) and saved as the new merged part (pointed to by sorted1).
    // Then they are swapped.

    long long total = 0;
    for (int i = 0; i < files_count; ++i) {
        total += resulting_arrays_sizes[i];
    }

    int sorted1_[total];
    int sorted2_[total];

    int *sorted1 = sorted1_, *sorted2 = sorted2_;
    int len1 = 0, len2 = 0;

    for (int i = 0; i < files_count; ++i) {
        (void)merge(sorted1, sorted2, len2, resulting_arrays[i], resulting_arrays_sizes[i], false, latency);
        len1 = resulting_arrays_sizes[i] + len2;
        free(resulting_arrays[i]);  // Won't use this again
        SWAP(int *, sorted1, sorted2);
        SWAP(int, len1, len2);
    }

    // Now the sorted array is in `sorted2`
    output_arr(sorted2, len2);

    return 0;
}

void output_arr(const int *arr, int size) {
    FILE *f = fopen("out.txt", "w");
    if (f == NULL) {
        perror("fopen out.txt");
        return;
    }

    for (int i = 0; i < size; ++i) {
        (void)fprintf(f, "%d ", arr[i]);
    }

    (void)fseek(f, -1, SEEK_CUR);
    (void)fputc('\n', f);
    (void)fclose(f);
}
