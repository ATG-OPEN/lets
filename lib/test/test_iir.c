/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "../iir.h"
#include "test.h"
#include <math.h>

/* gain at frequency f of a running float section (steady-state amplitude of a sine) */
static double gain_at(const iir_biquad_t *f, double hz, double fs)
{
	float v[4000];
	for (int i = 0; i < 4000; i++)
		v[i] = (float)sin(2 * M_PI * hz * i / fs);
	iir_run_f(f, v, 4000, 0);
	double m = 0;
	for (int i = 3000; i < 4000; i++)
		if (fabs(v[i]) > m)
			m = fabs(v[i]);
	return m;
}

int main(void)
{
	iir_biquad_t f;
	const float fs = 50e6f;

	CHECK(iir_design(&f, 0, 0, fs) == IIR_OFF);
	CHECK(iir_design(&f, 0, 30e6f, fs) == IIR_OFF); /* above 0.45 fs: not set */
	CHECK_NEAR(iir_dc_gain(&f), 1.0, 1e-6);

	CHECK(iir_design(&f, 0, 5e6f, fs) == IIR_LOWPASS);
	CHECK_NEAR(iir_dc_gain(&f), 1.0, 1e-4);
	CHECK_NEAR(gain_at(&f, 5e6, fs), 0.7071, 0.02); /* -3 dB at the cut-off */
	CHECK(gain_at(&f, 20e6, fs) < 0.1);

	CHECK(iir_design(&f, 1e6f, 0, fs) == IIR_HIGHPASS);
	CHECK_NEAR(iir_dc_gain(&f), 0.0, 1e-4);
	CHECK_NEAR(gain_at(&f, 1e6, fs), 0.7071, 0.02);

	CHECK(iir_design(&f, 1e6f, 12e6f, fs) == IIR_BANDPASS);
	CHECK_NEAR(iir_dc_gain(&f), 0.0, 1e-4);
	CHECK_NEAR(gain_at(&f, sqrt(12e12), fs), 1.0, 0.02);	/* 0 dB at the centre */
	CHECK(iir_design(&f, 12e6f, 1e6f, fs) == IIR_HIGHPASS); /* high below low: high-pass */

	/* steady-state start: a constant input stays constant (x dc gain) from the first sample */
	iir_design(&f, 0, 5e6f, fs);
	int32_t c[50];
	for (int i = 0; i < 50; i++)
		c[i] = 1854;
	iir_run_i32(&f, c, 50, 1854);
	for (int i = 0; i < 50; i++)
		CHECK(c[i] == 1854);

	/* Q14 section follows the float one, starts in its steady state, rounds without offset */
	iir_q14_t q;
	iir_design(&f, 1e6f, 12e6f, fs);
	CHECK(iir_to_q14(&f, &q) == 1);
	iir_q14_state_t s;
	iir_q14_reset(&q, &s, 1000);
	CHECK(iir_q14_dc(&q, 1000) == 0);
	float ref[2000];
	double sum = 0, err = 0;
	srand(1);
	for (int i = 0; i < 2000; i++)
		ref[i] = 1000.0f + (float)(rand() % 200 - 100);
	float fr[2000];
	for (int i = 0; i < 2000; i++)
		fr[i] = ref[i];
	iir_run_f(&f, fr, 2000, 1000);
	for (int i = 0; i < 2000; i++) {
		int32_t y = iir_q14_step(&q, &s, (int32_t)ref[i]);
		sum += y;
		if (fabs(y - fr[i]) > err)
			err = fabs(y - fr[i]);
	}
	CHECK(err < 3.0);
	CHECK_NEAR(sum / 2000, 0.0, 0.5); /* no rounding offset */

	/* clamping */
	CHECK(iir_q14_out(16384, 0, 0, 0, 0, 40000, 0, 0, 0, 0) == 32767);
	CHECK(iir_q14_out(16384, 0, 0, 0, 0, -40000, 0, 0, 0, 0) == -32768);
	TEST_END("iir");
}
