/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file alloc.h
 * @brief Fixed heap with per-task ownership.
 *
 * The kernel provides malloc() and free() itself, from one static buffer.
 * Every block can have an owner (a task): when the task stops, the scheduler
 * frees all blocks it owns, so a finished task can never leak memory.
 *
 * The heap is declared once in the application:
 * @code
 * ALLOC_INIT_HEAP(4096);    // in main.c: a 4 KiB heap
 * @endcode
 *
 * Allocation is first fit over at most MAX_BLOCK_COUNT blocks; sizes are
 * rounded up to ALLOC_ALIGN (2 bytes on the MCU, where odd word addresses
 * trap; 8 in a host build).
 */

#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Block alignment: 2 bytes on a 16-bit core (odd addresses trap); on a PC
 * (host build) 8, so blocks can hold pointers and 64-bit values.
 */
#if defined(__XC16__)
#define ALLOC_ALIGN 2u
#else
#define ALLOC_ALIGN 8u
#endif

/**
 * @brief Defines the heap buffer. Use exactly once, in one C file.
 * @param SIZE Heap size in bytes.
 */
#define ALLOC_INIT_HEAP(SIZE)                                              \
	uint8_t alloc_heap[(SIZE)] __attribute__((aligned(ALLOC_ALIGN))); \
	const size_t alloc_heap_length = (SIZE)

extern uint8_t alloc_heap[];	       /**< The heap, from ALLOC_INIT_HEAP(). */
extern const size_t alloc_heap_length; /**< Its size in bytes. */

#define MAX_BLOCK_COUNT	 64u  /**< Most blocks allocated at the same time. */
#define ALLOC_OWNER_NONE (-1) /**< Owner of a block no task owns. */

/** One allocated block. */
typedef struct {
	int owner;     /**< Task index, or ALLOC_OWNER_NONE. */
	size_t offset; /**< Start, in bytes from the start of the heap. */
	size_t size;   /**< Size in bytes; 0 marks an unused entry. */
} block_t;

/** @brief Returns the free bytes (the sum of all gaps). */
size_t alloc_free_size(void);

/** @brief Returns the heap size given to ALLOC_INIT_HEAP(). */
size_t alloc_heap_size(void);

/** @brief Returns the bytes owned by task @p owner. */
size_t alloc_get_owner_size(int owner);

/**
 * @brief Frees every block owned by task @p owner.
 * @return The number of blocks freed.
 */
int alloc_free_owner(int owner);

/**
 * @brief Gives the block that starts at @p ptr to task @p newowner.
 * @return 0, or -1 when @p ptr is not the start of an allocated block.
 */
int alloc_change_owner(void *ptr, int newowner);

/** @brief Prints every allocated block (slot, owner, offset, size). */
void alloc_map(void);

/** @brief Prints a summary: used, free, block count, largest free gap. */
void alloc_info(void);

#endif /* ALLOC_H */
