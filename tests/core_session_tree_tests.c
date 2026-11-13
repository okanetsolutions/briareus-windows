#include "session_tree.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static Session *list(const char *text, size_t *count) {
    Json *raw = json_parsez(text); Session *items = NULL;
    CHECK(sessions_parse(raw, &items, count)); json_free(raw);
    return items;
}

/// "id:depth:workers" for each row, comma separated. New string.
static char *shape(const SessionNode *rows, size_t count, const Session *sessions) {
    Str s; str_init(&s);
    for (size_t i = 0; i < count; i++) str_appendf(&s, "%s%s:%d:%zu", i ? "," : "", session_id(&sessions[rows[i].index]), rows[i].depth, rows[i].workers);
    str_appendz(&s, "");
    return s.data;
}

static void workers_follow_their_orchestrator_in_list_order(void) {
    size_t n; Session *items = list("[{\"status\":\"idle\",\"id\":\"w2\",\"parentId\":\"o1\"},{\"status\":\"idle\",\"id\":\"a\"},{\"status\":\"idle\",\"id\":\"o1\",\"orchestrator\":true},"
        "{\"status\":\"idle\",\"id\":\"w1\",\"parentId\":\"o1\"},{\"status\":\"idle\",\"id\":\"b\",\"parentId\":\"\"},{\"status\":\"idle\",\"id\":\"w3\",\"parentId\":\"o2\"}]", &n);
    size_t rn; SessionNode *rows = sessions_nested(items, n, &rn);
    // A worker whose orchestrator is not on the list, and an empty parent, stay at the top level.
    CHECK_OWNED_STR(shape(rows, rn, items), "a:0:0,o1:0:2,w2:1:0,w1:1:0,b:0:0,w3:0:0");
    free(rows); sessions_free(items, n);
    rows = sessions_nested(NULL, 0, &rn); CHECK_INT(rn, 0); free(rows);
}

static void nested_orchestrators_and_cycles_keep_every_row_once(void) {
    size_t n; Session *items = list("[{\"status\":\"idle\",\"id\":\"o1\"},{\"status\":\"idle\",\"id\":\"o2\",\"parentId\":\"o1\"},{\"status\":\"idle\",\"id\":\"w\",\"parentId\":\"o2\"},"
        "{\"status\":\"idle\",\"id\":\"x\",\"parentId\":\"y\"},{\"status\":\"idle\",\"id\":\"y\",\"parentId\":\"x\"},{\"status\":\"idle\",\"id\":\"self\",\"parentId\":\"self\"}]", &n);
    size_t rn; SessionNode *rows = sessions_nested(items, n, &rn);
    CHECK_INT(rn, 6);
    CHECK_OWNED_STR(shape(rows, rn, items), "o1:0:1,o2:1:1,w:2:0,self:0:0,x:0:1,y:1:0");
    free(rows); sessions_free(items, n);
}

static const char *FOLDING = "[{\"status\":\"idle\",\"id\":\"o1\"},{\"status\":\"idle\",\"id\":\"o2\",\"parentId\":\"o1\"},{\"status\":\"idle\",\"id\":\"w\",\"parentId\":\"o2\"},{\"status\":\"idle\",\"id\":\"a\"},"
    "{\"status\":\"idle\",\"id\":\"o3\"},{\"status\":\"idle\",\"id\":\"v\",\"parentId\":\"o3\"},{\"status\":\"idle\",\"id\":\"u\",\"parentId\":\"o3\"}]";

static char *folded(const Session *items, size_t n, const char *const *ids, size_t id_count, size_t keep) {
    SessionFolds folds = {0};
    for (size_t i = 0; i < id_count; i++) session_folds_toggle(&folds, ids[i]);
    size_t rn; SessionNode *rows = sessions_nested(items, n, &rn);
    rn = sessions_folded(rows, rn, items, &folds, keep);
    char *out = shape(rows, rn, items);
    free(rows); session_folds_free(&folds);
    return out;
}

static void folding_hides_everything_under_an_orchestrator(void) {
    size_t n; Session *items = list(FOLDING, &n);
    CHECK_OWNED_STR(folded(items, n, NULL, 0, SIZE_MAX), "o1:0:1,o2:1:1,w:2:0,a:0:0,o3:0:2,v:1:0,u:1:0");
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o1" }, 1, SIZE_MAX), "o1:0:1,a:0:0,o3:0:2,v:1:0,u:1:0");
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o2", "o3" }, 2, SIZE_MAX), "o1:0:1,o2:1:1,a:0:0,o3:0:2");
    // Folding a conversation with no workers hides nothing.
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "a" }, 1, SIZE_MAX), "o1:0:1,o2:1:1,w:2:0,a:0:0,o3:0:2,v:1:0,u:1:0");
    sessions_free(items, n);
}

static void folding_keeps_the_open_conversation_and_its_orchestrators(void) {
    size_t n; Session *items = list(FOLDING, &n);
    // The open worker stays under its folded orchestrator; its sibling stays hidden.
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o3" }, 1, 6), "o1:0:1,o2:1:1,w:2:0,a:0:0,o3:0:2,u:1:0");
    // Two levels down, the orchestrator between them stays too.
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o1" }, 1, 2), "o1:0:1,o2:1:1,w:2:0,a:0:0,o3:0:2,v:1:0,u:1:0");
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o1", "o3" }, 2, 1), "o1:0:1,o2:1:1,a:0:0,o3:0:2");
    // An open conversation already on show changes nothing.
    CHECK_OWNED_STR(folded(items, n, (const char *[]){ "o1" }, 1, 3), "o1:0:1,a:0:0,o3:0:2,v:1:0,u:1:0");
    sessions_free(items, n);
}

static void folds_toggle_by_id(void) {
    SessionFolds folds = {0};
    session_folds_toggle(&folds, "o1"); session_folds_toggle(&folds, "o2");
    CHECK(session_folds_contains(&folds, "o1")); CHECK(session_folds_contains(&folds, "o2")); CHECK(!session_folds_contains(&folds, "o3"));
    session_folds_toggle(&folds, "o1");
    CHECK(!session_folds_contains(&folds, "o1")); CHECK(session_folds_contains(&folds, "o2")); CHECK_INT(folds.count, 1);
    session_folds_free(&folds); CHECK_INT(folds.count, 0); CHECK(!session_folds_contains(&folds, "o2"));
}

void core_session_tree_tests(void) {
    test_run("Session tree places workers after their orchestrator in list order", workers_follow_their_orchestrator_in_list_order);
    test_run("Session tree nests orchestrators at any depth and breaks cycles at their first row, each row once", nested_orchestrators_and_cycles_keep_every_row_once);
    test_run("Session tree folding hides everything under a folded orchestrator", folding_hides_everything_under_an_orchestrator);
    test_run("Session tree folding keeps the open conversation and the orchestrators above it", folding_keeps_the_open_conversation_and_its_orchestrators);
    test_run("Session tree folds toggle by conversation id", folds_toggle_by_id);
}
