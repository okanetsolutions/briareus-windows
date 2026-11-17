#include "github_account.h"
#include "str.h"

static char *account_list(const Json *values, const char *empty) {
    Str out; str_init(&out);
    for (size_t i = 0; i < json_count(values); i++) {
        const char *value = json_str_nonempty(json_at(values, i));
        if (!value) continue;
        if (out.len) str_appendz(&out, ", ");
        str_appendz(&out, value);
    }
    if (!out.len) str_appendz(&out, empty);
    return str_detach(&out);
}

char *github_account_scopes(const Json *account) {
    const Json *scopes = json_get(account, "scopes");
    if (json_is_null(scopes)) return xstrdup("scopes unknown (fine-grained token)");
    return account_list(scopes, "no classic scopes reported");
}

char *github_account_projects(const Json *account) {
    return account_list(json_get(account, "repos"), "No assigned projects");
}
