/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "../spiral.h"
#include "test.h"
#include <math.h>

int main(void)
{
	spiral_t sp;
	spiral_start(&sp, 100, 20, 0.25f, 0.02f, 0.5f);
	CHECK_NEAR(spiral_duration(0.25f, 0.02f, 0.5f), 19.63, 0.02);

	/* walk it: rings 0.02 apart, constant speed, ends at rmax after the duration */
	float p[2], t[2], last[2] = {100, 20};
	double time = 0, path = 0;
	while (spiral_advance(&sp, 0.01f)) {
		time += 0.01;
		spiral_point(&sp, sp.s, p, t);
		path += hypot(p[0] - last[0], p[1] - last[1]);
		last[0] = p[0];
		last[1] = p[1];
		CHECK_NEAR(hypot(t[0], t[1]), 1.0, 1e-4);
	}
	CHECK_NEAR(time, 19.63, 0.05);
	CHECK_NEAR(hypot(last[0] - 100, last[1] - 20), 0.25, 0.005);
	CHECK_NEAR(path / time, 0.5, 0.03); /* speed along the path */

	/* ring spacing: radius grows by `pitch` per turn */
	CHECK_NEAR(spiral_radius(&sp, 3.14159265f * 0.1f * 0.1f / 0.02f), 0.1, 1e-4);

	/* back by time */
	CHECK_NEAR(spiral_back(&sp, 1.0f), sp.s - 0.5, 1e-4);
	CHECK(spiral_back(&sp, 1e6f) == 0);
	spiral_point(&sp, 0, p, t);
	CHECK(p[0] == 100 && p[1] == 20);
	TEST_END("spiral");
}
