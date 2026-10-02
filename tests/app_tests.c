// The app tests: the helpers the screens share, linked with the app itself but run without a window.
#include "suites.h"
#include "test.h"

int main(void) {
    app_format_tests();
    app_common_tests();
    app_store_tests();
    app_sql_tests();
    return test_summary();
}
