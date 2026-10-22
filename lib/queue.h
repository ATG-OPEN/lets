/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file queue.h
 * @brief Type-generic ring queue (FIFO), declared by a macro.
 *
 * DECLARE_RING_QUEUE_TYPE(NAME, TYPE, CAPACITY) declares a queue type
 * `NAME_t` holding up to CAPACITY elements of TYPE, and its functions
 * `NAME_init`, `NAME_push`, `NAME_pop`, ... as static inline functions. No
 * allocation: the storage is inside the struct.
 *
 * @code
 * DECLARE_RING_QUEUE_TYPE(rxq, uint8_t, 64)  // rxq_t: 64 bytes
 *
 * static rxq_t rx;
 * rxq_init(&rx);
 *
 * rxq_push(&rx, byte);                       // e.g. in the UART interrupt
 *
 * uint8_t b;
 * while (rxq_pop(&rx, &b))                   // in a task
 *         handle(b);
 * @endcode
 *
 * The queue itself has no locking: when one side runs in an interrupt,
 * disable that interrupt around the other side's calls.
 */

#ifndef GENERIC_RING_QUEUE_H
#define GENERIC_RING_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Declares the queue type NAME_t and its functions.
 *
 * - NAME_init(q): empties the queue.
 * - NAME_is_empty(q), NAME_is_full(q), NAME_size(q).
 * - NAME_push(q, value): adds one element; false when full.
 * - NAME_push_array(q, src, count): adds up to count; returns the number added.
 * - NAME_pop(q, &out): takes the oldest element; false when empty. out may be
 *   NULL to drop it.
 * - NAME_pop_array(q, dst, count): takes up to count; returns the number taken.
 */
#define DECLARE_RING_QUEUE_TYPE(NAME, TYPE, CAPACITY)                                      \
                                                                                           \
	typedef struct {                                                                   \
		TYPE buffer[CAPACITY];                                                     \
		size_t head; /* next element to read */                                    \
		size_t tail; /* next free place */                                         \
		size_t size; /* elements stored */                                         \
	} NAME##_t;                                                                        \
                                                                                           \
	static inline void NAME##_init(NAME##_t *q)                                        \
	{                                                                                  \
		q->head = 0;                                                               \
		q->tail = 0;                                                               \
		q->size = 0;                                                               \
	}                                                                                  \
                                                                                           \
	static inline bool NAME##_is_empty(const NAME##_t *q)                              \
	{                                                                                  \
		return q->size == 0;                                                       \
	}                                                                                  \
                                                                                           \
	static inline bool NAME##_is_full(const NAME##_t *q)                               \
	{                                                                                  \
		return q->size == CAPACITY;                                                \
	}                                                                                  \
                                                                                           \
	static inline size_t NAME##_size(const NAME##_t *q)                                \
	{                                                                                  \
		return q->size;                                                            \
	}                                                                                  \
                                                                                           \
	static inline bool NAME##_push(NAME##_t *q, TYPE value)                            \
	{                                                                                  \
		if (q->size == CAPACITY)                                                   \
			return false;                                                      \
		q->buffer[q->tail] = value;                                                \
		q->tail = (q->tail + 1) % CAPACITY;                                        \
		q->size++;                                                                 \
		return true;                                                               \
	}                                                                                  \
                                                                                           \
	static inline size_t NAME##_push_array(NAME##_t *q, const TYPE *src, size_t count) \
	{                                                                                  \
		size_t written = 0;                                                        \
		while (written < count && q->size < CAPACITY) {                            \
			q->buffer[q->tail] = src[written];                                 \
			q->tail = (q->tail + 1) % CAPACITY;                                \
			q->size++;                                                         \
			written++;                                                         \
		}                                                                          \
		return written;                                                            \
	}                                                                                  \
                                                                                           \
	static inline bool NAME##_pop(NAME##_t *q, TYPE *out)                              \
	{                                                                                  \
		if (q->size == 0)                                                          \
			return false;                                                      \
		if (out)                                                                   \
			*out = q->buffer[q->head];                                         \
		q->head = (q->head + 1) % CAPACITY;                                        \
		q->size--;                                                                 \
		return true;                                                               \
	}                                                                                  \
                                                                                           \
	static inline size_t NAME##_pop_array(NAME##_t *q, TYPE *dst, size_t count)        \
	{                                                                                  \
		size_t read = 0;                                                           \
		while (read < count && q->size > 0) {                                      \
			dst[read] = q->buffer[q->head];                                    \
			q->head = (q->head + 1) % CAPACITY;                                \
			q->size--;                                                         \
			read++;                                                            \
		}                                                                          \
		return read;                                                               \
	}

#endif /* GENERIC_RING_QUEUE_H */
