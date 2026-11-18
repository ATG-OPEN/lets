/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "uuid.h"
#include <xc.h>
#include <stdint.h>

#if defined(__dsPIC33C__)
#define UUID_UDID 0x1200u /* dsPIC33CK */
#else
#define UUID_UDID 0x1600u /* PIC24FJ */
#endif

/* the five UDID words, read from program memory; `out` holds 5 values */
static void udid_read(uint32_t *out)
{
	int i;

	TBLPAG = 0x80;

	for (i = 0; i < 5; i++) {
		out[i] = __builtin_tblrdl(UUID_UDID + 2u * i);
		out[i] |= ((uint32_t)__builtin_tblrdh(UUID_UDID + 2u * i)) << 16;
	}
}

uint32_t get_unique_id(void)
{
	uint32_t udid[5];
	uint32_t id = 2166136261ul; /* FNV-1a */
	uint16_t page = TBLPAG;
	int i, j;

	udid_read(udid);
	TBLPAG = page;

	/*
	 * FNV-1a over the 15 bytes, not a xor of the words: the UDID holds small
	 * numbers (die position, wafer), and with a xor two chips differing by
	 * one bit in two words cancel out and get the same id.
	 */
	for (i = 0; i < 5; i++) {
		for (j = 0; j < 3; j++) {
			id ^= (udid[i] >> (8 * j)) & 0xFFu;
			id *= 16777619ul;
		}
	}

	/* 0 and all ones are not addresses (nmac: none, broadcast) */
	if (id == 0ul || id == 0xFFFFFFFFul) {
		id = 0x5A5A5A5Aul;
	}

	return id;
}
