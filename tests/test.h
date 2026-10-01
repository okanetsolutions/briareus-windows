// The test harness both test programs share: CHECK macros that count instead of stopping, and a runner that prints one line per test.
#ifndef BRIAREUS_TEST_H
#define BRIAREUS_TEST_H
#include "str.h"
#include <stdio.h>

extern int test_failures, test_checks;

#define CHECK(cond) do { test_checks++; if (!(cond)) { test_failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { test_checks++; const char *_a = (a), *_b = (b); if (!str_eq(_a, _b)) { test_failures++; printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define CHECK_INT(a, b) do { test_checks++; long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { test_failures++; printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
/// Checks a new string against an expected one and frees it.
#define CHECK_OWNED_STR(a, b) do { char *_o = (a); CHECK_STR(_o, b); free(_o); } while (0)

/// Runs one test and prints whether it passed.
void test_run(const char *name, void (*fn)(void));
/// Prints the totals; the process exit code.
int test_summary(void);

#endif
