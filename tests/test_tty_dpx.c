/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * opt/tty_dpx.h on a straight full-duplex cable: two ends start in the same direction (both
 * DPX1, as two ports of one board after the rc); each sends a PING every 1000 ms and the other
 * answers at once (a DRN link); bytes pass only while their directions are opposite. They must
 * find each other, and never swap again while the PINGs go on. Two ends with the same seed show
 * what the per-port seed and the clock noise are for: without noise they swap together for
 * ever; with each end's us clock stirred in they link anyway.
 */

#include "../opt/tty_dpx.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
	tty_dpx_t s;
	int dir; /* 0, 1: DPX1, DPX2 */
	uint32_t next_ping;
} end_t;

static int failures;

/*
 * ms until the two heard each other (0: not within limit_ms); swaps after that in *late.
 * noise: the ends read their port this many us apart (0: no clock noise at all).
 */
static uint32_t play(uint16_t seed_a, uint16_t seed_b, uint32_t start_b, uint32_t limit_ms,
		     unsigned int noise, unsigned int *late)
{
	end_t e[2] = {{.dir = 0, .next_ping = 0}, {.dir = 0, .next_ping = start_b}};
	uint32_t found = 0;

	tty_dpx_init(&e[0].s, seed_a, 0);
	tty_dpx_init(&e[1].s, seed_b, start_b);
	*late = 0;

	for (uint32_t now = 0; now < limit_ms; now++) {
		for (int i = 0; i < 2; i++) {
			if (i == 1 && now < start_b) {
				continue;
			}

			if (now >= e[i].next_ping) {
				e[i].next_ping = now + 1000u;

				/* the PING is heard, and its answer */
				if (e[0].dir != e[1].dir) {
					tty_dpx_heard(&e[1 - i].s, now);
					tty_dpx_heard(&e[i].s, now);

					if (found == 0) {
						found = (now == 0) ? 1 : now;
					}
				}
			}

			uint16_t us = 0;

			if (noise != 0) {
				us = (uint16_t)(now * 1000u + (i ? noise : 0) + (unsigned int)(rand() % 50));
			}

			if (tty_dpx_due(&e[i].s, now, us)) {
				e[i].dir ^= 1;

				if (found != 0) {
					(*late)++;
				}
			}
		}
	}

	return found;
}

int main(void)
{
	uint32_t worst = 0;
	uint64_t sum = 0;
	unsigned int runs = 0, late;

	srand(1);

	/*
	 * Two ports of one board, as port/pic24/tty.c seeds them: tty_args_t a few dozen bytes apart,
	 * ports 2 and 3, the us clock a little later for the second; started in the same ms or two.
	 */
	for (unsigned int k = 0; k < 2000; k++) {
		uint16_t a = (uint16_t)(0x1000u + 2u * (unsigned int)(rand() % 8000));
		uint16_t b = (uint16_t)(a + 2u * (unsigned int)(1 + rand() % 40));
		uint16_t us = (uint16_t)rand();
		uint16_t seed_a = (uint16_t)(a ^ (2u << 8) ^ us);
		uint16_t seed_b = (uint16_t)(b ^ (3u << 8) ^ (uint16_t)(us + 20u + rand() % 400));
		uint32_t t = play(seed_a, seed_b, (uint32_t)(rand() % 3), 600000u,
				  20u + (unsigned int)(rand() % 400), &late);

		if (t == 0 || late != 0) {
			printf("FAIL seeds %04X %04X: linked at %lu ms, %u swaps after\n", seed_a,
			       seed_b, (unsigned long)t, late);
			failures++;
			continue;
		}

		worst = (t > worst) ? t : worst;
		sum += t;
		runs++;
	}

	printf("tty_dpx: 2000 cable starts: linked after %lu ms on average, %lu ms at worst, "
	       "no swap once linked\n",
	       (unsigned long)(runs ? sum / runs : 0), (unsigned long)worst);

	if (worst > 60000u) {
		printf("FAIL worst %lu ms > 60 s\n", (unsigned long)worst);
		failures++;
	}

	/* the same seed and tick, no noise: lock-step, never opposite */
	if (play(0x1234, 0x1234, 0, 600000u, 0, &late) != 0) {
		printf("FAIL equal seeds linked: the model does not show the lock-step\n");
		failures++;
	} else {
		printf("tty_dpx: equal seeds, same tick, no noise: never linked in 10 min (lock-step)\n");
	}

	/* the same seed and tick, but each end's us clock: they link */
	uint32_t t = play(0x1234, 0x1234, 0, 600000u, 100, &late);

	if (t == 0 || t > 60000u || late != 0) {
		printf("FAIL equal seeds with noise: linked at %lu ms\n", (unsigned long)t);
		failures++;
	} else {
		printf("tty_dpx: equal seeds, same tick, with the us clock: linked after %lu ms\n",
		       (unsigned long)t);
	}

	printf(failures ? "test_tty_dpx: %d FAILED\n" : "test_tty_dpx: ok\n", failures);

	return failures ? 1 : 0;
}
