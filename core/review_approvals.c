#include "review_approvals.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *nonempty(const Json *value) { return xstrdup(json_str_nonempty(value)); }
// A whole id from 1 up, as JSON numbers carry them exactly; 0 otherwise.
static int64_t id_of(const Json *value) {
    double n;
    if (!json_num(value, &n) || n < 1 || n > 9007199254740991.0 || floor(n) != n) return 0;
    return (int64_t)n;
}
static bool approval_parse(const Json *j, ReviewApproval *out) {
    memset(out, 0, sizeof *out);
    out->id = id_of(json_get(j, "id"));
    if (!out->id) return false;
    const Json *by = json_get(j, "by");
    out->from_github = str_eq(json_str(json_get(j, "source")), "github");
    out->user_id = id_of(json_get(by, "userId"));
    out->name = nonempty(json_get(by, "name")); out->github_login = nonempty(json_get(by, "githubLogin"));
    out->head_sha = xstrdup(json_str_or(json_get(j, "headSha"), ""));
    out->at = nonempty(json_get(j, "at")); out->note = nonempty(json_get(j, "note"));
    out->withdrawn_at = nonempty(json_get(j, "withdrawnAt")); out->withdraw_note = nonempty(json_get(j, "withdrawNote"));
    out->withdrawn = out->withdrawn_at != NULL;
    out->counted = json_bool_is(json_get(j, "counted"), true);
    out->reason = nonempty(json_get(j, "reason")); out->message = nonempty(json_get(j, "message"));
    return true;
}
static void approval_free(ReviewApproval *a) {
    free(a->name); free(a->github_login); free(a->head_sha); free(a->at); free(a->note);
    free(a->withdrawn_at); free(a->withdraw_note); free(a->reason); free(a->message);
}
static void risk_parse(const Json *list, ReviewApprovals *out) {
    out->risk = xcalloc(json_count(list) + 1, sizeof *out->risk);
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *r = json_at(list, i);
        const char *code = json_str_nonempty(json_get(r, "code")), *message = json_str_nonempty(json_get(r, "message"));
        if (!code) continue;
        RiskReason *reason = &out->risk[out->risk_count++];
        reason->code = xstrdup(code); reason->tier = nonempty(json_get(r, "tier"));
        reason->message = message ? xstrdup(message) : str_replace(code, "_", " ");
    }
}
void review_approvals_parse(const Json *j, ReviewApprovals *out) {
    memset(out, 0, sizeof *out);
    const char *result = json_str_nonempty(json_get(j, "result"));
    if (!json_is_object(j) || !result) return;
    out->present = true; out->result = xstrdup(result);
    out->enabled = json_bool_is(json_get(j, "enabled"), true);
    out->review_current = json_bool_is(json_get(j, "reviewCurrent"), true);
    out->tier = nonempty(json_get(j, "tier")); out->explanation = nonempty(json_get(j, "explanation"));
    out->head_sha = nonempty(json_get(j, "headSha"));
    out->required = json_int_or(json_get(j, "required"), 0); out->valid = json_int_or(json_get(j, "valid"), 0);
    out->missing = json_int_or(json_get(j, "missing"), 0);
    risk_parse(json_get(json_get(j, "risk"), "reasons"), out);
    review_reasons_parse(json_get(j, "reasons"), &out->reasons);
    const Json *list = json_get(j, "approvals");
    out->items = xcalloc(json_count(list) + 1, sizeof *out->items);
    for (size_t i = 0; i < json_count(list); i++) if (approval_parse(json_at(list, i), &out->items[out->count])) out->count++;
}
void review_approvals_free(ReviewApprovals *a) {
    if (!a) return;
    free(a->result); free(a->tier); free(a->explanation); free(a->head_sha);
    for (size_t i = 0; i < a->risk_count; i++) { free(a->risk[i].code); free(a->risk[i].tier); free(a->risk[i].message); }
    free(a->risk);
    review_reasons_free(&a->reasons);
    for (size_t i = 0; i < a->count; i++) approval_free(&a->items[i]);
    free(a->items);
    memset(a, 0, sizeof *a);
}

char *review_approval_who(const ReviewApproval *a) {
    if (a->name) return xstrdup(a->name);
    return a->github_login ? xstrfmt("@%s", a->github_login) : xstrdup("Someone");
}
char *review_approval_state(const ReviewApproval *a) {
    if (a->counted) return xstrdup("Counts");
    if (a->withdrawn) return xstrdup("Withdrawn");
    if (a->message) return xstrdup(a->message);
    return a->reason ? str_replace(a->reason, "_", " ") : xstrdup("Does not count");
}
ReviewTone review_approval_tone(const ReviewApproval *a) { return a->counted ? REVIEW_TONE_OK : REVIEW_TONE_MUTED; }
char *review_approval_detail(const ReviewApproval *a) {
    Str text; str_init(&text);
    if (a->from_github) str_appendz(&text, "on GitHub");
    if (a->head_sha && *a->head_sha) str_appendf(&text, "%s%.7s", text.len ? " \xC2\xB7 " : "", a->head_sha);
    return text.data ? str_detach(&text) : xstrdup("");
}
char *review_approval_withdraw_line(const ReviewApproval *a) { return a->withdraw_note ? xstrfmt("Withdrawn: %s", a->withdraw_note) : NULL; }
bool review_approval_can_withdraw(const ReviewApproval *a, int64_t me) {
    if (a->from_github || a->withdrawn) return false;
    return !me || !a->user_id || me == a->user_id;
}
static bool counts_for(const ReviewApproval *a, int64_t me, const char *head) { return a->counted && a->user_id == me && str_eq(a->head_sha, head); }
bool review_approvals_can_approve(const ReviewApprovals *a, int64_t me) {
    if (!a->present || !a->enabled || str_eq(a->result, "not_required")) return false;
    if (!a->review_current || !a->head_sha) return false;
    if (!me) return true;
    for (size_t i = 0; i < a->count; i++) if (counts_for(&a->items[i], me, a->head_sha)) return false;
    return true;
}
char *review_approvals_approve_consequence(const ReviewApprovals *a) {
    char *head = a->head_sha ? xstrfmt("%.7s", a->head_sha) : xstrdup("this head");
    char *text = xstrfmt("Your approval counts for %s only, and stops counting once the pull request moves on.", head);
    free(head);
    return text;
}
const char *review_approval_withdraw_consequence(void) { return "It stays on record and no longer counts."; }

static void set_note(Json *args, const char *note) {
    char *trimmed = str_trim(note ? note : "");
    if (*trimmed) json_set_str(args, "note", trimmed);
    free(trimmed);
}
static Json *pull_args(const char *repo, int number) {
    Json *args = json_object();
    json_set_str(args, "repo", repo); json_set_num(args, "pr", number);
    return args;
}
Json *review_approval_approve_args(const char *repo, int number, const char *head_sha, const char *note) {
    Json *args = pull_args(repo, number);
    json_set_str(args, "headSha", head_sha);
    set_note(args, note);
    return args;
}
Json *review_approval_withdraw_args(const char *repo, int number, int64_t approval_id, const char *note) {
    Json *args = pull_args(repo, number);
    json_set_num(args, "approvalId", (double)approval_id);
    set_note(args, note);
    return args;
}
bool review_approval_rereads(const char *code) {
    return str_eq(code, "head_moved") || str_eq(code, "review_not_current") || str_eq(code, "review_running");
}
