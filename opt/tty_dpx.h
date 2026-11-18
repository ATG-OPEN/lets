/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file tty_dpx.h
 * @brief When a full-duplex RS485 port (tty DPX1 / DPX2) swaps its direction by itself.
 *
 * With a straight full-duplex cable the direction pin picks the pair a port sends on, so the two
 * ends must be in opposite modes, and neither knows which one it should be. A DPX port that has
 * received nothing for a while swaps DPX1 <-> DPX2, like auto-MDIX: two ends that start alike
 * find each other within seconds, and a port that hears bytes never swaps.
 *
 * The wait before each swap is pseudo-random per port, TTY_DPX_MIN_MS .. + TTY_DPX_SPAN_MS: two
 * ends with one fixed wait that swapped together would swap together for ever and never be
 * opposite (two ports of one board, cabled, start at the same tick). The minimum is above the
 * 1 s a link protocol such as DRN pings at, so a live link never swaps.
 *
 * Seeds can still meet (16 bits), so each swap also stirs in the low bits of the caller's us
 * clock: two ports of one board are read some us apart, and two boards' clocks differ, so even
 * equal seeds drift apart after a swap or two.
 *
 * Pure (no I/O, no clock of its own): opt/tty.c calls it with the time in ms, and
 * tests/test_tty_dpx.c plays it on a cable model.
 */

#ifndef TTY_DPX_H
#define TTY_DPX_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TTY_DPX_MIN_MS
#define TTY_DPX_MIN_MS 2000u /**< The shortest silence before a swap. */
#endif
#ifndef TTY_DPX_SPAN_MS
#define TTY_DPX_SPAN_MS 2048u /**< Plus up to this much (a power of two). */
#endif

/** The state of one port. */
typedef struct {
	uint32_t heard; /**< ms of the last byte heard (or of the start, or of the last swap). */
	uint16_t wait;	/**< ms of silence before the next swap. */
	uint16_t state; /**< xorshift16, never 0. */
} tty_dpx_t;

/** @brief The next pseudo-random number of @p s (xorshift16). */
static inline uint16_t tty_dpx_next(tty_dpx_t *s)
{
	uint16_t x = s->state;

	x ^= (uint16_t)(x << 7);
	x ^= (uint16_t)(x >> 9);
	x ^= (uint16_t)(x << 8);
	s->state = x;

	return x;
}

/** @brief A new silence starts at @p now, with a new pseudo-random wait. */
static inline void tty_dpx_wait(tty_dpx_t *s, uint32_t now)
{
	s->heard = now;
	s->wait = (uint16_t)(TTY_DPX_MIN_MS + (tty_dpx_next(s) & (TTY_DPX_SPAN_MS - 1u)));
}

/**
 * @brief Starts @p s at @p now (ms).
 * @param s    The port's state.
 * @param now  The time, in ms.
 * @param seed Anything that differs between the ports of a board, and between boards if it
 *             can (opt/tty.c: the port, the address of its tty_args_t, the us clock).
 */
static inline void tty_dpx_init(tty_dpx_t *s, uint16_t seed, uint32_t now)
{
	s->state = (uint16_t)(seed * 40503u) ^ 0xACE1u;

	if (s->state == 0) {
		s->state = 0xACE1u;
	}

	tty_dpx_wait(s, now);
}

/** @brief A byte came at @p now: the silence starts over. */
static inline void tty_dpx_heard(tty_dpx_t *s, uint32_t now)
{
	s->heard = now;
}

/**
 * @brief true: nothing heard for the wait, swap the direction now (the next wait starts).
 * @param s     The port's state.
 * @param now   The time, in ms.
 * @param noise Stirred into the generator on a swap: the low bits of a us clock (0: none).
 */
static inline bool tty_dpx_due(tty_dpx_t *s, uint32_t now, uint16_t noise)
{
	if ((uint32_t)(now - s->heard) < s->wait) {
		return false;
	}

	if ((uint16_t)(s->state ^ noise) != 0) {
		s->state ^= noise;
	}

	tty_dpx_wait(s, now);

	return true;
}

#endif /* TTY_DPX_H */
