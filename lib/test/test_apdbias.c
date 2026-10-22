/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "../apdbias.h"
#include "test.h"
#include <math.h>

/* a model APD: noise 0.3 below ~edge, rising steeply above it; the HV ramps at 4 V per frame */
static float model_noise(float v, float edge)
{
	return 0.3f * (1.0f + expf((v - edge) / 1.5f));
}

static float run(apdbias_t *b, const apdbias_params_t *p, float edge, int frames, float *hv,
		 int *cal_frames)
{
	for (int i = 0; i < frames; i++) {
		float d = b->apd - *hv;
		*hv += d > 4 ? 4 : d < -4 ? -4 : d;
		/* still moving: noisy */
		float n = fabsf(d) > 0.5f ? 3.0f : model_noise(*hv, edge);
		apdbias_update(b, p, n, *hv);
		if (cal_frames && apdbias_calibrating(b))
			(*cal_frames)++;
	}
	return b->apd;
}

int main(void)
{
	apdbias_params_t p;
	apdbias_t b = {0};
	float hv = 0;
	int cal = 0;

	/* conservative defaults: edge at 2 x floor, 4 V below */
	apdbias_defaults(&p);
	apdbias_start(&b, &p);
	CHECK(b.phase == APDBIAS_FLOOR && b.apd == 50);
	run(&b, &p, 178, 400, &hv, &cal);
	int cal_default = cal;
	CHECK(b.phase == APDBIAS_TRACK);
	CHECK(b.edge > 175 && b.edge < 180);
	CHECK_NEAR(b.apd, b.edge - 4, 1.0);

	/* fast start near the edge, held at it (no backoff) */
	apdbias_defaults(&p);
	p.search_from = 170;
	p.fine_from = 150;
	p.backoff = 0;
	apdbias_t c = {0};
	hv = 0;
	cal = 0;
	apdbias_start(&c, &p);
	run(&c, &p, 178, 400, &hv, &cal);
	CHECK(c.phase == APDBIAS_TRACK);
	CHECK(c.apd > 175 && c.apd < 180);
	printf("apdbias: edge %.1f V, held %.1f V, %d calibration frames (from 100 V: %d)\n",
	       c.edge, c.apd, cal, cal_default);
	CHECK(cal < cal_default);

	/* the APD warms up: the edge moves up and the hold follows (no backoff) */
	run(&c, &p, 183, 600, &hv, NULL);
	CHECK(c.apd > 180);

	/* a restart with a learned edge skips the floor search length */
	apdbias_start(&c, &p);
	cal = 0;
	run(&c, &p, 183, 400, &hv, &cal);
	CHECK(c.phase == APDBIAS_TRACK && c.apd > 180);

	/* set_edge keeps too_noisy above the edge ratio */
	apdbias_set_edge(&p, 3.0f, 0);
	CHECK(p.too_noisy_ratio >= 3.6f);
	apdbias_forget(&c);
	CHECK(!c.have_floor && !c.have_edge);

	/* manual: nothing moves */
	c.phase = APDBIAS_MANUAL;
	float before = c.apd;
	CHECK(apdbias_update(&c, &p, 99, c.apd) == 0 && c.apd == before);
	TEST_END("apdbias");
}
