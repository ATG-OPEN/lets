/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file spiral.h
 * @brief Archimedean spiral walked at constant speed: a 2-axis search pattern.
 *
 * To find a small target with a pan / tilt (or x / y) platform, walk outwards
 * on a spiral whose rings are `pitch` apart: nothing wider than the ring
 * spacing can be skipped. The spiral is parametrised by its path length s,
 * so the platform moves at a constant speed:
 *
 *     r = pitch theta / 2 pi,   s ~= pitch theta^2 / 4 pi   =>   r(s) = sqrt(pitch s / pi)
 *
 * spiral_point() gives the point and the unit tangent (the direction of
 * travel), for a position follower with speed feed-forward. spiral_back()
 * rewinds by time, for example to where the pattern was when a detection that
 * is reported late actually happened. Units are the caller's (degrees, mm, ...).
 *
 * @code
 * spiral_t sp;
 * spiral_start(&sp, pan, tilt, 2.0f, 0.03f, 0.25f);  // 2 deg out, 0.03 deg rings, 0.25 deg/s
 *
 * // every 10 ms:
 * if (!spiral_advance(&sp, 0.01f))
 *         stop();                                     // reached the edge, not found
 * float p[2], t[2];
 * spiral_point(&sp, sp.s, p, t);
 * move_to(p[0], p[1], t[0] * sp.speed, t[1] * sp.speed);
 * @endcode
 *
 * No allocation, no platform dependencies (C99).
 */

#ifndef SPIRAL_H
#define SPIRAL_H

#include <stdint.h>

/** A spiral and the position along it. */
typedef struct {
	float centre[2]; /**< Centre point. */
	float pitch;	 /**< Ring spacing. */
	float speed;	 /**< Speed along the path, units per second. */
	float rmax;	 /**< Largest radius: the pattern ends there. */
	float s;	 /**< Path length walked so far. */
} spiral_t;

/** @brief Starts a spiral at its centre (s = 0). */
void spiral_start(spiral_t *sp, float c0, float c1, float rmax, float pitch, float speed);

/** @brief Returns the radius at path length @p s. */
float spiral_radius(const spiral_t *sp, float s);

/**
 * @brief Point and direction at path length @p s.
 * @param sp The spiral.
 * @param s  Path length.
 * @param p Receives the point.
 * @param t Receives the unit tangent (direction of travel).
 */
void spiral_point(const spiral_t *sp, float s, float p[2], float t[2]);

/**
 * @brief Moves on by @p dt seconds.
 * @return 1, or 0 when that would pass rmax (s then stays where it was).
 */
uint8_t spiral_advance(spiral_t *sp, float dt);

/** @brief Returns the path length @p seconds ago (not below 0). */
float spiral_back(const spiral_t *sp, float seconds);

/** @brief Returns the time to walk out to @p rmax: path pi rmax^2 / pitch at @p speed. */
float spiral_duration(float rmax, float pitch, float speed);

#endif /* SPIRAL_H */
