/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* biquad design (RBJ cookbook) and float runs; see iir.h. */

#include "iir.h"

#include <string.h>
#include <math.h>

uint8_t iir_design(iir_biquad_t *f, float low_hz, float high_hz, float sample_hz)
{
	memset(f, 0, sizeof(*f));
	uint8_t lo = low_hz > 0 && low_hz < sample_hz * 0.45f;
	uint8_t hi = high_hz > 0 && high_hz < sample_hz * 0.45f;
	if (lo && hi && high_hz <= low_hz)
		hi = 0;
	if (!lo && !hi) {
		return IIR_OFF;
	}

	/* centre or corner frequency, quality factor and the cookbook's helpers */
	float f0 = (lo && hi) ? sqrtf(low_hz * high_hz) : lo ? low_hz : high_hz;
	float q = (lo && hi) ? f0 / (high_hz - low_hz) : 0.70710678f;
	float w0 = 2.0f * 3.14159265f * f0 / sample_hz;
	float cw = cosf(w0);
	float alpha = sinf(w0) / (2.0f * q);
	float a0 = 1.0f + alpha;

	if (lo && hi) {
		f->b0 = alpha;
		f->b1 = 0;
		f->b2 = -alpha;
		f->kind = IIR_BANDPASS;
	} else if (lo) {
		f->b0 = (1.0f + cw) / 2.0f;
		f->b1 = -(1.0f + cw);
		f->b2 = f->b0;
		f->kind = IIR_HIGHPASS;
	} else {
		f->b0 = (1.0f - cw) / 2.0f;
		f->b1 = 1.0f - cw;
		f->b2 = f->b0;
		f->kind = IIR_LOWPASS;
	}
	/* normalise to a0 = 1 */
	f->b0 /= a0;
	f->b1 /= a0;
	f->b2 /= a0;
	f->a1 = -2.0f * cw / a0;
	f->a2 = (1.0f - alpha) / a0;
	return f->kind;
}

float iir_dc_gain(const iir_biquad_t *f)
{
	if (f->kind == IIR_OFF)
		return 1.0f;
	float den = 1.0f + f->a1 + f->a2;
	return (den > 1e-9f || den < -1e-9f) ? (f->b0 + f->b1 + f->b2) / den : 0.0f;
}

/* the history starts as if x0 had always been the input: no start-up step */
void iir_run_f(const iir_biquad_t *f, float *v, uint16_t n, float x0)
{
	if (f->kind == IIR_OFF)
		return;
	float x1 = x0, x2 = x0;
	float y1 = x0 * iir_dc_gain(f), y2 = y1;
	for (uint16_t i = 0; i < n; i++) {
		float x = v[i];
		float y = f->b0 * x + f->b1 * x1 + f->b2 * x2 - f->a1 * y1 - f->a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		v[i] = y;
	}
}

void iir_run_i32(const iir_biquad_t *f, int32_t *v, uint16_t n, float x0)
{
	if (f->kind == IIR_OFF)
		return;
	float x1 = x0, x2 = x0;
	float y1 = x0 * iir_dc_gain(f), y2 = y1;
	for (uint16_t i = 0; i < n; i++) {
		float x = (float)v[i];
		float y = f->b0 * x + f->b1 * x1 + f->b2 * x2 - f->a1 * y1 - f->a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		v[i] = (int32_t)lrintf(y);
	}
}

/* one coefficient in Q14, rounded and clamped to int16_t */
static int16_t iir_q14_coef(float v)
{
	float q = v * IIR_Q14;
	return (int16_t)(q > 32767.0f ? 32767 : q < -32768.0f ? -32768 : lrintf(q));
}

uint8_t iir_to_q14(const iir_biquad_t *f, iir_q14_t *q)
{
	memset(q, 0, sizeof(*q));
	if (f->kind == IIR_OFF)
		return 0;
	q->b[0] = iir_q14_coef(f->b0);
	q->b[1] = iir_q14_coef(f->b1);
	q->b[2] = iir_q14_coef(f->b2);
	q->a[0] = iir_q14_coef(f->a1);
	q->a[1] = iir_q14_coef(f->a2);
	return 1;
}
