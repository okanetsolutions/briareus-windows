#include "pull_attention.h"
#include "models.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static bool kind_of(const char *text, PullAttentionKind *kind) {
    static const char *const names[] = { "review-approval", "review-limit", "review-status" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (str_eq(text, names[i])) { *kind = (PullAttentionKind)i; return true; }
    return false;
}
static bool item_parse(const Json *j, PullAttention *out) {
    memset(out, 0, sizeof *out);
    const char *repo = json_str_nonempty(json_get(j, "repo"));
    int number = json_int_or(json_get(j, "prNumber"), 0);
    if (!kind_of(json_str(json_get(j, "kind")), &out->kind) || !repo || number <= 0) return false;
    out->number = number; out->repo = xstrdup(repo);
    out->id = xstrdup(json_str_or(json_get(j, "id"), ""));
    const char *title = json_str_nonempty(json_get(j, "title"));
    out->title = title ? xstrdup(title) : xstrfmt("Pull request #%d", number);
    out->summary = json_dup_nonempty(json_get(j, "summary")); out->href = json_dup_nonempty(json_get(j, "href")); out->at = json_dup_nonempty(json_get(j, "at"));
    return true;
}
static void item_free(PullAttention *a) { free(a->id); free(a->repo); free(a->title); free(a->summary); free(a->href); free(a->at); }
static DEFINE_LIST_PARSE(PullAttention, items_parse, item_parse)
static DEFINE_LIST_FREE(PullAttention, items_free, item_free)

bool pull_attention_parse(const Json *answer, PullAttention **out, size_t *count) {
    *out = NULL; *count = 0;
    const Json *items = json_get(answer, "items");
    if (!json_is_array(items)) return false;
    *out = items_parse(items, count);
    return true;
}
void pull_attention_free(PullAttention *items, size_t count) { items_free(items, count); }
const char *pull_attention_kind_title(PullAttentionKind kind) {
    switch (kind) {
    case PULL_ATTENTION_APPROVAL: return "Waiting on approval";
    case PULL_ATTENTION_LIMIT: return "Review rounds used up";
    case PULL_ATTENTION_STATUS: return "Commit status not published";
    }
    return "";
}
