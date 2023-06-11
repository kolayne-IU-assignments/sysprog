#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libcoro.h"

#define SWAP(type, a, b) { type swap_tmp = (a); (a) = (b); (b) = swap_tmp; }

/**
 * Merge function. Can be used both for merging two already sorted arrays, when given subsort=false,
 * and for performing a complete merge sort (O(N*logN)) of an array, when given subsort=true.
 *
 * If subsort=true, the original arrays is reordered somehow (not necessarily sorted).
 * If subsort=false, the original arrays is unmodified.
 *
 * WARNING: requires that libcoro has been initialized with `coro_sched_init()` before `merge` is called.
 */
void merge(int *out, int *from1, int len1, int *from2, int len2, bool subsort) {
    if (subsort) {
        if (len1 > 1) {
            int *tmp = malloc(sizeof (int) * len1);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return;
            }
            merge(tmp, from1, len1/2, from1 + len1/2, len1 - len1/2, subsort);
            memcpy(from1, tmp, sizeof (int) * len1);
            free(tmp);
        }
        if (len2 > 1) {
            int *tmp = malloc(sizeof (int) * len2);
            if (!tmp) {
                perror("Temp array malloc inside merge");
                return;
            }
            merge(tmp, from2, len2/2, from2 + len2/2, len2 - len2/2, subsort);
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
        
        coro_yield();  // Yield on every iteration
    }

    while (i < from1 + len1) {
        *out++ = *i++;
        coro_yield();  // Yield on every iteration
    }
    while (j < from2 + len2) {
        *out++ = *j++;
        coro_yield();  // Yield on every iteration
    }
}

int _test_merge_main() {
    int arr[] = {1, 5, -9, -27, 2, 6};
    int sorted[(sizeof arr) / sizeof (int)];
    merge(sorted, arr, (sizeof arr) / sizeof (int), NULL, 0, true);
    for (int i = 0; i < (sizeof arr) / sizeof (int); ++i) {
        printf("%d ", arr[i]);
    }
    puts("");
    for (int i = 0; i < (sizeof arr) / sizeof (int); ++i) {
        printf("%d ", sorted[i]);
    }
    puts("");

    return 0;
}

struct cor_res {
    int *array;
    int arr_size;
    int switch_count;
};

static long long
sort_file(void *data)
{
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

    printf("read %d numbers from %s\n", arr_size, filename);

    int *sorted = malloc(sizeof (int) * arr_size);
    if (sorted == NULL) {
        perror("malloc for sorted array");
        return -1;
    }
    merge(sorted, array, arr_size, NULL, 0, true);
    
    free(array);

    struct cor_res *res = malloc(sizeof (struct cor_res));
    if (res == NULL) {
        perror("malloc for struct cor_res");
        return -1;
    }

    res->array = sorted;
    res->arr_size = arr_size;
    res->switch_count = coro_switch_count(coro_this());
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
        results[res_idx] = *res;
        free(res);
        printf("Finished: %d numbers sorted\n", results[res_idx].arr_size);
        res_idx++;
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
        merge(sorted1, sorted2, len2, results[i].array, results[i].arr_size, false);
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
