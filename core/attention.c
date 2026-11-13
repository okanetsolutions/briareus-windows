#include "attention.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *nonempty(const Json *value) { return xstrdup(json_str_nonempty(value)); }
static int pull_number(const Json *value) {
    double n;
    if (!json_num(value, &n) || n < 1 || n > 2147483647.0 || floor(n) != n) return 0;
    return (int)n;
}
// A pull request waiting on a person's approval, which has no session of its own.
static bool approval_item(const Json *item) {
    if (!str_eq(json_str(json_get(item, "kind")), "review-approval")) return false;
    if (json_str_nonempty(json_get(item, "sessionId"))) return false;
    return json_str_nonempty(json_get(item, "repo")) && pull_number(json_get(item, "prNumber"));
}
AttentionPull *attention_approval_pulls(const Json *answer, size_t *count) {
    const Json *items = json_get(answer, "items");
    AttentionPull *out = xcalloc(json_count(items) + 1, sizeof *out);
    size_t n = 0;
    for (size_t i = 0; i < json_count(items); i++) {
        const Json *item = json_at(items, i);
        if (!json_is_object(item) || !approval_item(item)) continue;
        AttentionPull *p = &out[n++];
        p->id = nonempty(json_get(item, "id")); p->repo = xstrdup(json_str(json_get(item, "repo")));
        p->number = pull_number(json_get(item, "prNumber"));
        p->title = nonempty(json_get(item, "title")); p->summary = nonempty(json_get(item, "summary")); p->at = nonempty(json_get(item, "at"));
    }
    *count = n;
    return out;
}
void attention_pulls_free(AttentionPull *pulls, size_t count) {
    if (!pulls) return;
    for (size_t i = 0; i < count; i++) {
        AttentionPull *p = &pulls[i];
        free(p->id); free(p->repo); free(p->title); free(p->summary); free(p->at);
    }
    free(pulls);
}
bool attention_pull_same(const AttentionPull *a, const AttentionPull *b) { return a->number == b->number && str_eq(a->repo, b->repo); }
