// The runner behind test.h.
#include "test.h"
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int test_failures = 0, test_checks = 0;
static int tests = 0;
static const char *current;

// A crash names the test it happened in: a UBSan trap is an illegal instruction, a fortify or stack check an abort.
static void report_crash(const char *what) {
    fflush(stdout);
    fprintf(stderr, "\nCRASH in %s: %s\n", current ? current : "(no test)", what);
    fflush(stderr);
}
static LONG WINAPI on_exception(EXCEPTION_POINTERS *info) {
    char what[64];
    snprintf(what, sizeof what, "exception 0x%08lX", (unsigned long)info->ExceptionRecord->ExceptionCode);
    report_crash(what);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void on_abort(int sig) {
    (void)sig;
    report_crash("abort");
    _exit(3);
}

#if defined(_MSC_VER) && defined(_DEBUG)
// In file mode the debug CRT prints an error or assert and carries on, so the hook fails the test; returning FALSE
// still lets the report print. /RTC and heap reports come through either the narrow or the wide entry point.
static int count_report(int type) {
    if (type == _CRT_ERROR || type == _CRT_ASSERT) test_failures++;
    return FALSE;
}
static int __cdecl on_report(int type, char *message, int *ret) { (void)message; (void)ret; return count_report(type); }
static int __cdecl on_report_w(int type, wchar_t *message, int *ret) { (void)message; (void)ret; return count_report(type); }
#endif

static void harness_init(void) {
    SetUnhandledExceptionFilter(on_exception);
    signal(SIGABRT, on_abort);
#if defined(_MSC_VER) && defined(_DEBUG)
    // The debug CRT's asserts and /RTC failures go to stderr: a message box would hang CI.
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, on_report);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, on_report_w);
#endif
}

void test_run(const char *name, void (*fn)(void)) {
    if (!tests) harness_init();
    int before = test_failures;
    tests++;
    current = name;
#if defined(_MSC_VER) && defined(_DEBUG)
    // Debug builds (the CI leak job) fail a test that leaves memory it allocated behind. Caches allocate with
    // xmalloc_kept, as client blocks, so only normal blocks count.
    _CrtMemState start, end, diff;
    _CrtMemCheckpoint(&start);
    fn();
    _CrtMemCheckpoint(&end);
    // The counts are unsigned: a test that frees memory from before it started wraps them, so they are read as signed.
    _CrtMemDifference(&diff, &start, &end);
    ptrdiff_t blocks = (ptrdiff_t)diff.lCounts[_NORMAL_BLOCK];
    if (blocks > 0) {
        test_failures++;
        printf("  FAIL leaked %td blocks, %td bytes:\n", blocks, (ptrdiff_t)diff.lSizes[_NORMAL_BLOCK]);
        fflush(stdout);
        _CrtMemDumpAllObjectsSince(&start);
    }
#else
    fn();
#endif
    current = NULL;
    printf("%s %s\n", test_failures == before ? "ok  " : "FAIL", name);
}

int test_summary(void) {
    printf("\n%d tests, %d checks, %d failures\n", tests, test_checks, test_failures);
    return test_failures ? 1 : 0;
}
