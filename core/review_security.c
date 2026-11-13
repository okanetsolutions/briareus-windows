#include "review_security.h"
#include "models.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static int count(const Json *o, const char *key) { return json_int_or(json_get(o, key), 0); }

static bool category_parse(const Json *j, SecurityCategory *out) {
    memset(out, 0, sizeof *out);
    const char *id = json_str_nonempty(json_get(j, "id"));
    if (!id) return false;
    out->id = xstrdup(id); out->assessment = json_dup_nonempty(json_get(j, "assessment"));
    out->applicable = json_bool_is(json_get(j, "applicable"), true);
    out->findings = count(j, "findings");
    return true;
}
static void category_free(SecurityCategory *c) { free(c->id); free(c->assessment); }
static bool entry_parse(const Json *j, SecurityEntry *out) {
    memset(out, 0, sizeof *out);
    const char *title = json_str_nonempty(json_get(j, "title"));
    if (!title) return false;
    out->title = xstrdup(title); out->category = json_dup_nonempty(json_get(j, "category"));
    out->severity = json_dup_nonempty(json_get(j, "severity")); out->file_path = json_dup_nonempty(json_get(j, "filePath"));
    out->start_line = count(j, "startLine"); out->claim = json_dup_nonempty(json_get(j, "claim"));
    return true;
}
static void entry_free(SecurityEntry *e) { free(e->category); free(e->severity); free(e->title); free(e->file_path); free(e->claim); }
static bool claim_parse(const Json *j, SecurityClaim *out) {
    memset(out, 0, sizeof *out);
    const char *ref = json_str_nonempty(json_get(j, "findingRef"));
    if (!ref) return false;
    out->finding_ref = xstrdup(ref); out->status = json_dup_nonempty(json_get(j, "status"));
    out->evidence = json_dup_nonempty(json_get(j, "evidence"));
    return true;
}
static void claim_free(SecurityClaim *c) { free(c->finding_ref); free(c->status); free(c->evidence); }
static DEFINE_LIST_PARSE(SecurityCategory, categories_parse, category_parse)
static DEFINE_LIST_PARSE(SecurityEntry, entries_parse, entry_parse)
static DEFINE_LIST_PARSE(SecurityClaim, claims_parse, claim_parse)

// `widened` in words: a sentence, or a list of what it took in; NULL for neither.
static char *widened_text(const Json *value) {
    if (json_str_nonempty(value)) return xstrdup(json_str(value));
    Str text; str_init(&text);
    for (size_t i = 0; i < json_count(value); i++) {
        const char *part_text = json_str_nonempty(json_at(value, i));
        if (part_text) str_appendf(&text, "%s%s", text.len ? ", " : "", part_text);
    }
    if (text.len) return str_detach(&text);
    str_free(&text);
    return NULL;
}
void review_security_parse(const Json *j, ReviewSecurity *out) {
    memset(out, 0, sizeof *out);
    const char *result = json_str_nonempty(json_get(j, "result"));
    if (!json_is_object(j) || !result) return;
    out->present = true; out->result = xstrdup(result);
    const char *policy = json_str_nonempty(json_get(j, "policy"));
    out->policy = xstrdup(policy ? policy : "off");
    out->blocking = json_bool_is(json_get(j, "blocking"), true);
    out->stale = json_bool_is(json_get(j, "stale"), true);
    out->required_by_risk = json_dup_nonempty(json_get(j, "requiredByRisk"));
    out->stage = json_dup_nonempty(json_get(j, "stage")); out->head_sha = json_dup_nonempty(json_get(j, "headSha"));
    review_reasons_parse(json_get(j, "reasons"), &out->reasons);
    const Json *counts = json_get(j, "counts"), *coverage = json_get(j, "coverage");
    out->counts = (SecurityCounts){count(counts, "findings"), count(counts, "fix"), count(counts, "optional"),
        count(counts, "dismissed"), count(counts, "suppressed"), count(counts, "unverified")};
    out->covered = json_is_object(coverage);
    out->categories = categories_parse(json_get(coverage, "categories"), &out->category_count);
    out->entries = entries_parse(json_get(coverage, "entries"), &out->entry_count);
    out->claims = claims_parse(json_get(coverage, "claims"), &out->claim_count);
    out->recorded = count(coverage, "findings");
    out->widened = widened_text(json_get(coverage, "widened"));
}
void review_security_free(ReviewSecurity *s) {
    if (!s) return;
    free(s->policy); free(s->result); free(s->required_by_risk); free(s->stage); free(s->head_sha); free(s->widened);
    review_reasons_free(&s->reasons);
    for (size_t i = 0; i < s->category_count; i++) category_free(&s->categories[i]);
    for (size_t i = 0; i < s->entry_count; i++) entry_free(&s->entries[i]);
    for (size_t i = 0; i < s->claim_count; i++) claim_free(&s->claims[i]);
    free(s->categories); free(s->entries); free(s->claims);
    memset(s, 0, sizeof *s);
}

