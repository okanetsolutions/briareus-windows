#ifndef BRIAREUS_GITHUB_ACCOUNT_H
#define BRIAREUS_GITHUB_ACCOUNT_H
#include "json.h"

/// Owned display summaries of the account's reported scopes and assigned projects.
char *github_account_scopes(const Json *account);
char *github_account_projects(const Json *account);
/// Owned summaries of the project identity and the health endpoint's structured diagnostics.
char *github_account_identity(const Json *account);
char *github_account_health_overall(const Json *health);
char *github_account_health_token(const Json *health);
char *github_account_health_scopes(const Json *health);
char *github_account_health_repo(const Json *repo);
/// `now_ms` is epoch milliseconds, used to omit elapsed cooldowns.
char *github_account_health_rate(const Json *health, const char *bucket, double now_ms);
#endif
