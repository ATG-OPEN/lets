/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* APD bias controller; the phases are described in apdbias.h. */

#include "apdbias.h"

#include <math.h>

static float apdbias_clamp(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* median of v[0..n), sorts v in place */
static float apdbias_median(float *v, int n)
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

void apdbias_defaults(apdbias_params_t *p)
{
	p->apd_min = 50.0f;
	p->apd_max = 250.0f;
	p->search_from = 100.0f;
	p->search_step = 10.0f;
	p->fine_step = 2.0f;
	p->fine_from = 1e9f;
	p->search_lead = 25.0f;
	p->track_step = 0.5f;
	p->backoff = 4.0f;
	p->edge_ratio = 2.0f;
	p->too_noisy_ratio = 3.0f;
	p->settle = 1.0f;
	p->frames_per_step = 3;
	p->skip_jump = 3;
	p->skip_step = 1;
}

void apdbias_set_edge(apdbias_params_t *p, float ratio, float backoff)
{
	p->edge_ratio = apdbias_clamp(ratio, 1.05f, 10);
	p->backoff = apdbias_clamp(backoff, 0, 20);
	if (p->too_noisy_ratio < p->edge_ratio * 1.2f)
		p->too_noisy_ratio = p->edge_ratio * 1.5f;
}

void apdbias_start(apdbias_t *b, const apdbias_params_t *p)
{
	b->phase = APDBIAS_FLOOR;
	b->fine = 0;
	b->n_noise = 0;
	b->skip = 2;
	b->apd = p->apd_min;
}

void apdbias_forget(apdbias_t *b)
{
	b->have_floor = 0;
	b->have_edge = 0;
}

uint8_t apdbias_calibrating(const apdbias_t *b)
{
	return b->phase == APDBIAS_FLOOR || b->phase == APDBIAS_SEARCH;
}

uint8_t apdbias_update(apdbias_t *b, const apdbias_params_t *p, float frame_noise,
		       float apd_measured)
{
	if (b->phase == APDBIAS_MANUAL) {
		return 0;
	}

	/* only frames taken at the set point, after the skip, count */
	float apd0 = b->apd;
	uint8_t settled = fabsf(apd_measured - b->apd) <= p->settle;
	uint8_t frames = p->frames_per_step < 1			   ? 1
			 : p->frames_per_step > APDBIAS_MAX_FRAMES ? APDBIAS_MAX_FRAMES
								   : p->frames_per_step;

	if (settled && b->skip > 0) {
		b->skip--;
	} else if (settled) {
		b->noise[b->n_noise++] = frame_noise;

		if (b->n_noise >= frames) {
			float noise = apdbias_median(b->noise, b->n_noise);
			b->n_noise = 0;
			float edge_noise = (b->have_floor ? b->noise_floor : noise) * p->edge_ratio;

			if (b->phase == APDBIAS_FLOOR) {
				/* floor known: jump to the search start (below the last edge, if known) */
				b->noise_floor = noise;
				b->have_floor = 1;
				b->phase = APDBIAS_SEARCH;
				float start =
					(b->have_edge && b->edge - p->search_lead > p->search_from)
						? b->edge - p->search_lead
						: p->search_from;
				b->apd = start > p->apd_min ? start : p->apd_min;
				b->fine = start >= p->fine_from;
				b->skip = p->skip_jump;
			} else if (b->phase == APDBIAS_SEARCH) {
				/* at the edge: after a fine step it is found, after a coarse step go back
                   one step and repeat finely */
				if (noise >= edge_noise || b->apd >= p->apd_max) {
					if (b->fine || p->fine_step >= p->search_step) {
						b->edge = b->apd;
						b->have_edge = 1;
						b->apd -= p->backoff;
						b->phase = APDBIAS_TRACK;
						b->fine = 0;
					} else {
						b->apd -= p->search_step;
						b->fine = 1;
					}
				} else {
					b->apd += b->fine ? p->fine_step : p->search_step;
				}
				b->skip = p->skip_step;
			} else if (b->phase == APDBIAS_TRACK) {
				float quiet = b->have_floor ? b->noise_floor : noise;
				/* held below the edge: the noise there says nothing about the edge, so never above
                   it; held at the edge (no backoff): the edge follows */
				float ceiling = (b->have_edge && p->backoff > 0)
							? b->edge - p->backoff
							: p->apd_max;
				if (noise >= quiet * p->too_noisy_ratio) {
					b->apd -= 2.0f * p->track_step;
					b->edge = b->apd + p->backoff;
					b->have_edge = 1;
				} else if (noise < edge_noise &&
					   b->apd + p->track_step <= ceiling + 0.01f) {
					b->apd += p->track_step;
					if (p->backoff <= 0 && b->apd > b->edge)
						b->edge = b->apd;
				}
			}
		}
	}

	/* within range, on a 0.1 V grid */
	b->apd = apdbias_clamp(b->apd, p->apd_min, p->apd_max);
	b->apd = roundf(b->apd * 10) / 10;
	return b->apd != apd0;
}
