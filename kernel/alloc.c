/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Heap: a table of blocks over the static buffer from ALLOC_INIT_HEAP().
 * malloc() and free() replace the C library's; XC16 links them from the
 * sections named below.
 */

#include "alloc.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static block_t blocks[MAX_BLOCK_COUNT];

/* -------------------------------------------------------------------------- */

/* index of the allocated block that starts at ptr, -1 if there is none */
static int alloc_find(void *ptr)
{
	if (ptr == NULL || (uint8_t *)ptr < alloc_heap) {
		return -1;
	}

	size_t offset = (uint8_t *)ptr - alloc_heap;

	if (offset >= alloc_heap_length) {
		return -1;
	}

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size > 0 && blocks[i].offset == offset) {
			return i;
		}
	}

	return -1;
}

static void alloc_release(int i)
{
	blocks[i].size = 0;
	blocks[i].owner = ALLOC_OWNER_NONE;
	blocks[i].offset = 0;
}

/* ----------------------- C library replacements --------------------------- */

void *__attribute__((__section__(".libc.malloc"))) malloc(size_t size)
{
	if (size == 0 || size > alloc_heap_length) {
		return NULL;
	}

	/* 2-byte alignment: odd addresses cause address error traps on 16-bit cores */
	if (size % 2 != 0) {
		size++;
	}

	/* a free entry in the block table */
	int slot = -1;

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size == 0) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		return NULL;
	}

	/*
	 * First fit: try the lowest offset; when it overlaps a block, jump to
	 * the end of that block and try again.
	 */
	size_t offset = 0;

	while (offset + size <= alloc_heap_length) {
		bool conflict = false;

		for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
			if (blocks[i].size == 0) {
				continue;
			}

			if (offset < blocks[i].offset + blocks[i].size &&
			    blocks[i].offset < offset + size) {
				offset = blocks[i].offset + blocks[i].size;
				conflict = true;
				break;
			}
		}

		if (conflict == false) {
			blocks[slot].owner = ALLOC_OWNER_NONE;
			blocks[slot].offset = offset;
			blocks[slot].size = size;

			return (void *)&alloc_heap[offset];
		}
	}

	return NULL;
}

void __attribute__((__section__(".libc.free"))) free(void *ptr)
{
	int i = alloc_find(ptr);

	if (i < 0) {
		return;
	}

	alloc_release(i);
}

/* -------------------------------------------------------------------------- */

size_t alloc_free_size(void)
{
	size_t used = 0;

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		used += blocks[i].size;
	}

	return alloc_heap_length - used;
}

size_t alloc_heap_size(void)
{
	return alloc_heap_length;
}

size_t alloc_get_owner_size(int owner)
{
	size_t total = 0;

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size > 0 && blocks[i].owner == owner) {
			total += blocks[i].size;
		}
	}

	return total;
}

int alloc_free_owner(int owner)
{
	if (owner == ALLOC_OWNER_NONE) {
		return 0;
	}

	int count = 0;

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size > 0 && blocks[i].owner == owner) {
			alloc_release(i);
			count++;
		}
	}

	return count;
}

int alloc_change_owner(void *ptr, int newowner)
{
	int i = alloc_find(ptr);

	if (i < 0) {
		return -1;
	}

	blocks[i].owner = newowner;

	return 0;
}

/* -------------------------------------------------------------------------- */

void alloc_map(void)
{
	printf("Total %u B free %u B\n", (unsigned)alloc_heap_length, (unsigned)alloc_free_size());
	printf("SLOT\tOWNER\tOFFSET\tSIZE\n");

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size == 0) {
			continue;
		}

		printf("%u\t%d\t%u\t%u\n", (unsigned)i, blocks[i].owner, (unsigned)blocks[i].offset,
		       (unsigned)blocks[i].size);
	}
}

void alloc_info(void)
{
	size_t used = 0;
	unsigned count = 0;

	for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
		if (blocks[i].size > 0) {
			used += blocks[i].size;
			count++;
		}
	}

	/*
	 * Largest free gap: visit the blocks in order of their offset (each
	 * time the next one at or after `offset`) and measure the gaps.
	 */
	size_t largest = 0;
	size_t offset = 0;

	while (offset < alloc_heap_length) {
		int next = -1;

		for (int i = 0; i < MAX_BLOCK_COUNT; i++) {
			if (blocks[i].size == 0 || blocks[i].offset < offset) {
				continue;
			}

			if (next < 0 || blocks[i].offset < blocks[next].offset) {
				next = i;
			}
		}

		size_t end = (next < 0) ? alloc_heap_length : blocks[next].offset;

		if (end - offset > largest) {
			largest = end - offset;
		}

		if (next < 0) {
			break;
		}

		offset = blocks[next].offset + blocks[next].size;
	}

	printf("heap %u B used %u B free %u B\n", (unsigned)alloc_heap_length, (unsigned)used,
	       (unsigned)(alloc_heap_length - used));
	printf("blocks %u/%u largest free %u B\n", count, (unsigned)MAX_BLOCK_COUNT,
	       (unsigned)largest);
}
