// The app tests: the helpers the screens share, linked with the app itself but run without a window.
#include "suites.h"
#include "test.h"

int main(void) {
    app_status_tests();
    app_webhook_tests();
    app_conversation_tests();
    app_format_tests();
    app_common_tests();
    app_sidebar_tests();
    app_find_tests();
    app_store_tests();
    app_contract_tests();
    app_sql_tests();
    app_mcp_settings_tests();
    app_provider_settings_tests();
    app_project_settings_tests();
    app_home_assistant_tests();
    app_dashboard_tests();
    app_mail_tests();
    app_mail_consent_tests();
    app_mail_inbox_tests();
    app_mail_delete_tests();
    app_pulls_tests();
    return test_summary();
}
