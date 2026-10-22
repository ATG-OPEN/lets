/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "uuid.h"
#include <xc.h>
#include <stdint.h>

/* the five UDID words, read from program memory; `out` holds 5 values */
static void udid_read(uint32_t *out)
{
	TBLPAG = 0x80; /* UDID page (check your device's programming spec) */

	out[0] = __builtin_tblrdl(0x1600);
	out[0] |= ((uint32_t)__builtin_tblrdh(0x1600)) << 16;

	out[1] = __builtin_tblrdl(0x1602);
	out[1] |= ((uint32_t)__builtin_tblrdh(0x1602)) << 16;

	out[2] = __builtin_tblrdl(0x1604);
	out[2] |= ((uint32_t)__builtin_tblrdh(0x1604)) << 16;

	out[3] = __builtin_tblrdl(0x1606);
	out[3] |= ((uint32_t)__builtin_tblrdh(0x1606)) << 16;

	out[4] = __builtin_tblrdl(0x1608);
	out[4] |= ((uint32_t)__builtin_tblrdh(0x1608)) << 16;
}

uint32_t get_unique_id(void)
{
	uint32_t udid[5];
	uint32_t id;
	int i;

	udid_read(udid);

	/* xor the words together, rotating so each word lands on other bits */
	id = 0;
	for (i = 0; i < 5; i++) {
		id ^= udid[i];
		id = (id << 5) | (id >> (32 - 5));
	}

	return id;
}
