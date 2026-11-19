/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The kernel's clocks, as on Linux: CLOCK_MONOTONIC is the clock the board gave
 * task_initialize() (us since start-up); CLOCK_REALTIME is UTC, set with settimeofday() or
 * clock_settime(), slewed with adjtime() / adjtimex(ADJ_OFFSET_SINGLESHOT) and with its rate
 * corrected by adjtimex(ADJ_FREQUENCY). `date`, `uptime` and the node `date` (bin/date.c) are
 * programs on these.
 *
 * The time is counted from an anchor: what the kernel's clock read then (up0) and the time it
 * was (utc0), both in us. Since then it runs at the kernel's rate corrected by the rate (ppb),
 * and a slew still to be applied goes in at most CLOCK_SLEW_PPM.
 */

#include "task.h"
#include "../lib/posix.h"

#define CLOCK_SLEW_PPM	500		/* a slew moves the clock at most this fast */
#define CLOCK_ANCHOR_US 0x40000000ull	/* re-anchored after this long (~18 min): small products */

static uint64_t clock_up0;
static uint64_t clock_utc0;
static int32_t clock_rate; /* ppb */
static int64_t clock_slew; /* us still to go in */
static bool clock_valid;   /* set since the reset */

/* the time at kernel time @p up, and in @p slewed how much of the slew is in it */
static uint64_t clock_at(uint64_t up, int64_t *slewed)
{
	uint64_t dt = up - clock_up0;
	int64_t corr = (int64_t)dt * clock_rate / 1000000000ll;
	int64_t most = (int64_t)(dt / (1000000ull / CLOCK_SLEW_PPM));
	int64_t s = clock_slew;

	if (s > most) {
		s = most;
	} else if (s < -most) {
		s = -most;
	}

	*slewed = s;

	return clock_utc0 + dt + (uint64_t)(corr + s);
}

/* moves the anchor to now; the time now */
static uint64_t clock_anchor(void)
{
	uint64_t up = task_get_system_time();
	int64_t s;
	uint64_t now = clock_at(up, &s);

	clock_up0 = up;
	clock_utc0 = now;
	clock_slew -= s;

	return now;
}

/* UTC in us */
static uint64_t clock_realtime(void)
{
	int64_t s;
	uint64_t up = task_get_system_time();

	return (up - clock_up0 >= CLOCK_ANCHOR_US) ? clock_anchor() : clock_at(up, &s);
}

static void clock_set(uint64_t us)
{
	clock_up0 = task_get_system_time();
	clock_utc0 = us;
	clock_slew = 0;
	clock_valid = true;
}

int clock_gettime(clockid_t clock, struct timespec *t)
{
	uint64_t us = (clock == CLOCK_REALTIME) ? clock_realtime() : task_get_system_time();

	t->tv_sec = (time_t)(us / 1000000u);
	t->tv_nsec = (long)(uint32_t)(us - (uint64_t)t->tv_sec * 1000000u) * 1000;

	return 0;
}

int clock_settime(clockid_t clock, const struct timespec *t)
{
	if (clock != CLOCK_REALTIME) {
		return -1;
	}

	clock_set((uint64_t)t->tv_sec * 1000000u + (uint32_t)t->tv_nsec / 1000u);

	return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	uint64_t us = clock_realtime();

	(void)tz;
	tv->tv_sec = (time_t)(us / 1000000u);
	tv->tv_usec = (suseconds_t)(us - (uint64_t)tv->tv_sec * 1000000u);

	return 0;
}

int settimeofday(const struct timeval *tv, const void *tz)
{
	(void)tz;
	clock_set((uint64_t)tv->tv_sec * 1000000u + (uint64_t)tv->tv_usec);

	return 0;
}

/* the slew still to go in, in us */
static int64_t clock_pending(void)
{
	int64_t s;

	clock_at(task_get_system_time(), &s);

	return clock_slew - s;
}

int adjtime(const struct timeval *delta, struct timeval *olddelta)
{
	struct timex t = {0};

	if (delta != NULL) {
		/* time_t may be unsigned (XC16): a negative second count is taken back as such */
		t.modes = ADJ_OFFSET_SINGLESHOT;
		t.offset = (long)((int64_t)(long)delta->tv_sec * 1000000 + delta->tv_usec);
	}

	adjtimex(&t);

	if (olddelta != NULL) {
		olddelta->tv_sec = (time_t)(t.offset / 1000000);
		olddelta->tv_usec = (suseconds_t)(t.offset % 1000000);
	}

	return 0;
}

int adjtimex(struct timex *t)
{
	if ((t->modes & ADJ_OFFSET_SS_READ) == ADJ_OFFSET_SINGLESHOT) {
		clock_anchor();
		clock_slew += t->offset;
	}

	if (t->modes & ADJ_FREQUENCY) {
		/* scaled ppm (ppm << 16) to ppb, rounded */
		int64_t f = (int64_t)t->freq * 1000;

		clock_anchor();
		clock_rate = (int32_t)((f + ((f < 0) ? -32768 : 32768)) / 65536);
	}

	int64_t pending = clock_pending();
	int64_t f = (int64_t)clock_rate * 65536;

	t->offset = (long)pending;
	t->freq = (long)((f + ((f < 0) ? -500 : 500)) / 1000);
	t->status = clock_valid ? 0 : STA_UNSYNC;

	return TIME_OK;
}
