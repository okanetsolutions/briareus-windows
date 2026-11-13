#include "review_outcome.h"
#include "review_approvals.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static const Json *object_or_null(const Json *value) { return json_is_object(value) ? value : NULL; }
ReviewOutcomeSource review_outcome_source_after(const char *call) {
    if (str_eq(call, "update_code_review_finding")) return REVIEW_OUTCOME_FROM_VERDICT;
    if (str_eq(call, "approve_pull") || str_eq(call, "withdraw_pull_approval")) return REVIEW_OUTCOME_FROM_APPROVAL;
    return REVIEW_OUTCOME_FROM_OTHER;
}
// An approval's answer: its outcome replaces the kept one; without one, its requirement replaces the kept outcome's
// approvals; with neither, nothing changes.
static Json *approval_state(const Json *answer, const Json *previous) {
    Json *kept = json_is_object(previous) ? json_clone(previous) : NULL;
    const Json *outcome = object_or_null(json_get(answer, "reviewOutcome")), *approvals = object_or_null(json_get(answer, "approvals"));
    if (outcome) {
        if (!kept) { kept = json_object(); json_object_set(kept, "reviewRounds", json_null()); }
        json_object_set(kept, "reviewOutcome", json_clone(outcome));
    } else if (approvals && json_is_object(json_get(kept, "reviewOutcome"))) {
        Json *merged = json_clone(json_get(kept, "reviewOutcome"));
        json_object_set(merged, "approvals", json_clone(approvals));
        json_object_set(kept, "reviewOutcome", merged);
    }
    return kept;
}
Json *review_outcome_state(const Json *answer, ReviewOutcomeSource source, const Json *previous) {
    if (source == REVIEW_OUTCOME_FROM_OTHER) return previous ? json_clone(previous) : NULL;
    if (source == REVIEW_OUTCOME_FROM_APPROVAL) return approval_state(answer, previous);
    const Json *outcome = object_or_null(json_get(answer, "reviewOutcome"));
    const Json *rounds = object_or_null(json_get(source == REVIEW_OUTCOME_FROM_READ ? answer : previous, "reviewRounds"));
    if (!outcome && !rounds) return NULL;
    Json *kept = json_object();
    json_object_set(kept, "reviewOutcome", outcome ? json_clone(outcome) : json_null());
    json_object_set(kept, "reviewRounds", rounds ? json_clone(rounds) : json_null());
    return kept;
}

