/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#include "date.h"

#include <sys/time.h>
#include <sys/timex.h>
#include <time.h>
#include <lets/node.h>

#include <limits.h>

#include <stdio.h>
#include <string.h>

#define DATE_TEXT 20u		/* "YYYY-MM-DD hh:mm:ss" and its 0 */
#define DATE_DAY  86400ul	/* seconds */
#define DATE_MAX_YEAR 2105	/* seconds since 1970 fit 32 bits up to here */

/* -------------------------------------------------------------------------- */
/* the kernel's clocks (kernel/clock.c), as the date_*() calls of before */

uint32_t date_uptime(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);

	return (uint32_t)t.tv_sec;
}

uint64_t date_now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

uint32_t date_now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (uint32_t)tv.tv_sec;
}

void date_set_us(uint64_t us)
{
	struct timeval tv;

	tv.tv_sec = (time_t)(us / 1000000u);
	tv.tv_usec = (suseconds_t)(us % 1000000u);
	settimeofday(&tv, NULL);
}

void date_set(uint32_t seconds)
{
	date_set_us((uint64_t)seconds * 1000000u);
}

/* a slew of at most a long's us (35 min with 32 bits): a longer one is a step's business */
void date_adjust_us(int64_t delta)
{
	struct timex t = {ADJ_OFFSET_SINGLESHOT, 0, 0, 0};

	t.offset = (delta > LONG_MAX) ? LONG_MAX : (delta < -LONG_MAX) ? -LONG_MAX : (long)delta;
	adjtimex(&t);
}

int64_t date_pending_us(void)
{
	struct timex t = {0, 0, 0, 0};

	adjtimex(&t);

	return t.offset;
}

void date_set_rate_ppb(int32_t ppb)
{
	int64_t f = (int64_t)ppb * 65536;
	struct timex t = {ADJ_FREQUENCY, 0, 0, 0};

	t.freq = (long)((f + ((f < 0) ? -500 : 500)) / 1000); /* ppm << 16 */
	adjtimex(&t);
}

int32_t date_rate_ppb(void)
{
	struct timex t = {0, 0, 0, 0};

	adjtimex(&t);

	int64_t f = (int64_t)t.freq * 1000;

	return (int32_t)((f + ((f < 0) ? -32768 : 32768)) / 65536);
}

bool date_is_set(void)
{
	struct timex t = {0, 0, 0, 0};

	adjtimex(&t);

	return (t.status & STA_UNSYNC) == 0;
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

/* the number at *@p text (at most 9999: more digits stop adding), -1 if there is none */
static int date_field(const char **text)
{
	const char *p = *text;
	int n = 0;

	if (*p < '0' || *p > '9') {
		return -1;
	}

	for (; *p >= '0' && *p <= '9'; p++) {
		n = (n < 1000) ? n * 10 + (*p - '0') : 10000;
	}

	*text = p;

	return n;
}

/* @p separator and then a number, -1 if not */
static int date_next(const char **text, char separator)
{
	if (**text != separator) {
		return -1;
	}

	(*text)++;

	return date_field(text);
}

/* the end of what date_parse() reads */
static bool date_end(char c)
{
	return c == '\0' || c == '\n' || c == '\r';
}

int date_parse(const char *text, uint32_t *seconds)
{
	const char *p = text;

	if (*p == '@') {
		uint32_t n = 0;

		/* more than 10 digits, or more than 32 bits: not a time */
		for (p++; *p >= '0' && *p <= '9' && n <= (0xFFFFFFFFul - 9u) / 10u; p++) {
			n = n * 10u + (uint32_t)(*p - '0');
		}

		if (p == text + 1 || !date_end(*p)) {
			return -1;
		}

		*seconds = n;

		return 0;
	}

	int y = date_field(&p);
	int m = date_next(&p, '-');
	int d = date_next(&p, '-');
	int hour = 0, minute = 0, second = 0;

	if (*p == ' ') {
		hour = date_next(&p, ' ');
		minute = date_next(&p, ':');
		second = date_next(&p, ':');
	}

	/* a day the month has: no 31 April, no 29 February out of a leap year */
	if (y < 1970 || y > DATE_MAX_YEAR || m < 1 || m > 12 || d < 1 ||
	    (unsigned int)d > date_days((unsigned int)y, (unsigned int)m) || hour < 0 || hour > 23 ||
	    minute < 0 || minute > 59 || second < 0 || second > 59 || !date_end(*p)) {
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

	*seconds = days * DATE_DAY + (uint32_t)((unsigned int)hour * 60u + (unsigned int)minute) * 60ul +
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
	printf("%s%s\n", text, date_is_set() ? "" : " (not set)");

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
