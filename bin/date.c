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

static uint32_t date_base; /* the time at reset */
static bool date_valid;

uint32_t date_uptime(void)
{
	return (uint32_t)(task_get_system_time() / 1000000ull);
}

uint32_t date_now(void)
{
	return date_base + date_uptime();
}

void date_set(uint32_t seconds)
{
	date_base = seconds - date_uptime();
	date_valid = true;
}

bool date_is_set(void)
{
	return date_valid;
}

/*
 * Days from 1970-03-01 stand on years that start in March, so that the leap day
 * is the last day of a year: 153 days for each 5 months is what the months of
 * March to January add up to, and February needs no case of its own.
 */
void date_format(uint32_t seconds, char *text)
{
	uint32_t days = seconds / DATE_DAY + 719468ul; /* from 0000-03-01 */
	unsigned int rest = (unsigned int)(seconds % DATE_DAY / 60u);

	uint32_t era = days / 146097ul;			/* 400 years */
	uint32_t day = days - era * 146097ul;
	uint32_t y = (day - day / 1460ul + day / 36524ul - day / 146096ul) / 365ul;
	uint32_t yday = day - (365ul * y + y / 4ul - y / 100ul);
	uint32_t m = (5ul * yday + 2ul) / 153ul;

	unsigned int d = (unsigned int)(yday - (153ul * m + 2ul) / 5ul + 1ul);
	unsigned int month = (unsigned int)((m < 10ul) ? m + 3ul : m - 9ul);
	unsigned int year = (unsigned int)(y + era * 400ul + (month <= 2u ? 1ul : 0ul));

	snprintf(text, DATE_TEXT, "%04u-%02u-%02u %02u:%02u:%02u", year, month, d, rest / 60u, rest % 60u,
		(unsigned int)(seconds % 60u));
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
	uint32_t ym = (uint32_t)((m <= 2) ? y - 1 : y);
	uint32_t mp = (uint32_t)((m > 2) ? m - 3 : m + 9);
	uint32_t days = 365ul * ym + ym / 4ul - ym / 100ul + ym / 400ul +
			(153ul * mp + 2ul) / 5ul + (uint32_t)d - 1ul - 719468ul;

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

	if (argc == 3 && strcmp(argv[1], "-s") == 0 && date_parse(argv[2], &seconds) == 0) {
		date_set(seconds);
		return 0;
	}

	if (argc != 1) {
		printf("usage: date [+%%s] [-s \"YYYY-MM-DD hh:mm:ss\"]\n");
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