static bool outcome_kind(const char *text, ReviewOutcomeKind *kind) {
    static const char *const names[] = { "pending", "incomplete", "changes_required", "passed" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (str_eq(text, names[i])) { *kind = (ReviewOutcomeKind)i; return true; }
    return false;
}
static char *nonempty(const Json *value) { return xstrdup(json_str_nonempty(value)); }
void review_reasons_parse(const Json *list, ReviewReasons *out) {
    out->items = xcalloc(json_count(list) + 1, sizeof *out->items); out->count = 0;
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *r = json_at(list, i);
        const char *code = json_str_nonempty(json_get(r, "code"));
        if (!code) continue;
        ReviewReason *reason = &out->items[out->count++];
        reason->code = xstrdup(code);
        reason->effect = xstrdup(json_str_or(json_get(r, "effect"), ""));
        if (!*reason->effect) { free(reason->effect); reason->effect = xstrdup("note"); }
        const char *message = json_str_nonempty(json_get(r, "message"));
        reason->message = message ? xstrdup(message) : str_replace(code, "_", " ");
    }
}
void review_reasons_free(ReviewReasons *reasons) {
    for (size_t i = 0; i < reasons->count; i++) { free(reasons->items[i].code); free(reasons->items[i].effect); free(reasons->items[i].message); }
    free(reasons->items);
    memset(reasons, 0, sizeof *reasons);
}
void review_security_parse(const Json *j, ReviewSecurity *out) {
    memset(out, 0, sizeof *out);
    const char *result = json_str_nonempty(json_get(j, "result"));
    if (!json_is_object(j) || !result) return;
    out->present = true; out->result = xstrdup(result);
    const char *policy = json_str_nonempty(json_get(j, "policy"));
    out->policy = xstrdup(policy ? policy : "off");
    out->blocking = json_bool_is(json_get(j, "blocking"), true);
    review_reasons_parse(json_get(j, "reasons"), &out->reasons);
}
void review_security_free(ReviewSecurity *s) {
    free(s->policy); free(s->result); review_reasons_free(&s->reasons);
    memset(s, 0, sizeof *s);
}
static void rounds_parse(const Json *j, ReviewRounds *out) {
    double used;
    if (!json_is_object(j) || !json_num(json_get(j, "used"), &used)) return;
    out->present = true; out->used = json_int_or(json_get(j, "used"), 0);
    out->limit = json_int_or(json_get(j, "limit"), 0);
    out->limit_reached = json_bool_is(json_get(j, "limitReached"), true);
    out->continuations = json_int_or(json_get(j, "continuations"), 0);
}
bool review_outcome_parse(const Json *state, ReviewOutcome *out) {
    memset(out, 0, sizeof *out);
    const Json *o = json_get(state, "reviewOutcome"), *status = json_get(o, "status");
    if (!json_is_object(o) || !outcome_kind(json_str(json_get(o, "outcome")), &out->outcome)) return false;
    review_reasons_parse(json_get(o, "reasons"), &out->reasons);
    out->stale = json_bool_is(json_get(o, "stale"), true);
    out->ready = json_bool_is(json_get(o, "ready"), true);
    out->enforced = json_bool_is(json_get(o, "enforced"), true);
    out->published = json_bool_is(json_get(status, "published"), true);
    out->status_error = nonempty(json_get(status, "error"));
    out->status_retried = !json_is_null(json_get(status, "retryAt"));
    review_security_parse(json_get(o, "security"), &out->security);
    review_approvals_parse(json_get(o, "approvals"), &out->approvals);
    rounds_parse(json_get(state, "reviewRounds"), &out->rounds);
    return true;
}
void review_outcome_free(ReviewOutcome *o) {
    if (!o) return;
    review_reasons_free(&o->reasons); free(o->status_error);
    review_security_free(&o->security);
    review_approvals_free(&o->approvals);
    memset(o, 0, sizeof *o);
}

const char *review_outcome_title(ReviewOutcomeKind kind) {
    switch (kind) {
    case REVIEW_OUTCOME_PENDING: return "Pending";
    case REVIEW_OUTCOME_INCOMPLETE: return "Incomplete";
    case REVIEW_OUTCOME_CHANGES_REQUIRED: return "Changes required";
    case REVIEW_OUTCOME_PASSED: return "Passed";
    }
    return "";
}
ReviewTone review_outcome_tone(ReviewOutcomeKind kind) {
    switch (kind) {
    case REVIEW_OUTCOME_PENDING: return REVIEW_TONE_RUNNING;
    case REVIEW_OUTCOME_INCOMPLETE: return REVIEW_TONE_WARN;
    case REVIEW_OUTCOME_CHANGES_REQUIRED: return REVIEW_TONE_DANGER;
    case REVIEW_OUTCOME_PASSED: return REVIEW_TONE_OK;
    }
    return REVIEW_TONE_MUTED;
}
bool review_reason_is_note(const ReviewReason *reason) { return str_eq(reason->effect, "note"); }
ReviewTone review_reason_tone(const ReviewReason *reason) {
    const char *e = reason->effect;
    if (str_eq(e, "changes_required") || str_eq(e, "failed")) return REVIEW_TONE_DANGER;
    if (str_eq(e, "incomplete")) return REVIEW_TONE_WARN;
    return str_eq(e, "pending") ? REVIEW_TONE_RUNNING : REVIEW_TONE_MUTED;
}
void review_reasons_listed(const ReviewReasons *reasons, const ReviewReason **out) {
    size_t n = 0;
    for (size_t i = 0; i < reasons->count; i++) if (!review_reason_is_note(&reasons->items[i])) out[n++] = &reasons->items[i];
    for (size_t i = 0; i < reasons->count; i++) if (review_reason_is_note(&reasons->items[i])) out[n++] = &reasons->items[i];
}
const char *review_outcome_stale_note(const ReviewOutcome *o) {
    if (!o->stale) return NULL;
    // Past the round limit no review of the new revision runs until a person grants one more round.
    if (o->rounds.present && o->rounds.limit_reached)
        return "The pull request moved on from the reviewed revision, and its review rounds are used: no review runs until one more round is granted.";
    return "The pull request moved on from the reviewed revision.";
}
char *review_outcome_status_note(const ReviewOutcome *o) {
    if (!o->enforced || o->published) return NULL;
    if (!o->status_error) return xstrdup("Not yet published to GitHub as the briareus/code-review status.");
    if (o->status_retried) return xstrfmt("Publishing the briareus/code-review status failed and is being retried: %s", o->status_error);
    return xstrfmt("GitHub refused the briareus/code-review status, and it is not retried: %s", o->status_error);
}
const char *review_outcome_ready_note(const ReviewOutcome *o) {
    return o->ready ? "Review passed and the approvals it needs are in." : NULL;
}

