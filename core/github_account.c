#include "github_account.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>

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

char *github_account_identity(const Json *account) {
    if (json_bool_is(json_get(account, "globalFallback"), true)) return xstrdup("Server default (GITHUB_TOKEN)");
    const char *login = json_str_or(json_get(account, "login"), "unknown");
    const char *label = json_str_nonempty(json_get(account, "label"));
    return label ? xstrfmt("%s (@%s)", label, login) : xstrfmt("@%s", login);
}

char *github_account_health_scopes(const Json *health) {
    const Json *scopes = json_get(health, "scopes");
    const char *status = json_str_or(json_get(scopes, "status"), "unknown");
    if (str_eq(status, "unknown")) return xstrdup("unknown");
    if (!str_eq(status, "missing")) return xstrdup(status);
    char *missing = account_list(json_get(scopes, "missing"), "unknown");
    char *text = xstrfmt("missing: %s", missing); free(missing); return text;
}

char *github_account_health_repo(const Json *repo) {
    const Json *accessible = json_get(repo, "accessible");
    const char *access = json_is_null(accessible) ? "access unknown"
        : json_bool_is(accessible, true) ? "accessible" : "access denied";
    const char *status = json_str_or(json_get(repo, "status"), "not_checked");
    int http = json_int_or(json_get(repo, "httpStatus"), 0);
    char *suffix = http ? xstrfmt("; HTTP %d", http) : xstrdup("");
    char *text = xstrfmt("%s: %s (%s%s)", json_str_or(json_get(repo, "repo"), "unknown"), status, access, suffix);
    free(suffix); return text;
}

static char *reset_time(const Json *value) {
    double ms = json_num_or(value, 0);
    char date[40] = "unknown";
    if (isfinite(ms) && ms > 0 && ms <= 253402300799000.0) {
        time_t seconds = (time_t)(ms / 1000); struct tm *utc = gmtime(&seconds);
        if (utc) strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S UTC", utc);
    }
    return xstrdup(date);
}

static char *allowance(const Json *value) {
    double n;
    return json_num(value, &n) && isfinite(n) && n >= 0 ? xstrfmt("%.0f", n) : xstrdup("unknown");
}

char *github_account_health_rate(const Json *health, const char *bucket) {
    const Json *limits = json_get(health, "rateLimit"), *rate = json_get(limits, bucket);
    char *remaining = allowance(json_get(rate, "remaining")), *limit = allowance(json_get(rate, "limit"));
    char *reset = reset_time(json_get(rate, "resetAt"));
    const Json *until = json_get(json_get(limits, "cooldownUntil"), bucket);
    char *cooldown = json_num_or(until, 0) > 0 ? reset_time(until) : NULL;
    char *text = xstrfmt("%s: %s / %s remaining; resets %s%s%s", str_eq(bucket, "core") ? "REST" : "GraphQL",
        remaining, limit, reset, cooldown ? "; retry after " : "", cooldown ? cooldown : "");
    free(remaining); free(limit); free(reset); free(cooldown); return text;
}
