// An orchestrator's workers drawn under it in a project's conversation list: each conversation's place, how deep it
// sits and how many workers it holds, and the list with a folded orchestrator's workers left out. No UI here.
#ifndef BRIAREUS_SESSION_TREE_H
#define BRIAREUS_SESSION_TREE_H
#include "models.h"
#include <stdint.h>

/// A conversation's place in the nested list: its index in the session array it was nested from, how deep it sits (0
/// at the top level, one more than its orchestrator's depth under one), and how many rows sit directly under it.
typedef struct { size_t index; int depth; size_t workers; } SessionNode;
/// Workers under the orchestrator their `parentId` names, depth first, each keeping the list's order among its siblings.
/// An orchestrator on the list nests its workers whatever its status, a closed one included; only a worker whose
/// orchestrator is missing from the list stays at the top level. In a cycle, the first member in list order is drawn at
/// the top level and the others nest under it. Every session is drawn once. Owned; release with free().
SessionNode *sessions_nested(const Session *sessions, size_t count, size_t *row_count);

/// Orchestrators whose workers are hidden, by conversation id. Owned; release with session_folds_free.
typedef struct { char **ids; size_t count; } SessionFolds;
bool session_folds_contains(const SessionFolds *folds, const char *id);
void session_folds_toggle(SessionFolds *folds, const char *id);
void session_folds_free(SessionFolds *folds);
/// Compacts nested rows in place, returning how many stay shown, with worker counts as before folding. Everything under
/// a folded orchestrator is left out, except the conversation at session index `keep` (SIZE_MAX for none) and the
/// orchestrators above it: the open conversation is never hidden, while its sibling workers stay folded away.
size_t sessions_folded(SessionNode *rows, size_t count, const Session *sessions, const SessionFolds *folds, size_t keep);
#endif
