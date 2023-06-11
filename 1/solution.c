#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libcoro.h"

#define SWAP(type, a, b) { type swap_tmp = (a); (a) = (b); (b) = swap_tmp; }

inline static struct timespec timespec_sum(const struct timespec a, const struct timespec b) {
    struct timespec sum = { .tv_sec = a.tv_sec + b.tv_sec, .tv_nsec = a.tv_nsec + b.tv_nsec };
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

struct timespec coro_yield_timered() {
    struct timespec start, stop;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &start)) {
        perror("First clock_gettime(CLOCK_MONOTONIC) in coro_yield_timered");
    }
    coro_yield();
    if (0 != clock_gettime(CLOCK_MONOTONIC, &stop)) {
        perror("Second clock_gettime(CLOCK_MONOTONIC) in coro_yield_timered");
    }
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
struct timespec merge(int *out, int *from1, int len1, int *from2, int len2, bool subsort) {
    struct timespec wait_time = {0};

    if (subsort) {
        if (len1 > 1) {
            int *tmp = malloc(sizeof (int) * len1);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return wait_time;
            }
            struct timespec slept = merge(tmp, from1, len1/2, from1 + len1/2, len1 - len1/2, subsort);
            wait_time = timespec_sum(wait_time, slept);
            memcpy(from1, tmp, sizeof (int) * len1);
            free(tmp);
        }
        if (len2 > 1) {
            int *tmp = malloc(sizeof (int) * len2);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return wait_time;
            }
            struct timespec slept = merge(tmp, from2, len2/2, from2 + len2/2, len2 - len2/2, subsort);
            wait_time = timespec_sum(wait_time, slept);
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
        
        // Yield on every iteration
        wait_time = timespec_sum(wait_time, coro_yield_timered());
    }

    while (i < from1 + len1) {
        *out++ = *i++;
        // Yield on every iteration
        wait_time = timespec_sum(wait_time, coro_yield_timered());
    }
    while (j < from2 + len2) {
        *out++ = *j++;
        // Yield on every iteration
        wait_time = timespec_sum(wait_time, coro_yield_timered());
    }

    return wait_time;
}

struct cor_res {
    int *array;
    int arr_size;
    int switch_count;
    struct timespec time_spent;
};

static long long
sort_file(void *data)
{
    struct timespec start;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &start)) {
        perror("First clock_gettime(CLOCK_MONOTONIC) in sort_file");
        return -1;
    }

    const char *filename = data;
    
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        perror("fopen of input file");
        return -1;
    }

    int *array = NULL;
    int arr_size = 0;
    int arr_idx = 0;

    int val;
    while(1 == fscanf(f, "%d", &val)) {
        if (arr_idx >= arr_size) {
            arr_size = 1 + arr_size * 2;
            array = reallocarray(array, sizeof (int), arr_size);
            if (array == NULL) {
                perror("reallocarray");
                return -1;
            }
        }
        array[arr_idx++] = val;
    }

    // Truncate the unread numbers. There is no memory safety problem: libc still remembers
    // the actual amount of memory to free.
    arr_size = arr_idx;

    (void)printf("read %d numbers from %s\n", arr_size, filename);

    int *sorted = malloc(sizeof (int) * arr_size);
    if (sorted == NULL) {
        perror("malloc for sorted array");
        return -1;
    }
    struct timespec wait_time = merge(sorted, array, arr_size, NULL, 0, true);
    
    free(array);

    struct cor_res *res = malloc(sizeof (struct cor_res));
    if (res == NULL) {
        perror("malloc for struct cor_res");
        return -1;
    }

    struct timespec stop;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &stop)) {
        perror("Second clock_gettime(CLOCK_MONOTONIC) in sort_file");
        return -1;
    }

    res->array = sorted;
    res->arr_size = arr_size;
    res->switch_count = coro_switch_count(coro_this());
    res->time_spent = timespec_diff(timespec_diff(stop, start), wait_time);
    return (long long)res;
}

void output_arr(const int *arr, int size);

int
main(int argc, char **argv)
{
    _Static_assert (sizeof (long long) == sizeof (void *), "`long long` is expected pointer size: "
            "coroutine functions can't return pointers");

    /* Initialize our coroutine global cooperative scheduler. */
    coro_sched_init();

    for (int i = 1; i < argc; ++i) {
        coro_new(sort_file, argv[i]);
    }

    struct cor_res results[argc - 1];
    int res_idx = 0;

    /* Wait for all the coroutines to end. */
    struct coro *c;
    while ((c = coro_sched_wait()) != NULL) {
        struct cor_res *res = (struct cor_res *)coro_status(c);
        if ((long long)res == -1) {
            (void)printf("Error: a coroutine terminated with an error\n");
        } else {
            (void)printf("Coroutine finished. Sorted %d numbers in %lld.%.9ld seconds with %d switches\n",
                    res->arr_size, (long long)res->time_spent.tv_sec, res->time_spent.tv_nsec,
                    res->switch_count);
            results[res_idx] = *res;
            free(res);
            res_idx++;
        }
        coro_delete(c);
    }

    // Total merge. The arrays are added one by one to the previously merged part
    // (poined to by sorted2) and saved as the new merged part (pointed to by sorted1).
    // Then they are swapped.

    long long total = 0;
    for (int i = 0; i < res_idx; ++i) {
        total += results[i].arr_size;
    }

    int sorted1_[total];
    int sorted2_[total];

    int *sorted1 = sorted1_, *sorted2 = sorted2_;
    int len1 = 0, len2 = 0;

    for (int i = 0; i < res_idx; ++i) {
        (void)merge(sorted1, sorted2, len2, results[i].array, results[i].arr_size, false);
        len1 = results[i].arr_size + len2;
        free(results[i].array);  // Won't use this array again
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
