/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* Constant-speed Archimedean spiral; see spiral.h. */

#include "spiral.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void spiral_start(spiral_t *sp, float c0, float c1, float rmax, float pitch, float speed)
{
	sp->centre[0] = c0;
	sp->centre[1] = c1;
	sp->rmax = rmax;
	sp->pitch = pitch;
	sp->speed = speed;
	sp->s = 0;
}

float spiral_radius(const spiral_t *sp, float s)
{
	return sqrtf(sp->pitch * (s > 0 ? s : 0) / (float)M_PI);
}

void spiral_point(const spiral_t *sp, float s, float p[2], float t[2])
{
	float r = spiral_radius(sp, s);
	/* r = pitch theta / 2 pi */
	float theta = (r > 0) ? 2.0f * (float)M_PI * r / sp->pitch : 0;
	float c = cosf(theta), sn = sinf(theta);

	p[0] = sp->centre[0] + r * c;
	p[1] = sp->centre[1] + r * sn;

	/* dp/ds = dr/ds (c, s) + (-s, c), dr/ds = pitch / (2 pi r); limited near the centre */
	float rr = (r > sp->pitch * 0.5f) ? r : sp->pitch * 0.5f;
	float k = sp->pitch / (2.0f * (float)M_PI * rr);
	t[0] = k * c - sn;
	t[1] = k * sn + c;
	float n = sqrtf(t[0] * t[0] + t[1] * t[1]);
	t[0] /= n;
	t[1] /= n;
}

uint8_t spiral_advance(spiral_t *sp, float dt)
{
	float s = sp->s + sp->speed * dt;
	if (spiral_radius(sp, s) > sp->rmax) {
		return 0;
	}
	sp->s = s;
	return 1;
}

float spiral_back(const spiral_t *sp, float seconds)
{
	float s = sp->s - sp->speed * seconds;
	return s > 0 ? s : 0;
}

/* path length to rmax: s = pi r^2 / pitch (from r(s) above) */
float spiral_duration(float rmax, float pitch, float speed)
{
	return (float)M_PI * rmax * rmax / (pitch > 1e-6f ? pitch : 1e-6f) /
	       (speed > 1e-6f ? speed : 1e-6f);
}