char *review_security_risk_note(const ReviewSecurity *s) {
    return s->required_by_risk ? xstrfmt("Required by this change's %s risk tier.", s->required_by_risk) : NULL;
}
static const char *stage_words(const char *stage) {
    if (str_eq(stage, "completed")) return "Pass completed";
    if (str_eq(stage, "running")) return "Pass running";
    if (str_eq(stage, "failed")) return "Pass failed";
    return str_eq(stage, "pending") ? "Pass waiting to run" : NULL;
}
char *review_security_scope_line(const ReviewSecurity *s) {
    const char *stage = stage_words(s->stage);
    if (!stage && !s->head_sha) return NULL;
    Str text; str_init(&text);
    str_appendz(&text, stage ? stage : "No security pass ran");
    if (s->head_sha) str_appendf(&text, " on %.7s", s->head_sha);
    if (s->stale) str_appendz(&text, " \xC2\xB7 out of date");
    return str_detach(&text);
}
static void part(Str *text, int n, const char *words) { if (n > 0) str_appendf(text, " \xC2\xB7 %d %s", n, words); }
char *review_security_findings_line(const ReviewSecurity *s) {
    const SecurityCounts *c = &s->counts;
    if (!c->findings) return s->covered ? xstrdup("No security findings") : NULL;
    Str text; str_init(&text);
    str_appendf(&text, "%d finding%s", c->findings, c->findings == 1 ? "" : "s");
    part(&text, c->fix, "to fix"); part(&text, c->optional, "optional"); part(&text, c->dismissed, "dismissed");
    part(&text, c->suppressed, "suppressed"); part(&text, c->unverified, "unverified");
    return str_detach(&text);
}
char *review_security_claims_line(const ReviewSecurity *s) {
    int fixed = 0, standing = 0;
    for (size_t i = 0; i < s->claim_count; i++) {
        if (str_eq(s->claims[i].status, "fixed")) fixed++;
        else if (str_eq(s->claims[i].status, "stands")) standing++;
    }
    if (!fixed && !standing) return NULL;
    Str text; str_init(&text);
    str_appendz(&text, "Earlier claims:");
    if (fixed) str_appendf(&text, " %d fixed", fixed);
    if (standing) str_appendf(&text, "%s %d still stand%s", fixed ? " \xC2\xB7" : "", standing, standing == 1 ? "s" : "");
    return str_detach(&text);
}
char *review_security_claim_line(const SecurityClaim *c) {
    Str text; str_init(&text);
    str_appendf(&text, "%s: %s", c->finding_ref, c->status ? c->status : "unchecked");
    if (c->evidence) str_appendf(&text, " \xC2\xB7 %s", c->evidence);
    return str_detach(&text);
}
ReviewTone review_security_claim_tone(const SecurityClaim *c) {
    if (str_eq(c->status, "stands")) return REVIEW_TONE_WARN;
    return str_eq(c->status, "fixed") ? REVIEW_TONE_OK : REVIEW_TONE_MUTED;
}
bool review_security_of_review(const Json *review, ReviewSecurity *out) {
    review_security_parse(json_get(review, "security"), out);
    // A policy that ran no pass in this review has nothing of its own to show.
    if (out->present && !(str_eq(out->result, "not_required") && !out->stage)) return true;
    review_security_free(out);
    return false;
}
char *review_security_category_title(const SecurityCategory *c) {
    static const char *const known[][2] = {
        {"permissions", "Permissions"}, {"secrets", "Secrets"}, {"isolation", "Isolation"},
        {"execution", "Execution"}, {"untrusted_input", "Untrusted input"}, {"dependencies", "Dependencies"},
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (str_eq(c->id, known[i][0])) return xstrdup(known[i][1]);
    return str_replace(c->id, "_", " ");
}
char *review_security_category_line(const SecurityCategory *c) {
    if (!c->applicable) return xstrdup("Does not apply");
    if (!c->findings) return xstrdup("Applies");
    return xstrfmt("Applies \xC2\xB7 %d finding%s", c->findings, c->findings == 1 ? "" : "s");
}
ReviewTone review_security_category_tone(const SecurityCategory *c) {
    if (!c->applicable) return REVIEW_TONE_MUTED;
    return c->findings ? REVIEW_TONE_WARN : REVIEW_TONE_OK;
}
char *review_security_entry_location(const SecurityEntry *e) {
    if (!e->file_path) return NULL;
    return e->start_line > 0 ? xstrfmt("%s:%d", e->file_path, e->start_line) : xstrdup(e->file_path);
}

SecurityRank review_security_rank(const ReviewSecurity *s) {
    if (!s || !s->present || str_eq(s->result, "not_required")) return SECURITY_RANK_OFF;
    if (str_eq(s->result, "failed")) return SECURITY_RANK_FAILED;
    if (str_eq(s->result, "incomplete")) return SECURITY_RANK_INCOMPLETE;
    if (str_eq(s->result, "not_run")) return SECURITY_RANK_NOT_RUN;
    return str_eq(s->result, "passed") ? SECURITY_RANK_PASSED : SECURITY_RANK_PENDING;
}
const char *review_security_rank_title(SecurityRank rank) {
    switch (rank) {
    case SECURITY_RANK_FAILED: return "Failed";
    case SECURITY_RANK_INCOMPLETE: return "Incomplete";
    case SECURITY_RANK_NOT_RUN: return "Not run";
    case SECURITY_RANK_PENDING: return "Pending";
    case SECURITY_RANK_PASSED: return "Passed";
    case SECURITY_RANK_OFF: case SECURITY_RANKS: break;
    }
    return "Not asked";
}
char *review_security_summary(const size_t counts[SECURITY_RANKS]) {
    static const char *const words[SECURITY_RANKS] = { "failed", "incomplete", "not run", "pending", "passed", "not asked" };
    Str text; str_init(&text);
    for (int rank = 0; rank < SECURITY_RANKS; rank++) {
        if (counts[rank]) str_appendf(&text, "%s%u %s", text.len ? " \xC2\xB7 " : "", (unsigned)counts[rank], words[rank]);
    }
    if (!text.len) str_appendz(&text, "No open pull requests");
    return str_detach(&text);
}
