/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file pidlib.h
 * @brief PID controller (float), with output limits and anti-windup.
 *
 * Each call of pidlib_calculate() measures the time since the previous call
 * and computes
 *
 *     output = kp * error + ki * integral(error dt) + kd * d(error)/dt
 *
 * where error = set point - process variable. The integral and the output are
 * both kept within [min, max], so the integral cannot wind up while the output
 * is saturated.
 *
 * @code
 * pid_calc_t pid;
 * pidlib_initialize(&pid);
 * pidlib_set_gains(&pid, 2.0f, 0.5f, 0.0f);
 * pidlib_set_limits(&pid, 0.0f, 100.0f);         // heater power, %
 * pidlib_set_sp(&pid, 60.0f);                    // target temperature
 *
 * // in a task, every 100 ms:
 * pidlib_set_pv(&pid, read_temperature());
 * heater_set(pidlib_calculate(&pid, now));
 * @endcode
 */

#ifndef PIDLIB_H
#define PIDLIB_H

#include <stdint.h>

/** Controller state. Use the functions below rather than the fields. */
typedef struct {
	float sp; /**< Set point. */
	float pv; /**< Process variable (the measurement). */

	float pv_prev; /**< pv at the previous calculation. */

	float integral_error; /**< Integral of the error over time (error x s). */
	float prev_error;     /**< Error at the previous calculation. */

	float kp; /**< Proportional gain. */
	float ki; /**< Integral gain, per second. */
	float kd; /**< Derivative gain, x seconds. */

	float max; /**< Upper limit of the output (and the integral). */
	float min; /**< Lower limit of the output (and the integral). */

	uint64_t time_prev; /**< Time of the previous calculation, in us. */

	float speed; /**< Rate of change of pv, units per second. */

	float output; /**< The last output. */
} pid_calc_t;

/** @brief Clears the controller: gains 0, limits -1..1. */
void pidlib_initialize(pid_calc_t *p);

/** @brief Sets the target value. */
void pidlib_set_sp(pid_calc_t *p, float sp);

/** @brief Sets the measured value; call before each pidlib_calculate(). */
void pidlib_set_pv(pid_calc_t *p, float pv);

/** @brief Returns the set point. */
float pidlib_read_sp(pid_calc_t *p);

/** @brief Returns the process variable. */
float pidlib_read_pv(pid_calc_t *p);

/** @brief Returns |set point - process variable|. */
float pidlib_read_error(pid_calc_t *p);

/** @brief Returns the last output. */
float pidlib_read_output(pid_calc_t *p);

/** @brief Sets the output range (also the range of the integral). */
void pidlib_set_limits(pid_calc_t *p, float min, float max);

/** @brief Sets the gains. */
void pidlib_set_gains(pid_calc_t *p, float kp, float ki, float kd);

/**
 * @brief Forgets the history (integral, previous error, time).
 *
 * Use it after a pause, so the first calculation does not see a long dt.
 */
void pidlib_reset(pid_calc_t *p);

/**
 * @brief Computes a new output.
 * @param p           The controller.
 * @param system_time Current time in microseconds.
 * @return The output, within the limits (-1 when @p p is NULL).
 */
float pidlib_calculate(pid_calc_t *p, uint64_t system_time);

#endif /* PIDLIB_H */
