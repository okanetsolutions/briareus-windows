#include "review_approvals.h"
#include "models.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static long long whole(const Json *value) { return (long long)json_num_or(value, 0); }

static bool approval_parse(const Json *j, ReviewApproval *out) {
    memset(out, 0, sizeof *out);
    double id;
    if (!json_num(json_get(j, "id"), &id)) return false;
    const Json *by = json_get(j, "by");
    out->id = (long long)id; out->user_id = whole(json_get(by, "userId"));
    out->from_github = str_eq(json_str(json_get(j, "source")), "github");
    out->name = json_dup_nonempty(json_get(by, "name")); out->github_login = json_dup_nonempty(json_get(by, "githubLogin"));
    out->head_sha = json_dup_nonempty(json_get(j, "headSha")); out->at = json_dup_nonempty(json_get(j, "at"));
    out->note = json_dup_nonempty(json_get(j, "note")); out->withdraw_note = json_dup_nonempty(json_get(j, "withdrawNote"));
    out->withdrawn = json_str_nonempty(json_get(j, "withdrawnAt")) != NULL;
    out->counted = json_bool_is(json_get(j, "counted"), true);
    out->reason = json_dup_nonempty(json_get(j, "reason")); out->message = json_dup_nonempty(json_get(j, "message"));
    return true;
}
static void approval_free(ReviewApproval *a) {
    free(a->name); free(a->github_login); free(a->head_sha); free(a->at); free(a->note); free(a->withdraw_note);
    free(a->reason); free(a->message);
}
static bool risk_parse(const Json *j, RiskReason *out) {
    memset(out, 0, sizeof *out);
    const char *code = json_str_nonempty(json_get(j, "code"));
    if (!code) return false;
    out->code = xstrdup(code); out->tier = json_dup_nonempty(json_get(j, "tier"));
    const char *message = json_str_nonempty(json_get(j, "message"));
    out->message = message ? xstrdup(message) : str_replace(code, "_", " ");
    return true;
}
static void risk_free(RiskReason *r) { free(r->code); free(r->tier); free(r->message); }
static DEFINE_LIST_PARSE(ReviewApproval, approvals_list, approval_parse)
static DEFINE_LIST_PARSE(RiskReason, risk_list, risk_parse)

void review_approvals_parse(const Json *j, ReviewApprovals *out) {
    memset(out, 0, sizeof *out);
    const char *result = json_str_nonempty(json_get(j, "result"));
    if (!json_is_object(j) || !result) return;
    out->present = true; out->result = xstrdup(result);
    out->enabled = json_bool_is(json_get(j, "enabled"), true);
    out->review_current = json_bool_is(json_get(j, "reviewCurrent"), true);
    out->tier = json_dup_nonempty(json_get(j, "tier")); out->explanation = json_dup_nonempty(json_get(j, "explanation"));
    out->head_sha = json_dup_nonempty(json_get(j, "headSha"));
    out->required = json_int_or(json_get(j, "required"), 0); out->valid = json_int_or(json_get(j, "valid"), 0);
    out->missing = json_int_or(json_get(j, "missing"), 0);
    review_reasons_parse(json_get(j, "reasons"), &out->reasons);
    out->risk = risk_list(json_get(json_get(j, "risk"), "reasons"), &out->risk_count);
    out->items = approvals_list(json_get(j, "approvals"), &out->count);
}
void review_approvals_free(ReviewApprovals *a) {
    if (!a) return;
    free(a->result); free(a->tier); free(a->explanation); free(a->head_sha);
    review_reasons_free(&a->reasons);
    for (size_t i = 0; i < a->risk_count; i++) risk_free(&a->risk[i]);
    for (size_t i = 0; i < a->count; i++) approval_free(&a->items[i]);
    free(a->risk); free(a->items);
    memset(a, 0, sizeof *a);
}

static bool not_required(const ReviewApprovals *a) { return str_eq(a->result, "not_required"); }
bool review_approvals_shown(const ReviewApprovals *a) { return a->present && a->enabled && (!not_required(a) || a->count > 0); }
char *review_approvals_line(const ReviewApprovals *a) {
    char *count = not_required(a) ? xstrdup("None needed") : xstrfmt("%d of %d approval%s", a->valid, a->required, a->required == 1 ? "" : "s");
    if (!a->tier) return count;
    char *line = xstrfmt("%s \xC2\xB7 %s risk", count, a->tier);
    free(count); return line;
}
ReviewTone review_approvals_tone(const ReviewApprovals *a) {
    if (str_eq(a->result, "satisfied")) return REVIEW_TONE_OK;
    return not_required(a) ? REVIEW_TONE_MUTED : REVIEW_TONE_WARN;
}
// Whether user `me` already has an approval that counts for `head`.
static bool approved_by(const ReviewApprovals *a, long long me, const char *head) {
    for (size_t i = 0; i < a->count; i++) {
        const ReviewApproval *x = &a->items[i];
        if (x->counted && x->user_id == me && str_eq(x->head_sha, head)) return true;
    }
    return false;
}
bool review_approvals_can_approve(const ReviewApprovals *a, long long me) {
    if (!a->present || !a->enabled || not_required(a)) return false;
    if (!a->review_current || !a->head_sha) return false;
    return !me || !approved_by(a, me, a->head_sha);
}
static char *short_sha(const char *sha) { return sha ? xstrndup(sha, strlen(sha) < 7 ? strlen(sha) : 7) : NULL; }
char *review_approvals_approve_consequence(const ReviewApprovals *a) {
    char *head = short_sha(a->head_sha);
    char *text = xstrfmt("Your approval counts for %s only, and stops counting once the pull request moves on.", head ? head : "this head");
    free(head); return text;
}
const ReviewApproval *review_approvals_find(const ReviewApprovals *a, long long id) {
    for (size_t i = 0; a && i < a->count; i++) if (a->items[i].id == id) return &a->items[i];
    return NULL;
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
    char *head = short_sha(a->head_sha);
    if (head) str_appendf(&text, "%s%s", text.len ? " \xC2\xB7 " : "", head);
    free(head);
    return str_detach(&text);
}
bool review_approval_can_withdraw(const ReviewApproval *a, long long me) {
    if (a->from_github || a->withdrawn) return false;
    return !me || !a->user_id || a->user_id == me;
}
const char *review_approval_withdraw_consequence(void) { return "It stays on record and no longer counts."; }

static Json *args(const char *repo, int number, const char *note) {
    Json *o = json_object();
    json_set_str(o, "repo", repo); json_set_num(o, "pr", number);
    char *trimmed = str_trim(note ? note : "");
    if (*trimmed) json_set_str(o, "note", trimmed);
    free(trimmed);
    return o;
}
Json *review_approval_approve_args(const char *repo, int number, const char *head_sha, const char *note) {
    Json *o = args(repo, number, note);
    json_set_str(o, "headSha", head_sha);
    return o;
}
Json *review_approval_withdraw_args(const char *repo, int number, long long approval_id, const char *note) {
    Json *o = args(repo, number, note);
    json_set_num(o, "approvalId", (double)approval_id);
    return o;
}
bool review_approval_rereads(const char *code) {
    return str_eq(code, "head_moved") || str_eq(code, "review_not_current") || str_eq(code, "review_running");
}
