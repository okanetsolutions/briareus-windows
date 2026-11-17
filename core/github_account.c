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
    if (json_bool_is(json_get(account, "globalFallback"), true)) {
        return xstrdup(json_bool_is(json_get(account, "hasToken"), false)
            ? "Server default (GITHUB_TOKEN not configured)"
            : "Server default (GITHUB_TOKEN)");
    }
    const char *login = json_str_or(json_get(account, "login"), "unknown");
    const char *label = json_str_nonempty(json_get(account, "label"));
    return label ? xstrfmt("%s (@%s)", label, login) : xstrfmt("@%s", login);
}

static char *http_suffix(const Json *check) {
    int status = json_int_or(json_get(check, "httpStatus"), 0);
    return status ? xstrfmt("; HTTP %d", status) : xstrdup("");
}

char *github_account_health_overall(const Json *health) {
    return xstrfmt("Healthy: %s", json_bool_is(json_get(health, "healthy"), true) ? "yes" : "no");
}

char *github_account_health_token(const Json *health) {
    const Json *token = json_get(health, "token");
    char *suffix = http_suffix(token);
    char *text = xstrfmt("Token: %s%s", json_str_or(json_get(token, "status"), "unavailable"), suffix);
    free(suffix); return text;
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
    char *suffix = http_suffix(repo);
    char *text = xstrfmt("%s: %s (%s%s)", json_str_or(json_get(repo, "repo"), "unknown"), status, access, suffix);
    free(suffix); return text;
}

static char *reset_time(double ms) {
    if (!isfinite(ms) || ms <= 0 || ms > 253402300799000.0) return NULL;
    time_t seconds = (time_t)(ms / 1000); struct tm *utc = gmtime(&seconds);
    char date[40];
    return utc && strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S UTC", utc) ? xstrdup(date) : NULL;
}

static char *allowance(const Json *value) {
    double n;
    return json_num(value, &n) && isfinite(n) && n >= 0 ? xstrfmt("%.0f", n) : xstrdup("unknown");
}

char *github_account_health_rate(const Json *health, const char *bucket, double now_ms) {
    const Json *limits = json_get(health, "rateLimit"), *rate = json_get(limits, bucket);
    char *remaining = allowance(json_get(rate, "remaining")), *limit = allowance(json_get(rate, "limit"));
    char *reset = reset_time(json_num_or(json_get(rate, "resetAt"), 0));
    double until = json_num_or(json_get(json_get(limits, "cooldownUntil"), bucket), 0);
    char *cooldown = until > now_ms ? reset_time(until) : NULL;
    char *text = xstrfmt("%s: %s / %s remaining; resets %s%s%s", str_eq(bucket, "core") ? "REST" : "GraphQL",
        remaining, limit, reset ? reset : "unknown", cooldown ? "; retry after " : "", cooldown ? cooldown : "");
    free(remaining); free(limit); free(reset); free(cooldown); return text;
}
