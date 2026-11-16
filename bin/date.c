/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "date.h"

#include "../kernel/task.h"
#include "../vfs/vfs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DATE_TEXT 20u		/* "YYYY-MM-DD hh:mm:ss" and its 0 */
#define DATE_DAY  86400ul	/* seconds */
#define DATE_MAX_YEAR 2105	/* seconds since 1970 fit 32 bits up to here */

#define DATE_SLEW_PPM 500		/* a slew moves the clock at most this fast */
#define DATE_ANCHOR_US 0x40000000ull	/* re-anchored after this long (~18 min): small products */

/*
 * The time is counted from an anchor: what the kernel's clock read then (up0) and the time it
 * was (utc0), both in µs. Since then it runs at the kernel's rate corrected by date_rate (ppb),
 * and a slew still to be applied (date_slew) goes in at DATE_SLEW_PPM.
 */
static uint64_t date_up0;
static uint64_t date_utc0;
static int32_t date_rate;
static int64_t date_slew;
static bool date_valid;

/* the time at kernel time @p up, and in @p slewed how much of the slew is in it */
static uint64_t date_at(uint64_t up, int64_t *slewed)
{
	uint64_t dt = up - date_up0;
	int64_t corr = (int64_t)dt * date_rate / 1000000000ll;
	int64_t most = (int64_t)(dt / (1000000ull / DATE_SLEW_PPM));
	int64_t s = date_slew;

	if (s > most) {
		s = most;
	} else if (s < -most) {
		s = -most;
	}

	*slewed = s;

	return date_utc0 + dt + (uint64_t)(corr + s);
}

/* moves the anchor to now */
static uint64_t date_anchor(void)
{
	uint64_t up = task_get_system_time();
	int64_t s;
	uint64_t now = date_at(up, &s);

	date_up0 = up;
	date_utc0 = now;
	date_slew -= s;

	return now;
}

uint32_t date_uptime(void)
{
	return (uint32_t)(task_get_system_time() / 1000000ull);
}

uint64_t date_now_us(void)
{
	uint64_t up = task_get_system_time();
	int64_t s;

	if (up - date_up0 >= DATE_ANCHOR_US) {
		return date_anchor();
	}

	return date_at(up, &s);
}

uint32_t date_now(void)
{
	return (uint32_t)(date_now_us() / 1000000ull);
}

void date_set_us(uint64_t us)
{
	date_up0 = task_get_system_time();
	date_utc0 = us;
	date_slew = 0;
	date_valid = true;
}

void date_set(uint32_t seconds)
{
	date_set_us((uint64_t)seconds * 1000000ull);
}

void date_adjust_us(int64_t delta)
{
	date_anchor();
	date_slew += delta;
}

int64_t date_pending_us(void)
{
	int64_t s;

	date_at(task_get_system_time(), &s);

	return date_slew - s;
}

void date_set_rate_ppb(int32_t ppb)
{
	date_anchor();
	date_rate = ppb;
}

int32_t date_rate_ppb(void)
{
	return date_rate;
}

bool date_is_set(void)
{
	return date_valid;
}

/*
 * Calendar days by counting: whole years from 1970, then whole months. Up to 2106 (32-bit seconds)
 * a year divisible by 4 is a leap year, except 2100. Counting is less code than the division
 * formulas on a 16-bit chip, and at most 136 + 11 steps.
 */
static unsigned int date_days(unsigned int year, unsigned int month)
{
	if (month == 0u) {
		return ((year & 3u) == 0u && year != 2100u) ? 366u : 365u; /* the whole year */
	}

	if (month == 2u) {
		return 28u + (date_days(year, 0) - 365u);
	}

	return 30u + ((month + (month >> 3)) & 1u); /* 31: Jan, Mar, May, Jul, Aug, Oct, Dec */
}

void date_format(uint32_t seconds, char *text)
{
	unsigned int days = (unsigned int)(seconds / DATE_DAY);
	unsigned int minutes = (unsigned int)(seconds % DATE_DAY / 60u);
	unsigned int year = 1970u;
	unsigned int month = 1u;

	while (days >= date_days(year, 0)) {
		days -= date_days(year++, 0);
	}

	while (days >= date_days(year, month)) {
		days -= date_days(year, month++);
	}

	/*
	 * Each field is in its range here (year < 2107, the others 2 digits), so the text is 19
	 * characters; the compiler cannot see that and would warn of a cut (the % that would show it
	 * costs code on a small chip).
	 */
#if defined(__GNUC__) && __GNUC__ >= 7
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
	snprintf(text, DATE_TEXT, "%04u-%02u-%02u %02u:%02u:%02u", year, month, days + 1u, minutes / 60u,
		 minutes % 60u, (unsigned int)(seconds % 60u));
#if defined(__GNUC__) && __GNUC__ >= 7
#pragma GCC diagnostic pop
#endif
}

