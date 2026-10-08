// Each suite file runs its own tests through test_run; the two programs' main functions call the suites in turn.
#ifndef BRIAREUS_SUITES_H
#define BRIAREUS_SUITES_H

// core_tests.exe
void str_tests(void);
void json_tests(void);
void markdown_tests(void);
void diff_tests(void);
void models_tests(void);
void board_tests(void);
void api_tests(void);
void contract_tests(void);
void cache_tests(void);
void vt_tests(void);
void sftp_tests(void);
void browser_tests(void);
void meet_tests(void);
void mcp_tests(void);
void update_tests(void);
void repo_tests(void);
void mail_tests(void);
void mail_inbox_tests(void);
void app_mail_inbox_tests(void);
void repo_index_tests(void);
void slack_tests(void);

// app_tests.exe
void app_format_tests(void);
void app_common_tests(void);
void app_store_tests(void);
void app_contract_tests(void);
void app_sql_tests(void);
void app_slack_tests(void);
void app_mcp_settings_tests(void);
void app_mail_tests(void);
void app_pulls_tests(void);

#endif
