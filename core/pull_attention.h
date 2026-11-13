// Pull requests waiting on a person, as `GET /attention` lists them beside session questions: a passed review whose
// risk asks for approvals not yet given (`review-approval`), a review refused at the project's round limit
// (`review-limit`), and a review outcome or approval requirement GitHub would not take as a commit status
// (`review-status`). The Status page lists them under Need you. No UI here.
#ifndef BRIAREUS_PULL_ATTENTION_H
#define BRIAREUS_PULL_ATTENTION_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum { PULL_ATTENTION_APPROVAL, PULL_ATTENTION_LIMIT, PULL_ATTENTION_STATUS } PullAttentionKind;
typedef struct {
    PullAttentionKind kind;
    char *id, *repo, *title, *summary, *href, *at;   // summary, href and at NULL when absent
    int number;
} PullAttention;

// The pull request items of an attention answer (`{ items }`), in the server's order; other kinds and items without a
// repository or pull request number are left out. False, with nothing, for an answer without an item list. Free with
// pull_attention_free.
bool pull_attention_parse(const Json *answer, PullAttention **out, size_t *count);
void pull_attention_free(PullAttention *items, size_t count);
// "Waiting on approval", "Review rounds used up", "Commit status not published"; static.
const char *pull_attention_kind_title(PullAttentionKind kind);
#endif