bool review_security_shown(const ReviewSecurity *s) { return s->present && !str_eq(s->result, "not_required"); }
char *review_security_line(const ReviewSecurity *s) {
    const char *said = "Pending", *held = "advisory";
    if (str_eq(s->result, "passed")) said = "Passed";
    else if (str_eq(s->result, "failed")) said = "Failed";
    else if (str_eq(s->result, "not_run")) said = "Not run";
    else if (str_eq(s->result, "incomplete")) said = "Incomplete";
    if (str_eq(s->policy, "required") || s->blocking) held = "required";
    else if (str_eq(s->policy, "off")) held = "off";
    return xstrfmt("%s \xC2\xB7 %s", said, held);
}
ReviewTone review_security_tone(const ReviewSecurity *s) {
    if (str_eq(s->result, "passed")) return REVIEW_TONE_OK;
    if (str_eq(s->result, "failed")) return REVIEW_TONE_DANGER;
    if (str_eq(s->result, "incomplete") || str_eq(s->result, "not_run")) return REVIEW_TONE_WARN;
    return REVIEW_TONE_RUNNING;
}
bool review_approvals_shown(const ReviewApprovals *a) {
    return a->present && a->enabled && (!str_eq(a->result, "not_required") || a->count > 0);
}
char *review_approvals_line(const ReviewApprovals *a) {
    char *count = str_eq(a->result, "not_required") ? xstrdup("None needed") : xstrfmt("%d of %d approval%s", a->valid, a->required, a->required == 1 ? "" : "s");
    if (!a->tier) return count;
    char *line = xstrfmt("%s \xC2\xB7 %s risk", count, a->tier);
    free(count);
    return line;
}
ReviewTone review_approvals_tone(const ReviewApprovals *a) {
    if (str_eq(a->result, "satisfied")) return REVIEW_TONE_OK;
    return str_eq(a->result, "not_required") ? REVIEW_TONE_MUTED : REVIEW_TONE_WARN;
}
char *review_rounds_line(const ReviewRounds *r) {
    Str text; str_init(&text);
    if (r->limit > 0) str_appendf(&text, "%d of %d round%s", r->used, r->limit, r->limit == 1 ? "" : "s");
    else str_appendf(&text, "%d round%s", r->used, r->used == 1 ? "" : "s");
    if (r->continuations > 0) str_appendf(&text, " \xC2\xB7 %d granted past the limit", r->continuations);
    return str_detach(&text);
}
ReviewTone review_rounds_tone(const ReviewRounds *r) { return r->limit_reached ? REVIEW_TONE_WARN : REVIEW_TONE_MUTED; }
