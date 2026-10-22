/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "../pulse.h"
#include "test.h"
#include <math.h>

static double gauss(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void pulse_at(int32_t *v, int n, double centre, double amp, double width)
{
	for (int i = 0; i < n; i++)
		v[i] += (int32_t)lrint(amp * exp(-pow((i - centre) / width, 2)));
}

int main(void)
{
	int32_t v[1000];
	pulse_analysis_t a;
	srand(3);

	/* noise with an offset and a slope: the noise estimate ignores both */
	for (int i = 0; i < 1000; i++)
		v[i] = 2000 + i / 4 + (int32_t)lrint(10 * gauss());
	CHECK_NEAR(pulse_noise(v, 1000, 2000, 1.0f, 0), 10.0, 2.5);
	/* flat noise only: nothing found */
	for (int i = 0; i < 1000; i++)
		v[i] = 2000 + (int32_t)lrint(10 * gauss());
	pulse_analyze(v, 1000, 2000, 1.0f, 50, 8.0f, &a);
	CHECK(a.count == 0);

	/* two returns: strongest first, 50 % edge before the peak */
	for (int i = 0; i < 1000; i++)
		v[i] = 2000 + (int32_t)lrint(10 * gauss());
	pulse_at(v, 1000, 300, 400, 3);
	pulse_at(v, 1000, 600, 900, 3);
	pulse_analyze(v, 1000, 2000, 1.0f, 50, 8.0f, &a);
	CHECK(a.count == 2);
	CHECK_NEAR(a.ret[0].amplitude, 900, 40);
	CHECK(a.ret[0].index > 595 && a.ret[0].index < 600); /* edge at half height: centre - 2.5 */
	CHECK_NEAR(a.ret[1].amplitude, 400, 40);
	CHECK(a.snr > 50);

	/* a strong near return inside the blank is ignored, and does not lift the noise */
	pulse_at(v, 1000, 30, 5000, 3);
	pulse_analyze(v, 1000, 2000, 1.0f, 60, 8.0f, &a);
	CHECK(a.count == 2 && a.ret[0].index > 590);
	CHECK(a.noise < 15);

	/* scale: per-shot units */
	pulse_analyze(v, 1000, 2000, 0.5f, 60, 8.0f, &a);
	CHECK_NEAR(a.ret[0].amplitude, 450, 20);

	int16_t y[100] = {0};
	y[10] = 500;
	y[70] = 300;
	uint16_t at = 0;
	CHECK_NEAR(pulse_max_i16(y, 100, 0, 20, 2.0f, &at), 600, 1e-6);
	CHECK(at == 70);

	float m[5] = {5, 1, 4, 2, 3};
	CHECK(pulse_median(m, 5) == 3);
	TEST_END("pulse");
}