/* the number at *@p text, -1 if there is none */
static long date_field(const char **text)
{
	const char *p = *text;
	long n = 0;

	if (*p < '0' || *p > '9') {
		return -1;
	}

	while (*p >= '0' && *p <= '9') {
		n = n * 10 + (*p++ - '0');
	}

	*text = p;

	return n;
}

/* @p separator and then a number, -1 if not */
static long date_next(const char **text, char separator)
{
	if (**text != separator) {
		return -1;
	}

	(*text)++;

	return date_field(text);
}

int date_parse(const char *text, uint32_t *seconds)
{
	const char *p = text;

	if (*p == '@') {
		uint32_t n = 0;

		for (p++; *p >= '0' && *p <= '9'; p++) {
			n = n * 10u + (uint32_t)(*p - '0');
		}

		if (p == text + 1 || (*p != '\0' && *p != '\n' && *p != '\r')) {
			return -1;
		}

		*seconds = n;

		return 0;
	}

	long y = date_field(&p);
	long m = date_next(&p, '-');
	long d = date_next(&p, '-');
	long hour = 0, minute = 0, second = 0;

	if (*p == ' ') {
		hour = date_next(&p, ' ');
		minute = date_next(&p, ':');
		second = date_next(&p, ':');
	}

	if (y < 1970 || y > DATE_MAX_YEAR || m < 1 || m > 12 || d < 1 || d > 31 || hour < 0 ||
	    hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
	    (*p != '\0' && *p != '\n' && *p != '\r')) {
		return -1;
	}

	/* as date_format(), the other way */
	uint32_t days = (uint32_t)d - 1u;

	for (unsigned int i = 1970u; i < (unsigned int)y; i++) {
		days += date_days(i, 0);
	}

	for (unsigned int i = 1u; i < (unsigned int)m; i++) {
		days += date_days((unsigned int)y, i);
	}

	*seconds = days * DATE_DAY + (uint32_t)hour * 3600ul + (uint32_t)minute * 60ul +
		   (uint32_t)second;

	return 0;
}

/* -------------------------------------------------------------------------- */

static int date_read(void *args, void *buffer, unsigned int length)
{
	char text[DATE_TEXT + 1u];

	date_format(date_now(), text);
	strcat(text, "\n");

	unsigned int n = (unsigned int)strlen(text);

	n = (n > length) ? length : n;
	memcpy(buffer, text, n);

	return (int)n;
}

static int date_write(void *args, void *buffer, unsigned int length)
{
	char text[DATE_TEXT + 1u];
	uint32_t seconds;

	if (length >= sizeof(text)) {
		return -1;
	}

	memcpy(text, buffer, length);
	text[length] = '\0';

	if (date_parse(text, &seconds) < 0) {
		return -1;
	}

	date_set(seconds);

	return (int)length;
}

int date_execute(void *args, int argc, char *argv[])
{
	char text[DATE_TEXT];
	uint32_t seconds;

	if (argc == 2 && strcmp(argv[1], "+%s") == 0) {
		printf("%lu\n", (unsigned long)date_now());
		return 0;
	}

	if (argc == 2 && strcmp(argv[1], "+%s.%N") == 0) {
		uint64_t us = date_now_us();

		printf("%lu.%06lu\n", (unsigned long)(us / 1000000ull), (unsigned long)(us % 1000000ull));
		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-s") == 0 && date_parse(argv[2], &seconds) == 0) {
		date_set(seconds);
		return 0;
	}

	if (argc != 1) {
		printf("usage: date [+%%s | +%%s.%%N] [-s \"YYYY-MM-DD hh:mm:ss\"]\n");
		return -1;
	}

	date_format(date_now(), text);
	printf("%s%s\n", text, date_valid ? "" : " (not set)");

	return 0;
}

int date_uptime_execute(void *args, int argc, char *argv[])
{
	uint32_t up = date_uptime();
	unsigned int rest = (unsigned int)(up % DATE_DAY / 60u);

	printf("up %lu days %02u:%02u:%02u\n", (unsigned long)(up / DATE_DAY), rest / 60u, rest % 60u,
	       (unsigned int)(up % 60u));

	return 0;
}

static const vfs_handler_t date_node = {
	.read_handler = date_read,
	.write_handler = date_write,
};

int date_initialize(void)
{
	return (vfs_node_create("date", false, NULL, &date_node) < 0) ? -1 : 0;
}
