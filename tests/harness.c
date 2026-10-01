// The runner behind test.h.
#include "test.h"

int test_failures = 0, test_checks = 0;
static int tests = 0;

void test_run(const char *name, void (*fn)(void)) {
    int before = test_failures;
    tests++;
    fn();
    printf("%s %s\n", test_failures == before ? "ok  " : "FAIL", name);
}

int test_summary(void) {
    printf("\n%d tests, %d checks, %d failures\n", tests, test_checks, test_failures);
    return test_failures ? 1 : 0;
}
