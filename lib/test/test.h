/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Minimal test helpers for the library host tests: a failed check prints
 * where and what, counts, and the test goes on; TEST_END() prints the result
 * and returns the exit code from main().
 */

#ifndef LIB_TEST_H
#define LIB_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int test_failures;

/* the condition must hold */
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			test_failures++;                                    \
		}                                                           \
	} while (0)

/* a and b must not differ by more than tol */
#define CHECK_NEAR(a, b, tol)                                                                      \
	do {                                                                                       \
		double _a = (a), _b = (b);                                                         \
		if (!(_a - _b <= (tol) && _b - _a <= (tol))) {                                     \
			printf("FAIL %s:%d: %s = %g, expected %g +- %g\n", __FILE__, __LINE__, #a, \
			       _a, _b, (double)(tol));                                             \
			test_failures++;                                                           \
		}                                                                                  \
	} while (0)

/* "name: ok" or "name: FAILED", and the exit code */
#define TEST_END(name)                                                     \
	do {                                                               \
		printf("%s: %s\n", name, test_failures ? "FAILED" : "ok"); \
		return test_failures != 0;                                 \
	} while (0)

#endif /* LIB_TEST_H */
