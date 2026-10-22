/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "../apdcurrent.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>

/*
 * A model of the rangefinder this was written for, fitted to measurements:
 * - HV supply: PID (kp 5 %/V, ki 1 %/V/s, 0..100 %) over a converter of 2.8 V/% with a 0.1 s lag:
 *   it covers most of a jump in 0.5 s, then creeps (~5 s time constant) over the last volts.
 * - APD current (uA) vs bias: 0 below 176 V, 0.2 at 178, 1 at 181, 3 at 185, 6 at 189.5,
 *   and on, read through 21 x 2 kOhm onto a 12-bit 3.3 V ADC with an offset of 1852 counts.
 */
#define UA_PER_COUNT (3.3f / 4096 / 21 / 2000 * 1e6f)
#define OFFSET	     1852.0f
#define FRAME_US     50000u

typedef struct {
	float sp, v, integral;
} hv_t;

static void hv_run(hv_t *h, float seconds)
{
	for (float t = 0; t < seconds; t += 0.001f) {
		float e = h->sp - h->v;
		h->integral += e * 0.001f;
		float u = 5 * e + 1 * h->integral;
		if (u > 100) {
			u = 100;
			h->integral -= e * 0.001f;
		} else if (u < 0) {
			u = 0;
			h->integral -= e * 0.001f;
		}
		h->v += (2.8f * u - h->v) * 0.001f / 0.1f;
	}
}

static float apd_ua(float v, float shift)
{
	static const float pv[] = {176, 178, 181, 185, 189.5f, 194};
	static const float pi[] = {0.001f, 0.2f, 1, 3, 6, 11};
	v -= shift;
	if (v <= pv[0])
		return 0;
	for (int i = 1; i < 6; i++) {
		if (v <= pv[i]) {
			float f = (v - pv[i - 1]) / (pv[i] - pv[i - 1]);
			return expf(logf(pi[i - 1]) + f * (logf(pi[i]) - logf(pi[i - 1])));
		}
	}
	return 11 * expf((v - 194) / 5);
}

/* frame mean in counts: offset + current, with 10 % avalanche noise and 0.5 count of amplifier noise */
static float level(float v, float shift)
{
	float ua = apd_ua(v, shift);
	float n = ((float)((double)rand() / RAND_MAX) - 0.5f) * 2;
	float m = ((float)((double)rand() / RAND_MAX) - 0.5f) * 2;
	return OFFSET + ua * (1 + 0.1f * n) / UA_PER_COUNT + 0.5f * m;
}

/* runs until the calibration ends (or max_s); returns the seconds it took */
static float calibrate(apdcurrent_t *c, const apdcurrent_params_t *p, hv_t *h, float target,
		       float hint, float shift, uint64_t *now, float max_s)
{
	uint64_t t0 = *now;
	apdcurrent_start(c, p, target, hint, *now);
	h->sp = c->apd;
	while (apdcurrent_calibrating(c) && (float)(*now - t0) < max_s * 1e6f) {
		hv_run(h, FRAME_US / 1e6f);
		*now += FRAME_US;
		apdcurrent_update(c, p, level(h->v, shift), h->v, *now);
		h->sp = c->apd;
	}
	return (float)(*now - t0) / 1e6f;
}

/* holds for `seconds`, returns the mean current over the last half */
static float hold(apdcurrent_t *c, const apdcurrent_params_t *p, hv_t *h, float shift,
		  uint64_t *now, float seconds)
{
	double sum = 0;
	int n = 0, frames = (int)(seconds * 1e6f / FRAME_US);
	for (int i = 0; i < frames; i++) {
		hv_run(h, FRAME_US / 1e6f);
		*now += FRAME_US;
		apdcurrent_update(c, p, level(h->v, shift), h->v, *now);
		h->sp = c->apd;
		if (i >= frames / 2) {
			sum += apd_ua(h->v, shift);
			n++;
		}
	}
	return (float)(sum / n);
}

int main(void)
{
	apdcurrent_params_t p;
	apdcurrent_t c = {0};
	hv_t h = {0};
	uint64_t now = 1000000;

	srand(1);
	apdcurrent_defaults(&p);
	p.current_per_unit = UA_PER_COUNT;

	/* the model supply alone: most of a jump in 0.5 s, then a slow creep (as measured) */
	h.sp = 170;
	hv_run(&h, 0.5f);
	CHECK(h.v > 155 && h.v < 162);
	hv_run(&h, 5);
	CHECK(h.v > 164 && h.v < 168);
	h = (hv_t){0};

	/* trigger: 1 uA, from cold */
	float t1 = calibrate(&c, &p, &h, 1.0f, 0, 0, &now, 30);
	CHECK(c.have_offset);
	CHECK_NEAR(c.offset, OFFSET, 1.0);
	CHECK(c.reached);
	CHECK(t1 <= 10.0f);
	float i1 = hold(&c, &p, &h, 0, &now, 20);
	printf("apdcurrent: 1 uA in %.1f s at %.1f V, held %.2f uA\n", t1, c.apd, i1);
	CHECK(fabsf(i1 - 1.0f) < 0.3f);

	/* held without current measurements (an armed trigger): the supply settles on the result */
	{
		apdcurrent_t k = {0};
		hv_t hk = {0};
		calibrate(&k, &p, &hk, 1.0f, 0, 0, &now, 30);
		for (int i = 0; i < 300; i++) {
			hv_run(&hk, 0.1f);
			now += 100000;
			apdcurrent_hold(&k, &p, hk.v, now);
			hk.sp = k.apd;
		}
		printf("apdcurrent: held 30 s without measuring: %.1f V (result %.1f V)\n", hk.v,
		       k.result);
		CHECK(fabsf(hk.v - k.result) < 0.3f);
	}

	/* LRF: 10 uA, starting from the trigger bias (recent offset reused, no discharge) */
	float t10 = calibrate(&c, &p, &h, 10.0f, 0, 0, &now, 30);
	float r10 = c.result;
	CHECK(c.reached);
	CHECK(t10 <= 10.0f);
	float i10 = hold(&c, &p, &h, 0, &now, 20);
	printf("apdcurrent: 10 uA in %.1f s at %.1f V, held %.2f uA\n", t10, c.apd, i10);
	CHECK(fabsf(i10 - 10.0f) < 3.0f);

	/* again from cold, with the last result as the hint: faster */
	h = (hv_t){0};
	c.have_offset = 0;
	float t10b = calibrate(&c, &p, &h, 10.0f, r10, 0, &now, 30);
	printf("apdcurrent: 10 uA again in %.1f s\n", t10b);
	CHECK(c.reached && t10b < t10 + 0.5f);

	/* the diode warms up: its curve moves 3 V up, tracking follows */
	float before = h.v;
	float drift = hold(&c, &p, &h, 3.0f, &now, 60);
	printf("apdcurrent: after +3 V drift %.1f -> %.1f V, %.2f uA\n", before, h.v, drift);
	CHECK(h.v > before + 2.0f);
	CHECK(fabsf(drift - 10.0f) < 3.0f);

	/* unreachable target: the calibration still ends at the timeout, never above apd_max */
	h = (hv_t){0};
	float tx = calibrate(&c, &p, &h, 1000.0f, 0, 0, &now, 30);
	CHECK(!c.reached && tx <= 10.1f && c.apd <= p.apd_max);

	TEST_END("apdcurrent");
}
