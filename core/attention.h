// What the server says waits on you (`attention`): questions, failures, SSH and Slack approvals, and pull requests
// waiting on a person. ◉ Status reads the pull requests whose review passed and whose risk asks for approvals not all
// given, to approve them in place. No UI here.
#ifndef BRIAREUS_ATTENTION_H
#define BRIAREUS_ATTENTION_H
#include "json.h"
#include <stddef.h>

// A pull request an item is about: its project, number, title and the server's summary; `at` is the ISO time it has
// waited since, NULL when not given.
typedef struct { char *id, *repo, *title, *summary, *at; int number; } AttentionPull;
// The `review-approval` items of an `attention` answer, in the server's order. Items tied to a session, or without a
// project and pull request, are left out. Owned; free with attention_pulls_free.
AttentionPull *attention_approval_pulls(const Json *answer, size_t *count);
void attention_pulls_free(AttentionPull *pulls, size_t count);
// The same project and pull request.
bool attention_pull_same(const AttentionPull *a, const AttentionPull *b);
#endif
