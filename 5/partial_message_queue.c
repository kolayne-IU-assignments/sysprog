#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>

#include "partial_message_queue.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

void pmq_init(struct partial_message_queue *pmq, size_t init_cap) {
	pmq->base = malloc(sizeof (char) * init_cap);
	if (!pmq->base)
		abort();
	pmq->read = pmq->base;
	pmq->read[0] = '\0';
	pmq->capacity = init_cap;
}

void pmq_destroy(struct partial_message_queue *pmq) {
	free(pmq->base);
}

static void pmq_rebuf(struct partial_message_queue *pmq, size_t min_len) {
	size_t new_sz = MAX(min_len, 2 * pmq->capacity);
	size_t read_shift = pmq->read - pmq->base;
	pmq->base = realloc(pmq->base, sizeof (char) * new_sz);
	if (!pmq->base)
		abort();
	pmq->read = pmq->base + read_shift;
}

char *pmq_next_message(struct partial_message_queue *pmq) {
	char *lf = strchrnul(pmq->read, '\n');
	if (*lf) {
		*lf = '\0';
		char *ret = pmq->read;
		pmq->read = lf + 1;
		return ret;
	}
	return NULL;
}

void pmq_put(struct partial_message_queue *pmq, const char *buf, size_t put_len) {
	size_t readable_len = strlen(pmq->read);

	/* Shift everything back to use all the available space */
	(void)memmove(pmq->base, pmq->read, readable_len + 1);
	pmq->read = pmq->base;

	if (readable_len + put_len + 1 > pmq->capacity) {
		pmq_rebuf(pmq, readable_len + put_len + 1);
	}

	memcpy(pmq->read + readable_len, buf, put_len);
	pmq->read[readable_len + put_len] = '\0';
}
