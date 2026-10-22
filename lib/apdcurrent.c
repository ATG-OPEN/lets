/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* APD bias controller on the APD current; the phases are described in apdcurrent.h. */

#include "apdcurrent.h"

#include <math.h>

#define US_PER_S 1000000.0f

/* the outer loop integrates only near the wanted voltage (a big jump is the supply's own job) */
#define SERVO_WINDOW 15.0f
/* the search ramps only while the supply keeps up within this */
#define RAMP_WINDOW 2.0f
/* longest time step the search ramp takes at once */
#define DT_MAX 0.25f
/* most of the bias error the outer loop takes in one update */
#define SERVO_STEP_MAX 0.5f
/* the supply is creeping (not jumping) below this rate, V/s */
#define CREEP_RATE 3.0f

static float apdcurrent_clamp(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void apdcurrent_defaults(apdcurrent_params_t *p)
{
	p->current_per_unit = 1.0f;
	p->apd_min = 0.0f;
	p->apd_max = 200.0f;
	p->offset_below = 100.0f;
	p->offset_max_age = 300.0f;
	p->search_from = 170.0f;
	p->search_lead = 3.0f;
	p->ramp_rate = 6.0f;
	p->tolerance = 0.25f;
	p->trim_gain = 1.5f;
	p->trim_step = 1.0f;
	p->track_step = 0.2f;
	p->settle = 0.2f;
	p->servo_gain = 2.0f;
	p->servo_max = 15.0f;
	p->smooth = 0.3f;
	p->timeout = 10.0f;
	p->offset_frames = 4;
	p->step_frames = 4;
	p->on_target = 2;
}

void apdcurrent_start(apdcurrent_t *c, const apdcurrent_params_t *p, float target, float hint,
		      uint64_t now)
{
	c->hint = hint;
	c->phase = APDCURRENT_OFFSET;
	c->target = target;
	c->count = 0;
	c->offset_sum = 0;
	c->have_current = 0;
	c->window_n = 0;
	c->reached = 0;
	c->want = p->apd_min;
	c->servo = 0;
	c->apd = p->apd_min;
	c->start_time = now;
	c->last_time = now;
}

void apdcurrent_stop(apdcurrent_t *c)
{
	c->phase = APDCURRENT_IDLE;
}

void apdcurrent_forget(apdcurrent_t *c)
{
	c->have_offset = 0;
	c->hint = 0;
}

uint8_t apdcurrent_calibrating(const apdcurrent_t *c)
{
	return c->phase == APDCURRENT_OFFSET || c->phase == APDCURRENT_SEARCH ||
	       c->phase == APDCURRENT_TRIM;
}

float apdcurrent_of(const apdcurrent_t *c, const apdcurrent_params_t *p, float level)
{
	return c->have_offset ? (level - c->offset) * p->current_per_unit : 0.0f;
}

/* offset known: jump to the search start, just below the hint when there is one */
static void apdcurrent_begin_search(apdcurrent_t *c, const apdcurrent_params_t *p)
{
	float from = p->search_from;

	if (c->hint > 0 && c->hint - p->search_lead > p->apd_min)
		from = c->hint - p->search_lead;
	c->phase = APDCURRENT_SEARCH;
	c->want = from;
	c->servo = 0;
	c->have_current = 0;
	c->window_n = 0;
}

/* calibration over: hold here and track from now on */
static void apdcurrent_finish(apdcurrent_t *c, uint8_t reached)
{
	c->phase = APDCURRENT_TRACK;
	c->reached = reached;
	c->result = c->want;
	c->count = 0;
}

/*
 * TRIM and TRACK: average a few frames while the bias holds still, then step the wanted voltage
 * by what the current says (the outer loop takes care of the supply sitting a little off it)
 */
static void apdcurrent_trim(apdcurrent_t *c, const apdcurrent_params_t *p, float current,
			    float apd_measured)
{
	if (c->window_n == 0) {
		c->window_lo = apd_measured;
		c->window_hi = apd_measured;
		c->window_sum = 0;
	}
	if (apd_measured < c->window_lo)
		c->window_lo = apd_measured;
	if (apd_measured > c->window_hi)
		c->window_hi = apd_measured;
	c->window_sum += current;
	if (++c->window_n < (p->step_frames ? p->step_frames : 1))
		return;

	uint8_t moving = c->window_hi - c->window_lo > p->settle;
	float mean = c->window_sum / (float)c->window_n;
	float ratio = c->target > 0 ? mean / c->target : 1.0f;

	c->window_n = 0;
	if (moving)
		return;

	if (fabsf(ratio - 1.0f) <= p->tolerance) {
		if (c->phase == APDCURRENT_TRIM && ++c->count >= p->on_target)
			apdcurrent_finish(c, 1);
		return;
	}

	/* the current grows roughly exponentially with the bias near the avalanche */
	float limit = c->phase == APDCURRENT_TRIM ? p->trim_step : p->track_step;
	float step = -p->trim_gain * logf(ratio > 0.05f ? ratio : 0.05f);

	c->count = 0;
	c->want += apdcurrent_clamp(step, -limit, limit);
}

/*
 * Outer loop: integrate what the supply still lacks, but only while it creeps (a jump in progress
 * is the supply's own job, and integrating it would overshoot). Updates seconds apart take at
 * most half the error each, so it still unwinds.
 */
static void apdcurrent_servo(apdcurrent_t *c, const apdcurrent_params_t *p, float apd_measured,
			     float elapsed)
{
	float error_v = c->want - apd_measured;
	float slew = elapsed > 0 ? (apd_measured - c->last_v) / elapsed : 0;

	if (p->servo_gain > 0 && fabsf(error_v) < SERVO_WINDOW && fabsf(slew) < CREEP_RATE) {
		float k = p->servo_gain * elapsed;

		c->servo += (k < SERVO_STEP_MAX ? k : SERVO_STEP_MAX) * error_v;
		c->servo = apdcurrent_clamp(c->servo, -p->servo_max, p->servo_max);
	}
}

/* the set point from the wanted voltage and the outer loop, on a 0.1 V grid like the supply's */
static uint8_t apdcurrent_apply(apdcurrent_t *c, const apdcurrent_params_t *p, float apd0)
{
	c->want = apdcurrent_clamp(c->want, p->apd_min, p->apd_max);
	c->apd = apdcurrent_clamp(c->want + c->servo, p->apd_min, p->apd_max);
	c->apd = roundf(c->apd * 10) / 10;
	return c->apd != apd0;
}

uint8_t apdcurrent_hold(apdcurrent_t *c, const apdcurrent_params_t *p, float apd_measured,
			uint64_t now)
{
	if (c->phase != APDCURRENT_TRACK)
		return 0;

	float apd0 = c->apd;
	float elapsed = (float)(now - c->last_time) / US_PER_S;

	c->last_time = now;
	apdcurrent_servo(c, p, apd_measured, elapsed);
	c->last_v = apd_measured;
	return apdcurrent_apply(c, p, apd0);
}

uint8_t apdcurrent_update(apdcurrent_t *c, const apdcurrent_params_t *p, float level,
			  float apd_measured, uint64_t now)
{
	if (c->phase == APDCURRENT_IDLE)
		return 0;

	float apd0 = c->apd;
	float elapsed = (float)(now - c->last_time) / US_PER_S;
	float dt = apdcurrent_clamp(elapsed, 0.0f, DT_MAX);

	c->last_time = now;

	if (c->phase == APDCURRENT_OFFSET) {
		c->want = p->apd_min;
		c->servo = 0;
		if (apd_measured > p->offset_below) {
			/* still discharging: a recent offset saves the wait */
			if (c->have_offset &&
			    (float)(now - c->offset_time) <= p->offset_max_age * US_PER_S)
				apdcurrent_begin_search(c, p);
		} else {
			c->offset_sum += level;
			if (++c->count >= (p->offset_frames ? p->offset_frames : 1)) {
				c->offset = c->offset_sum / (float)c->count;
				c->have_offset = 1;
				c->offset_time = now;
				c->count = 0;
				apdcurrent_begin_search(c, p);
			}
		}
	} else {
		float current = apdcurrent_of(c, p, level);
		float error_v = c->want - apd_measured;

		if (!c->have_current) {
			c->current = current;
			c->have_current = 1;
		} else {
			c->current += p->smooth * (current - c->current);
		}

		float ratio = c->target > 0 ? c->current / c->target : 1.0f;

		if (c->phase == APDCURRENT_SEARCH) {
			if (ratio >= 0.5f) {
				c->phase = APDCURRENT_TRIM;
				c->count = 0;
			} else if (fabsf(error_v) < RAMP_WINDOW) {
				/* fast through the voltages with no current, slower closer in */
				float rate = ratio < 0.05f   ? 2 * p->ramp_rate
					     : ratio < 0.25f ? p->ramp_rate
							     : p->ramp_rate / 2;
				c->want += rate * dt;
			}
		} else if (ratio > 3.0f) {
			/* far too much current (a jump overshot, or the light came on): back off now */
			c->want -= 2.0f * p->trim_step;
			c->window_n = 0;
			c->count = 0;
		} else {
			apdcurrent_trim(c, p, current, apd_measured);
		}

		if (apdcurrent_calibrating(c) &&
		    (float)(now - c->start_time) >= p->timeout * US_PER_S)
			apdcurrent_finish(c, 0);

		apdcurrent_servo(c, p, apd_measured, elapsed);
	}

	c->last_v = apd_measured;
	return apdcurrent_apply(c, p, apd0);
}
