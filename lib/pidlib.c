/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "pidlib.h"

#include <math.h>
#include <float.h>
#include <unistd.h>

void pidlib_initialize(pid_calc_t *p)
{
	if (p == NULL) {
		return;
	}

	p->sp = 0.0f;
	p->pv = 0.0f;

	p->pv_prev = 0.0f;

	p->integral_error = 0.0f;
	p->prev_error = 0.0f;

	p->kp = 0.0f;
	p->ki = 0.0f;
	p->kd = 0.0f;

	p->max = 1.0f;
	p->min = -1.0f;

	p->time_prev = 0;

	p->speed = 0.0f;
}

void pidlib_set_sp(pid_calc_t *p, float sp)
{
	p->sp = sp;
}

void pidlib_set_pv(pid_calc_t *p, float pv)
{
	p->pv = pv;
}

float pidlib_read_sp(pid_calc_t *p)
{
	return p->sp;
}

float pidlib_read_pv(pid_calc_t *p)
{
	return p->pv;
}

float pidlib_read_error(pid_calc_t *p)
{
	return fabs(p->sp - p->pv);
}

float pidlib_read_output(pid_calc_t *p)
{
	return p->output;
}

void pidlib_set_limits(pid_calc_t *p, float min, float max)
{
	p->min = min;
	p->max = max;
}

void pidlib_set_gains(pid_calc_t *p, float kp, float ki, float kd)
{
	p->kp = kp;
	p->ki = ki;
	p->kd = kd;
}

void pidlib_reset(pid_calc_t *p)
{
	p->integral_error = 0.0f;
	p->prev_error = 0.0f;
	p->pv_prev = p->pv;
	p->time_prev = 0;
	p->speed = 0.0f;
}

float pidlib_calculate(pid_calc_t *p, uint64_t system_time)
{
	if (p == NULL) {
		return -1.0f;
	}

	/* seconds since the previous call; the first call has none */
	float dt = 0.0f;

	if (p->time_prev != 0) {
		uint64_t dt_us = system_time - p->time_prev;
		dt = (float)dt_us / 1000000.0f;
	}

	p->time_prev = system_time;

	/* never divide by zero */
	if (dt <= 0.0f) {
		dt = 0.000001f;
	}

	float error = p->sp - p->pv;

	/* speed calculation */
	p->speed = (p->pv - p->pv_prev) / dt;
	p->pv_prev = p->pv;

	/* integrate */
	p->integral_error += error * dt;

	/* anti-windup: the integral stays within the output range */
	if (p->integral_error > p->max) {
		p->integral_error = p->max;
	} else if (p->integral_error < p->min) {
		p->integral_error = p->min;
	}

	/* derivative */
	float derivative = (error - p->prev_error) / dt;
	p->prev_error = error;

	/* pid output */
	p->output = (p->kp * error) + (p->ki * p->integral_error) + (p->kd * derivative);

	/* clamp output */
	if (p->output > p->max) {
		p->output = p->max;
	} else if (p->output < p->min) {
		p->output = p->min;
	}

	return p->output;
}
