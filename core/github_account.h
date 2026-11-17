#ifndef BRIAREUS_GITHUB_ACCOUNT_H
#define BRIAREUS_GITHUB_ACCOUNT_H
#include "json.h"

/// Owned display summaries of the account's reported scopes and assigned projects.
char *github_account_scopes(const Json *account);
char *github_account_projects(const Json *account);
#endif
