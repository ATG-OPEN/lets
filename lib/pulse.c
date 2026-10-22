/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* Return detection in sampled traces; see pulse.h. */

#include "pulse.h"

#include <string.h>
#include <math.h>

float pulse_median(float *v, int n)
{
	for (int i = 1; i < n; i++) {
		float t = v[i];
		int j = i - 1;
		while (j >= 0 && v[j] > t) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = t;
	}
	return n ? v[n / 2] : 0;
}

/* lower quartile of the blocks' detrended RMS (see pulse.h) */
float pulse_noise(const int32_t *v, uint16_t count, int32_t dc, float scale, uint16_t from)
{
	float blocks[PULSE_MAX_BLOCKS];
	int nb = 0;
	for (uint16_t s = from; s + PULSE_BLOCK <= count && nb < PULSE_MAX_BLOCKS;
	     s += PULSE_BLOCK) {
		/* least-squares line over k = 0..B-1, centred: t = k - (B-1)/2 */
		float sy = 0, sty = 0, stt = 0;
		for (int k = 0; k < PULSE_BLOCK; k++) {
			float t = (float)k - (PULSE_BLOCK - 1) * 0.5f;
			float x = (float)(v[s + k] - dc) * scale;
			sy += x;
			sty += t * x;
			stt += t * t;
		}
		float mean = sy / PULSE_BLOCK, slope = sty / stt;
		float sr = 0;
		for (int k = 0; k < PULSE_BLOCK; k++) {
			float t = (float)k - (PULSE_BLOCK - 1) * 0.5f;
			float r = (float)(v[s + k] - dc) * scale - mean - slope * t;
			sr += r * r;
		}
		/* RMS of the residual; the line fit used up 2 degrees of freedom */
		blocks[nb++] = sqrtf(sr / (PULSE_BLOCK - 2));
	}
	pulse_median(blocks, nb); /* sorts */
	float noise = nb ? blocks[nb / 4] : 0;
	return noise < PULSE_NOISE_MIN ? PULSE_NOISE_MIN : noise;
}

void pulse_analyze(const int32_t *v, uint16_t count, int32_t dc, float scale, uint16_t blank,
		   float snr_detect, pulse_analysis_t *a)
{
	memset(a, 0, sizeof(*a));

	/* noise after the blank zone too (near-field reflections are not noise), but keep at least
       half the trace for it */
	a->noise = pulse_noise(v, count, dc, scale, blank < count / 2 ? blank : count / 2);
	float level = snr_detect * a->noise;

	/* sample j above dc, scaled */
#define PULSE_AT(j) ((float)(v[(j)] - dc) * scale)
	for (uint16_t i = (blank > 1 ? blank : 1); i + 1 < count; i++) {
		/* a local maximum above the threshold (the first sample of a flat top) */
		float x = PULSE_AT(i);
		if (x < level || PULSE_AT(i - 1) > x || PULSE_AT(i + 1) >= x) {
			continue;
		}

		/* and the strongest within +-PULSE_SEPARATION */
		int better = 0;
		for (int k = -PULSE_SEPARATION; k <= PULSE_SEPARATION && !better; k++) {
			int j = (int)i + k;
			if (j >= 0 && j < count && k != 0 && PULSE_AT(j) > x)
				better = 1;
		}
		if (better)
			continue;

		/* constant fraction: 50 % on the rising edge, interpolated */
		float half = x * 0.5f;
		float edge = (float)i;
		for (int j = (int)i; j > 0; j--) {
			float lo = PULSE_AT(j - 1), hi = PULSE_AT(j);
			if (lo < half && hi >= half) {
				edge = (float)(j - 1) +
				       (half - lo) / ((hi - lo) > 1e-6f ? (hi - lo) : 1e-6f);
				break;
			}
		}

		/* a wide (low-passed) pulse just past the blank zone can have its edge inside */
		if (edge < (float)blank) {
			continue;
		}

		/* keep the strongest PULSE_MAX_RETURNS */
		int slot = a->count < PULSE_MAX_RETURNS ? a->count++ : PULSE_MAX_RETURNS - 1;
		if (slot == PULSE_MAX_RETURNS - 1 && a->count == PULSE_MAX_RETURNS &&
		    a->ret[slot].amplitude > x) {
			continue;
		}
		a->ret[slot].index = edge;
		a->ret[slot].amplitude = x;
		for (int k = slot; k > 0 && a->ret[k].amplitude > a->ret[k - 1].amplitude; k--) {
			pulse_return_t t = a->ret[k];
			a->ret[k] = a->ret[k - 1];
			a->ret[k - 1] = t;
		}
	}
#undef PULSE_AT

	a->peak = a->count ? a->ret[0].amplitude : 0;
	a->snr = a->peak / a->noise;
}

float pulse_max_i16(const int16_t *v, uint16_t count, int32_t dc, uint16_t from, float scale,
		    uint16_t *at)
{
	int32_t m = -65536;
	uint16_t where = from;
	for (uint16_t i = from; i < count; i++) {
		if (v[i] - dc > m) {
			m = v[i] - dc;
			where = i;
		}
	}
	if (at)
		*at = where;
	return (float)m * scale;
}
