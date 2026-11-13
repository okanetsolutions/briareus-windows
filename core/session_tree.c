#include "session_tree.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const size_t *parent; size_t count; SessionNode *rows; size_t row_count; bool *drawn; } Nest;

/// Where the orchestrator a session names sits on the list; SIZE_MAX when it is not there, or is the session itself.
static size_t parent_of(const Session *sessions, size_t count, size_t index) {
    const char *parent = json_str_nonempty(json_get(sessions[index].raw, "parentId"));
    if (!parent) return SIZE_MAX;
    for (size_t i = 0; i < count; i++) if (str_eq(session_id(&sessions[i]), parent)) return i == index ? SIZE_MAX : i;
    return SIZE_MAX;
}

static void draw(Nest *n, size_t index, int depth) {
    if (n->drawn[index]) return;
    n->drawn[index] = true;
    size_t at = n->row_count++;
    n->rows[at].index = index; n->rows[at].depth = depth;
    for (size_t i = 0; i < n->count; i++) {
        if (n->drawn[i] || n->parent[i] != index) continue;
        n->rows[at].workers++;
        draw(n, i, depth + 1);
    }
}

SessionNode *sessions_nested(const Session *sessions, size_t count, size_t *row_count) {
    size_t *parent = xcalloc(count, sizeof *parent);
    for (size_t i = 0; i < count; i++) parent[i] = parent_of(sessions, count, i);
    Nest n = { parent, count, xcalloc(count, sizeof(SessionNode)), 0, xcalloc(count, sizeof(bool)) };
    for (size_t i = 0; i < count; i++) if (parent[i] == SIZE_MAX) draw(&n, i, 0);
    // What a cycle kept from the top level.
    for (size_t i = 0; i < count; i++) draw(&n, i, 0);
    free(n.drawn); free(parent);
    *row_count = n.row_count;
    return n.rows;
}

bool session_folds_contains(const SessionFolds *folds, const char *id) {
    for (size_t i = 0; i < folds->count; i++) if (str_eq(folds->ids[i], id)) return true;
    return false;
}

void session_folds_toggle(SessionFolds *folds, const char *id) {
    for (size_t i = 0; i < folds->count; i++) if (str_eq(folds->ids[i], id)) {
        free(folds->ids[i]);
        memmove(&folds->ids[i], &folds->ids[i + 1], (folds->count - i - 1) * sizeof *folds->ids);
        folds->count--; return;
    }
    folds->ids = xrealloc(folds->ids, (folds->count + 1) * sizeof *folds->ids);
    folds->ids[folds->count++] = xstrdup(id);
}

void session_folds_free(SessionFolds *folds) { str_array_free(folds->ids, folds->count); memset(folds, 0, sizeof *folds); }

/// Marks the row of session `keep` and every row it sits under.
static void mark_kept(const SessionNode *rows, size_t count, size_t keep, bool *kept) {
    size_t at = 0;
    while (at < count && rows[at].index != keep) at++;
    if (at == count) return;
    kept[at] = true;
    int depth = rows[at].depth;
    for (size_t i = at; i-- > 0 && depth > 0;) if (rows[i].depth < depth) { kept[i] = true; depth = rows[i].depth; }
}

size_t sessions_folded(SessionNode *rows, size_t count, const Session *sessions, const SessionFolds *folds, size_t keep) {
    if (!folds->count) return count;
    bool *kept = xcalloc(count, sizeof *kept);
    mark_kept(rows, count, keep, kept);
    size_t out = 0;
    int hiding = -1;
    for (size_t i = 0; i < count; i++) {
        SessionNode row = rows[i];
        if (hiding >= 0 && row.depth > hiding) { if (kept[i]) rows[out++] = row; continue; }
        hiding = row.workers && session_folds_contains(folds, session_id(&sessions[row.index])) ? row.depth : -1;
        rows[out++] = row;
    }
    free(kept);
    return out;
}
