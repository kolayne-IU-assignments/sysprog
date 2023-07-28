#pragma once

#include <stddef.h>

struct partial_message_queue {
	/// Pointer to allocated memory. The pointer value shall not change.
	char *base;
	/// Pointer shifted `base` to where the next read shall happen.
	char *read;
	/// Capacity of allocated memory (corresponds to `base`).
	size_t capacity;
};

void pmq_init(struct partial_message_queue *pmq, size_t init_cap);

void pmq_destroy(struct partial_message_queue *pmq);

/**
 * Returns pointer to a NULL-terminated string that represents exactly one message
 * (without the trailing `'\n'`) or `NULL` if there are no complete messages.
 *
 * Pointers returned by this method are invalidated on the next `pmq_put` operation.
 */
const char *pmq_next_message(struct partial_message_queue *pmq);

/**
 * Copies the given buffer (which may be one lf-terminated message, or several
 * messages, or a partial message, or several message with last one being partial)
 * to the queue. If there are `'\0'` characters in the range [`buf`; `buf+count`),
 * the behavior is undefined.
 *
 * Invalidates all pointers previously returned by `pmq_next_message`.
 */
void pmq_put(struct partial_message_queue *pmq, const char *buf, size_t count);
