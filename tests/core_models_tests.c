#include "suites.h"
#include "test.h"
#include "models.h"
#include <stdlib.h>
#include <string.h>

static char *serialized(Json *j) { char *s = json_serialize(j, true); json_free(j); return s; }

// MARK: - Devices and permissions

static const char *DEVICE = "{\"id\":\"d1\",\"label\":\"Laptop\",\"permission\":\"manage\",\"repos\":[\"o/a\",\"o/b\"],\"expiresAt\":1790610949500}";

static void test_device_reads_every_field(void) {
    Json *j = json_parsez(DEVICE);
    Device d; CHECK(device_parse(j, &d));
    CHECK_STR(d.id, "d1"); CHECK_STR(d.label, "Laptop"); CHECK_STR(d.permission, "manage");
    CHECK_INT(d.repo_count, 2); CHECK_STR(d.repos[0], "o/a"); CHECK_STR(d.repos[1], "o/b");
    CHECK(d.expires_at == 1790610949500.0);
    // Milliseconds become seconds, rounded down.
    CHECK_INT(device_expiry(&d), 1790610949);
    device_free(&d); CHECK(d.id == NULL && d.repos == NULL && d.repo_count == 0);
    device_free(NULL);
    json_free(j);
}
static void test_device_rejects_missing_or_mistyped_fields(void) {
    const char *bad[] = {
        "{\"label\":\"L\",\"permission\":\"read\",\"repos\":[],\"expiresAt\":0}",
        "{\"id\":\"d\",\"permission\":\"read\",\"repos\":[],\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":\"read\",\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":\"read\",\"repos\":[]}",
        "{\"id\":7,\"label\":\"L\",\"permission\":\"read\",\"repos\":[],\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":2,\"repos\":[],\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":\"read\",\"repos\":\"o/a\",\"expiresAt\":0}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":\"read\",\"repos\":[],\"expiresAt\":\"0\"}",
        "{\"id\":\"d\",\"label\":\"L\",\"permission\":\"read\",\"repos\":null,\"expiresAt\":0}",
        "[]", "null", "\"d\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        Json *j = json_parsez(bad[i]); Device d;
        CHECK(!device_parse(j, &d)); CHECK(d.id == NULL && d.repos == NULL);
        json_free(j);
    }
    // Non-string repositories are skipped rather than failing the device.
    Json *j = json_parsez("{\"id\":\"d\",\"label\":\"L\",\"permission\":\"admin\",\"repos\":[\"o/a\",3,null,\"o/b\"],\"expiresAt\":0}");
    Device d; CHECK(device_parse(j, &d)); CHECK_INT(d.repo_count, 2); CHECK_STR(d.repos[1], "o/b"); device_free(&d); json_free(j);
}
static void test_device_round_trips_and_copies_deeply(void) {
    Json *j = json_parsez(DEVICE);
    Device d; CHECK(device_parse(j, &d));
    Json *saved = device_json(&d);
    CHECK(json_equal(saved, j));
    Device again; CHECK(device_parse(saved, &again)); json_free(saved);
    CHECK_STR(again.id, "d1"); CHECK_INT(again.repo_count, 2); CHECK(again.expires_at == d.expires_at);
    device_free(&again);
    Device copy; device_copy(&copy, &d);
    CHECK(copy.id != d.id && copy.repos != d.repos && copy.repos[0] != d.repos[0]);
    device_free(&d);
    CHECK_STR(copy.id, "d1"); CHECK_STR(copy.label, "Laptop"); CHECK_STR(copy.repos[1], "o/b"); CHECK_STR(copy.permission, "manage");
    device_free(&copy); json_free(j);
    // An admin token's empty project list survives.
    j = json_parsez("{\"id\":\"w\",\"label\":\"Web\",\"permission\":\"admin\",\"repos\":[],\"expiresAt\":0}");
    CHECK(device_parse(j, &d)); device_copy(&copy, &d);
    CHECK_INT(copy.repo_count, 0); CHECK_INT(device_expiry(&copy), 0);
    device_free(&copy); device_free(&d); json_free(j);
}
static void test_permission_ranks_and_who_may_manage(void) {
    CHECK_INT(permission_rank("read"), 0); CHECK_INT(permission_rank("manage"), 1); CHECK_INT(permission_rank("admin"), 2);
    CHECK_INT(permission_rank("owner"), -1); CHECK_INT(permission_rank(""), -1); CHECK_INT(permission_rank(NULL), -1);
    CHECK_INT(permission_rank("Admin"), -1); CHECK_INT(permission_rank("READ"), -1);
    const char *perms[] = { "read", "manage", "admin", "owner", "" };
    bool expected[] = { false, true, true, false, false };
    for (int i = 0; i < 5; i++) {
        Device d = { 0 }; d.permission = (char *)perms[i];
        CHECK(device_can_manage(&d) == expected[i]);
    }
    Device none = { 0 };
    CHECK(!device_can_manage(&none)); CHECK(!device_can_manage(NULL));
}
static void test_admin_tokens_manage_since_the_client_api(void) {
    // Before /api/v1 only "manage" could write; an admin token is held to nothing less.
    Json *j = json_parsez("{\"id\":\"w\",\"label\":\"Web\",\"permission\":\"admin\",\"repos\":[],\"expiresAt\":0}");
    Device d; CHECK(device_parse(j, &d)); CHECK(device_can_manage(&d)); device_free(&d); json_free(j);
}

// MARK: - Discovery

static void test_discovery_reads_the_client_record_and_transcription(void) {
    char *text = xstrfmt("{\"version\":1,\"client\":%s,\"transcribe\":false,\"future\":{}}", DEVICE);
    Json *j = json_parsez(text); free(text);
    Discovery d; CHECK(discovery_parse(j, &d));
    CHECK_INT(d.version, 1); CHECK_STR(d.device.id, "d1"); CHECK_INT(d.transcribe, 0);
    discovery_free(&d); discovery_free(NULL); json_free(j);
    // Transcription that is not a boolean reads as unknown.
    text = xstrfmt("{\"version\":3,\"client\":%s,\"transcribe\":\"yes\"}", DEVICE);
    j = json_parsez(text); free(text);
    CHECK(discovery_parse(j, &d)); CHECK_INT(d.version, 3); CHECK_INT(d.transcribe, -1); discovery_free(&d); json_free(j);
}
static void test_discovery_rejects_a_missing_version_or_client(void) {
    const char *shapes[] = {
        "{\"client\":%s}", "{\"version\":\"1\",\"client\":%s}", "{\"version\":-1,\"client\":%s}", "{\"version\":1.5,\"client\":%s}",
        "{\"version\":1,\"device\":%s}", "{\"version\":1,\"client\":null,\"x\":%s}",
    };
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        char *text = xstrfmt(shapes[i], DEVICE); Json *j = json_parsez(text); free(text);
        Discovery d; CHECK(!discovery_parse(j, &d)); discovery_free(&d); json_free(j);
    }
    // Version 0 is still a version.
    char *text = xstrfmt("{\"version\":0,\"client\":%s}", DEVICE); Json *j = json_parsez(text); free(text);
    Discovery d; CHECK(discovery_parse(j, &d)); CHECK_INT(d.version, 0); discovery_free(&d); json_free(j);
}
static void test_voice_notes_off_says_what_to_do(void) {
    CHECK(discovery_voice_notes_off(1) == NULL);
    CHECK_STR(discovery_voice_notes_off(0), "Voice notes are off: the server needs OPENAI_TRANSCRIBE_API_KEY and OPENAI_TRANSCRIBE_MODEL, and a restart once they are set.");
    CHECK_STR(discovery_voice_notes_off(-1), "This server cannot transcribe voice notes yet. Update Briareus on the server to a version whose client API transcribes.");
    // The old wording named the mobile endpoint.
    CHECK(strstr(discovery_voice_notes_off(-1), "mobile") == NULL);
    CHECK_STR(discovery_voice_notes_off(7), discovery_voice_notes_off(-1));
}

// MARK: - Routes

static const char *OPENAPI = "{\"openapi\":\"3.1.0\",\"paths\":{"
    "\"/sessions\":{\"parameters\":[],\"get\":{\"x-briareus-access\":\"read\"},\"post\":{\"x-briareus-access\":\"manage\"},\"head\":{},\"options\":{}},"
    "\"/sessions/{id}\":{\"delete\":{\"x-briareus-access\":\"manage\"},\"put\":{\"x-briareus-access\":\"admin\"},\"patch\":{\"x-briareus-access\":\"manage\"},\"get\":{\"x-briareus-access\":\"read\"}},"
    "\"/pulls/{number}/files\":{\"get\":{\"x-briareus-access\":\"read\"},\"post\":true},"
    "\"/settings\":{\"get\":{}},"
    "\"/odd\":{\"get\":{\"x-briareus-access\":\"owner\"}},"
    "\"/empty\":{},"
    "\"/\":{\"get\":{\"x-briareus-access\":\"read\"}}}}";

static void test_routes_read_each_operation_of_an_openapi_document(void) {
    Json *j = json_parsez(OPENAPI);
    Route *r = NULL; size_t n = 0;
    CHECK(routes_parse(j, &r, &n));
    // Methods come out in a fixed order; head, options, parameters and non-object operations are not routes.
    CHECK_INT(n, 10);
    CHECK_STR(r[0].method, "GET"); CHECK_STR(r[0].path, "/sessions"); CHECK_STR(r[0].access, "read");
    CHECK_STR(r[1].method, "POST"); CHECK_STR(r[1].access, "manage");
    CHECK_STR(r[2].method, "GET"); CHECK_STR(r[2].path, "/sessions/{id}");
    CHECK_STR(r[3].method, "PUT"); CHECK_STR(r[4].method, "PATCH"); CHECK_STR(r[5].method, "DELETE");
    CHECK_STR(r[6].path, "/pulls/{number}/files"); CHECK_STR(r[6].method, "GET");
    // An operation that says nothing about access is the operator's.
    CHECK_STR(r[7].path, "/settings"); CHECK_STR(r[7].access, "admin");
    CHECK_STR(r[8].path, "/odd"); CHECK_STR(r[8].access, "owner");
    CHECK_STR(r[9].path, "/");
    routes_free(r, n); routes_free(NULL, 0); json_free(j);
}
static void test_routes_reject_what_is_neither_openapi_nor_a_saved_list(void) {
    const char *bad[] = { "{\"operations\":[]}", "{\"paths\":[]}", "{\"paths\":null}", "null", "\"paths\"",
                          "[{\"method\":\"GET\",\"path\":\"/\"}]", "[{\"method\":\"GET\",\"access\":\"read\"}]", "[{\"path\":\"/\",\"access\":\"read\"}]",
                          "[{\"method\":\"GET\",\"path\":\"/\",\"access\":\"read\"},{\"method\":1,\"path\":\"/\",\"access\":\"read\"}]", "[1]" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        Json *j = json_parsez(bad[i]); Route *r = NULL; size_t n = 99;
        CHECK(!routes_parse(j, &r, &n)); CHECK(r == NULL);
        json_free(j);
    }
    // Empty documents and lists are fine: a server with nothing to offer.
    Json *j = json_parsez("{\"paths\":{}}"); Route *r; size_t n = 99;
    CHECK(routes_parse(j, &r, &n)); CHECK_INT(n, 0); routes_free(r, n); json_free(j);
    j = json_parsez("[]"); CHECK(routes_parse(j, &r, &n)); CHECK_INT(n, 0); routes_free(r, n); json_free(j);
}
static void test_routes_round_trip_and_copy_deeply(void) {
    Json *j = json_parsez(OPENAPI);
    Route *r; size_t n; CHECK(routes_parse(j, &r, &n)); json_free(j);
    Json *saved = routes_json(r, n);
    CHECK_INT(json_count(saved), n);
    CHECK_STR(json_str(json_get(json_at(saved, 7), "access")), "admin");
    Route *again; size_t m; CHECK(routes_parse(saved, &again, &m)); CHECK_INT(m, n);
    for (size_t i = 0; i < n && i < m; i++) { CHECK_STR(again[i].method, r[i].method); CHECK_STR(again[i].path, r[i].path); CHECK_STR(again[i].access, r[i].access); }
    Json *twice = routes_json(again, m); CHECK(json_equal(saved, twice)); json_free(twice);
    routes_free(again, m); json_free(saved);
    Route *copy = routes_copy(r, n);
    CHECK(copy[0].path != r[0].path);
    routes_free(r, n);
    CHECK_STR(copy[2].path, "/sessions/{id}"); CHECK(routes_allow(copy, n, "GET", "sessions/x", "read"));
    routes_free(copy, n);
    Route *none = routes_copy(NULL, 0); CHECK(none != NULL); routes_free(none, 0);
}
static void test_routes_allow_matches_parameters_on_either_side(void) {
    Json *j = json_parsez(OPENAPI);
    Route *r; size_t n; CHECK(routes_parse(j, &r, &n)); json_free(j);
    // A parameter in the server's path, the app's, or both.
    CHECK(routes_allow(r, n, "GET", "sessions/abc", "read"));
    CHECK(routes_allow(r, n, "GET", "sessions/{sessionId}", "read"));
    CHECK(routes_allow(r, n, "GET", "pulls/12/files", "read"));
    CHECK(routes_allow(r, n, "GET", "pulls/{pr}/files", "read"));
    Route literal = { "GET", "/sessions/archived", "read" };
    CHECK(routes_allow(&literal, 1, "GET", "sessions/{sessionId}", "read"));
    CHECK(!routes_allow(&literal, 1, "GET", "sessions/live", "read"));
    // A parameter stands for exactly one segment.
    CHECK(!routes_allow(r, n, "GET", "sessions/a/b", "admin"));
    CHECK(!routes_allow(r, n, "GET", "pulls/12", "admin"));
    CHECK(!routes_allow(r, n, "GET", "pulls//files", "admin"));
    CHECK(!routes_allow(r, n, "GET", "pulls/12/files/more", "admin"));
    CHECK(!routes_allow(r, n, "GET", "session", "admin"));
    CHECK(!routes_allow(r, n, "GET", "sessionsx", "admin"));
    // Leading and trailing slashes do not count.
    CHECK(routes_allow(r, n, "GET", "/sessions", "read"));
    CHECK(routes_allow(r, n, "GET", "sessions/", "read"));
    CHECK(routes_allow(r, n, "GET", "//sessions//", "read"));
    CHECK(routes_allow(r, n, "GET", "/sessions/abc/", "read"));
    CHECK(routes_allow(r, n, "GET", "", "read")); CHECK(routes_allow(r, n, "GET", "/", "read"));
    // Paths compare case-sensitively; methods do not.
    CHECK(!routes_allow(r, n, "GET", "Sessions", "admin"));
    CHECK(routes_allow(r, n, "get", "sessions", "read")); CHECK(routes_allow(r, n, "Delete", "sessions/x", "manage"));
    routes_free(r, n);
}
static void test_routes_allow_needs_the_method_and_enough_permission(void) {
    Json *j = json_parsez(OPENAPI);
    Route *r; size_t n; CHECK(routes_parse(j, &r, &n)); json_free(j);
    CHECK(!routes_allow(r, n, "PUT", "sessions", "admin"));
    CHECK(!routes_allow(r, n, "DELETE", "sessions", "admin"));
    CHECK(!routes_allow(r, n, "POST", "pulls/1/files", "admin"));
    CHECK(!routes_allow(r, n, "HEAD", "sessions", "admin"));
    CHECK(!routes_allow(r, n, "GET", "empty", "admin"));
    // read < manage < admin
    CHECK(!routes_allow(r, n, "POST", "sessions", "read"));
    CHECK(routes_allow(r, n, "POST", "sessions", "manage")); CHECK(routes_allow(r, n, "POST", "sessions", "admin"));
    CHECK(!routes_allow(r, n, "PUT", "sessions/x", "manage")); CHECK(routes_allow(r, n, "PUT", "sessions/x", "admin"));
    CHECK(!routes_allow(r, n, "GET", "settings", "manage")); CHECK(routes_allow(r, n, "GET", "settings", "admin"));
    // An access level or permission this app does not know allows nothing.
    CHECK(!routes_allow(r, n, "GET", "odd", "admin"));
    CHECK(!routes_allow(r, n, "GET", "sessions", "owner"));
    CHECK(!routes_allow(r, n, "GET", "sessions", NULL));
    CHECK(!routes_allow(NULL, 0, "GET", "sessions", "admin"));
    routes_free(r, n);
}

// MARK: - Connection

static void test_connection_round_trips_with_its_transcription(void) {
    int states[] = { -1, 0, 1 };
    for (int i = 0; i < 3; i++) {
        char *text = xstrfmt("{\"device\":%s,\"routes\":[{\"method\":\"GET\",\"path\":\"/sessions/{id}\",\"access\":\"read\"}]%s}", DEVICE,
                             states[i] < 0 ? "" : states[i] ? ",\"transcribe\":true" : ",\"transcribe\":false");
        Json *j = json_parsez(text); free(text);
        Connection c; CHECK(connection_parse(j, &c));
        CHECK_INT(c.transcribe, states[i]); CHECK_STR(c.device.id, "d1"); CHECK_INT(c.route_count, 1);
        Json *saved = connection_json(&c);
        CHECK(json_equal(saved, j));
        CHECK_INT(json_count(saved), states[i] < 0 ? 2 : 3);
        Connection again; CHECK(connection_parse(saved, &again));
        CHECK_INT(again.transcribe, states[i]); CHECK(routes_allow(again.routes, again.route_count, "GET", "sessions/7", again.device.permission));
        connection_free(&again); json_free(saved);
        connection_free(&c); CHECK(c.routes == NULL && c.device.id == NULL);
        json_free(j);
    }
    connection_free(NULL);
}
static void test_connection_rejects_a_bad_device_or_routes(void) {
    const char *shapes[] = {
        "{\"routes\":[],\"x\":%s}",
        "{\"device\":%s}",
        "{\"device\":%s,\"operations\":[]}",
        "{\"device\":%s,\"routes\":{\"paths\":{}}}",
        "{\"device\":%s,\"routes\":[{\"method\":\"GET\"}]}",
        "{\"device\":%s,\"routes\":null}",
    };
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        char *text = xstrfmt(shapes[i], DEVICE); Json *j = json_parsez(text); free(text);
        Connection c; CHECK(!connection_parse(j, &c)); CHECK(c.device.id == NULL && c.routes == NULL);
        json_free(j);
    }
    Json *j = json_parsez("{\"device\":{\"id\":\"d\"},\"routes\":[]}");
    Connection c; CHECK(!connection_parse(j, &c)); json_free(j);
}

// MARK: - Projects

static void test_projects_read_label_and_title(void) {
    Json *j = json_parsez("{\"projects\":[{\"repo\":\"o/a\",\"label\":\"Alpha\"},{\"repo\":\"o/b\",\"label\":\"\"},{\"repo\":\"o/c\",\"label\":null},{\"label\":\"No repo\"},{\"repo\":5},{\"repo\":\"o/d\",\"label\":3}]}");
    Project *p; size_t n; CHECK(projects_parse(j, &p, &n));
    CHECK_INT(n, 4);
    CHECK_STR(project_title(&p[0]), "Alpha"); CHECK_STR(project_title(&p[1]), "o/b"); CHECK_STR(project_title(&p[2]), "o/c"); CHECK_STR(project_title(&p[3]), "o/d");
    CHECK(p[2].label == NULL && p[3].label == NULL); CHECK_STR(p[1].label, "");
    // A bare array reads the same; neither an object without the list nor a scalar does.
    Json *saved = projects_json(p, n);
    Project *again; size_t m; CHECK(projects_parse(saved, &again, &m)); CHECK_INT(m, 4);
    CHECK_STR(again[0].label, "Alpha"); CHECK(again[2].label == NULL); CHECK_STR(again[3].repo, "o/d");
    Json *twice = projects_json(again, m); CHECK(json_equal(saved, twice)); json_free(twice);
    CHECK(json_is_null(json_get(json_at(saved, 2), "label")) && json_count(json_at(saved, 2)) == 2);
    projects_free(again, m); json_free(saved);
    projects_free(p, n); projects_free(NULL, 0); json_free(j);
    const char *bad[] = { "{}", "{\"projects\":null}", "{\"projects\":{}}", "null", "3" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { j = json_parsez(bad[i]); CHECK(!projects_parse(j, &p, &n)); json_free(j); }
    j = json_parsez("[]"); CHECK(projects_parse(j, &p, &n)); CHECK_INT(n, 0); projects_free(p, n); json_free(j);
}
static void test_project_copy_is_independent(void) {
    Json *j = json_parsez("{\"repo\":\"o/a\",\"label\":\"Alpha\"}");
    Project p; CHECK(project_parse(j, &p));
    Project c; project_copy(&c, &p);
    CHECK(c.repo != p.repo && c.label != p.label);
    project_free(&p); CHECK(p.repo == NULL);
    CHECK_STR(c.repo, "o/a"); CHECK_STR(c.label, "Alpha"); project_free(&c); json_free(j);
    j = json_parsez("{\"repo\":\"o/b\"}");
    CHECK(project_parse(j, &p)); project_copy(&c, &p); CHECK(c.label == NULL); CHECK_STR(project_title(&c), "o/b");
    Json *one = project_json(&c); CHECK_STR(json_str(json_get(one, "repo")), "o/b"); json_free(one);
    project_free(&c); project_free(&p); project_free(NULL); json_free(j);
    j = json_parsez("{\"label\":\"x\"}"); CHECK(!project_parse(j, &p)); CHECK(p.repo == NULL); json_free(j);
}

// MARK: - Sessions

static Session session_of(const char *text) {
    Json *j = json_parsez(text); Session s = { 0 };
    CHECK(j != NULL); CHECK(session_parse(j, &s)); json_free(j);
    return s;
}
static void test_session_needs_an_id_and_a_status(void) {
    const char *bad[] = { "{\"status\":\"idle\"}", "{\"id\":\"s\"}", "{\"id\":1,\"status\":\"idle\"}", "{\"id\":\"s\",\"status\":null}", "[]", "null" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { Json *j = json_parsez(bad[i]); Session s; CHECK(!session_parse(j, &s)); CHECK(s.raw == NULL); json_free(j); }
    Json *j = json_parsez("{\"sessions\":[{\"id\":\"a\",\"status\":\"idle\"},{\"id\":\"b\"},{\"id\":\"c\",\"status\":\"running\",\"future\":[1]}]}");
    Session *s; size_t n; CHECK(sessions_parse(j, &s, &n)); CHECK_INT(n, 2);
    CHECK_STR(session_id(&s[0]), "a"); CHECK_STR(session_id(&s[1]), "c");
    // Unknown fields ride along, so a saved list round-trips.
    Json *saved = sessions_json(s, n);
    CHECK(json_equal(json_at(saved, 1), json_at(json_get(j, "sessions"), 2)));
    Session *again; size_t m; CHECK(sessions_parse(saved, &again, &m)); CHECK_INT(m, 2); CHECK_STR(session_status(&again[1]), "running");
    sessions_free(again, m); json_free(saved); sessions_free(s, n); sessions_free(NULL, 0); json_free(j);
    const char *lists[] = { "{}", "{\"sessions\":null}", "\"x\"" };
    for (size_t i = 0; i < 3; i++) { j = json_parsez(lists[i]); CHECK(!sessions_parse(j, &s, &n)); json_free(j); }
}
static void test_session_copy_is_independent(void) {
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"title\":\"Fix it\"}");
    Session c; session_copy(&c, &s);
    CHECK(c.raw != s.raw);
    session_free(&s); CHECK(s.raw == NULL); session_free(&s); session_free(NULL);
    CHECK_STR(session_display_title(&c), "Fix it"); CHECK_STR(session_id(&c), "a");
    session_free(&c);
}
static void test_session_accessors_fall_back_when_fields_are_missing(void) {
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"repo\":\"o/r\",\"model\":\"opus\",\"provider\":\"claude\",\"title\":\"Fix login\"}");
    CHECK_STR(session_repo(&s), "o/r"); CHECK_STR(session_model(&s), "opus"); CHECK_STR(session_provider(&s), "claude");
    CHECK_STR(session_display_title(&s), "Fix login");
    session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"repo\":null,\"model\":\"\",\"provider\":2,\"title\":\"\"}");
    CHECK(session_repo(&s) == NULL); CHECK(session_model(&s) == NULL); CHECK(session_provider(&s) == NULL);
    CHECK_STR(session_display_title(&s), "New conversation");
    session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"title\":7}");
    CHECK_STR(session_display_title(&s), "New conversation"); session_free(&s);
    // A session never parsed reads as empty rather than crashing.
    Session empty = { 0 };
    CHECK_STR(session_id(&empty), ""); CHECK_STR(session_status(&empty), ""); CHECK(session_repo(&empty) == NULL);
    CHECK_STR(session_display_title(&empty), "New conversation"); CHECK(!session_is_active(&empty));
}
static void test_session_is_active_by_status(void) {
    const char *active[] = { "queued", "preparing", "running", "starting" };
    const char *inactive[] = { "idle", "closed", "failed", "done", "Running", "", "future" };
    for (size_t i = 0; i < 4; i++) { char *t = xstrfmt("{\"id\":\"a\",\"status\":\"%s\"}", active[i]); Session s = session_of(t); free(t); CHECK(session_is_active(&s)); session_free(&s); }
    for (size_t i = 0; i < 7; i++) { char *t = xstrfmt("{\"id\":\"a\",\"status\":\"%s\"}", inactive[i]); Session s = session_of(t); free(t); CHECK(!session_is_active(&s)); session_free(&s); }
}
static void test_session_live_input_and_queue(void) {
    Session s = session_of("{\"id\":\"a\",\"status\":\"running\",\"liveInput\":true,\"queued\":[{\"text\":\"next\"}]}");
    CHECK(session_live_input(&s)); CHECK_INT(json_count(session_queued(&s)), 1);
    session_free(&s);
    const char *off[] = { "{\"id\":\"a\",\"status\":\"running\"}", "{\"id\":\"a\",\"status\":\"running\",\"liveInput\":false}", "{\"id\":\"a\",\"status\":\"running\",\"liveInput\":1}", "{\"id\":\"a\",\"status\":\"running\",\"liveInput\":\"true\"}" };
    for (size_t i = 0; i < 4; i++) { s = session_of(off[i]); CHECK(!session_live_input(&s)); CHECK(json_is_null(session_queued(&s))); session_free(&s); }
}
static void test_session_review_loop_flags(void) {
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"reviewLoop\":{\"on\":true}}");
    CHECK(session_review_loop_on(&s)); CHECK(session_can_review_loop(&s)); session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\"}"); CHECK(!session_review_loop_on(&s)); CHECK(session_can_review_loop(&s)); session_free(&s);
    // Every flag that marks a session as not started from scratch on a task rules the loop out; an unset value does not.
    const char *flags[] = { "\"reviewBranch\":\"f\"", "\"qaBranch\":\"q\"", "\"autoClose\":true", "\"loopParentId\":\"p\"", "\"local\":true", "\"orchestrator\":1" };
    const char *unset[] = { "\"reviewBranch\":\"\"", "\"qaBranch\":null", "\"autoClose\":false", "\"loopParentId\":null", "\"local\":false", "\"orchestrator\":0" };
    for (size_t i = 0; i < 6; i++) {
        char *t = xstrfmt("{\"id\":\"a\",\"status\":\"idle\",%s}", flags[i]); s = session_of(t); free(t);
        CHECK(!session_can_review_loop(&s)); session_free(&s);
        t = xstrfmt("{\"id\":\"a\",\"status\":\"idle\",%s}", unset[i]); s = session_of(t); free(t);
        CHECK(session_can_review_loop(&s)); session_free(&s);
    }
    s = session_of("{\"id\":\"a\",\"status\":\"closed\"}"); CHECK(!session_can_review_loop(&s)); session_free(&s);
}
static void test_held_triage_needs_findings_and_prefers_the_standalone_review(void) {
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"reviewTriage\":{\"findings\":[{\"key\":\"own\"}]},\"reviewLoop\":{\"triage\":{\"findings\":[{\"key\":\"loop\"}]}}}");
    CHECK_STR(json_str(json_get(json_at(json_get(session_held_triage(&s), "findings"), 0), "key")), "own");
    // The Findings screen's round prefers the loop's.
    CHECK_STR(json_str(json_get(json_at(json_get(session_held_round(&s), "findings"), 0), "key")), "loop");
    session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"reviewTriage\":{\"findings\":[]},\"reviewLoop\":{\"triage\":{\"findings\":[{\"key\":\"loop\"}]}}}");
    CHECK_STR(json_str(json_get(json_at(json_get(session_held_triage(&s), "findings"), 0), "key")), "loop");
    session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"reviewTriage\":{},\"reviewLoop\":{\"triage\":{\"findings\":[]}}}");
    CHECK(session_held_triage(&s) == NULL); CHECK(session_held_round(&s) != NULL); session_free(&s);
}
static void test_session_pull_number_ignores_what_is_not_a_pull_request(void) {
    const char *cases[] = { "{\"id\":\"s\",\"status\":\"idle\",\"prStatus\":{\"number\":-3}}",
                            "{\"id\":\"s\",\"status\":\"idle\",\"startedOnPr\":\"4\"}",
                            "{\"id\":\"s\",\"status\":\"idle\",\"prStatus\":{\"number\":\"9\"},\"startedOnPr\":4}",
                            "{\"id\":\"s\",\"status\":\"idle\",\"prStatus\":{},\"startedOnPr\":11}" };
    int expected[] = { 0, 0, 4, 11 };
    for (int i = 0; i < 4; i++) { Session s = session_of(cases[i]); CHECK_INT(session_pull_number(&s), expected[i]); session_free(&s); }
}

// MARK: - Findings

static void test_held_rounds_of_an_empty_list(void) {
    size_t n = 99; HeldRound *r = sessions_held_rounds(NULL, 0, &n);
    CHECK(r != NULL); CHECK_INT(n, 0); free(r);
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"reviewTriage\":\"held\",\"reviewLoop\":{\"triage\":[]}}");
    CHECK(session_held_round(&s) == NULL);
    r = sessions_held_rounds(&s, 1, &n); CHECK_INT(n, 0); free(r); session_free(&s);
}
static void test_held_rounds_with_equal_holds_keep_the_list_order(void) {
    Json *j = json_parsez("[{\"id\":\"a\",\"status\":\"idle\",\"reviewTriage\":{\"heldAt\":\"2026-09-29T10:00:00Z\"}},"
                          "{\"id\":\"b\",\"status\":\"idle\"},"
                          "{\"id\":\"c\",\"status\":\"idle\",\"reviewTriage\":{\"heldAt\":\"2026-09-29T09:00:00Z\"}},"
                          "{\"id\":\"d\",\"status\":\"idle\",\"reviewTriage\":{\"heldAt\":\"2026-09-29T10:00:00Z\"}},"
                          "{\"id\":\"e\",\"status\":\"idle\",\"reviewTriage\":{\"heldAt\":\"2026-09-29T09:00:00Z\"}}]");
    Session *s; size_t n; CHECK(sessions_parse(j, &s, &n));
    size_t count; HeldRound *r = sessions_held_rounds(s, n, &count);
    CHECK_INT(count, 4);
    CHECK_INT(r[0].index, 2); CHECK_INT(r[1].index, 4); CHECK_INT(r[2].index, 0); CHECK_INT(r[3].index, 3);
    CHECK(r[0].held == session_held_round(&s[2]));
    free(r); sessions_free(s, n); json_free(j);
}
static void test_held_round_url_reuses_the_conversations_own_link_only_for_its_pull_request(void) {
    Json *held7 = json_parsez("{\"prNumber\":7}"), *held9 = json_parsez("{\"prNumber\":9}"), *none = json_parsez("{}");
    Session s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"repo\":\"o/r\",\"prStatus\":{\"number\":7,\"url\":\"https://ghe.example/o/r/pull/7\"}}");
    CHECK_OWNED_STR(held_round_pr_url(&s, held7), "https://ghe.example/o/r/pull/7");
    CHECK_OWNED_STR(held_round_pr_url(&s, held9), "https://github.com/o/r/pull/9");
    session_free(&s);
    // An empty link or one without a number is built from the round's number.
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"repo\":\"o/r\",\"prStatus\":{\"number\":7,\"url\":\"\"}}");
    CHECK_OWNED_STR(held_round_pr_url(&s, held7), "https://github.com/o/r/pull/7"); session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\",\"repo\":\"o/r\",\"prStatus\":{\"url\":\"https://ghe.example/x\"}}");
    CHECK_OWNED_STR(held_round_pr_url(&s, held7), "https://github.com/o/r/pull/7"); session_free(&s);
    s = session_of("{\"id\":\"a\",\"status\":\"idle\"}");
    CHECK_OWNED_STR(held_round_pr_url(&s, held9), "https://github.com//pull/9"); session_free(&s);
    CHECK_INT(held_round_pr_number(held7), 7); CHECK_INT(held_round_pr_number(none), 0); CHECK_INT(held_round_pr_number(NULL), 0);
    Json *odd = json_parsez("{\"prNumber\":0}"); CHECK_INT(held_round_pr_number(odd), 0); json_free(odd);
    odd = json_parsez("{\"prNumber\":\"7\"}"); CHECK_INT(held_round_pr_number(odd), 0); json_free(odd);
    json_free(held7); json_free(held9); json_free(none);
}
static void test_held_round_is_mine_unless_a_standalone_review_says_otherwise(void) {
    const char *mine[] = { "{}", "{\"mine\":false}", "{\"standalone\":false,\"mine\":false}", "{\"standalone\":null}", "{\"standalone\":true,\"mine\":true}" };
    const char *theirs[] = { "{\"standalone\":true}", "{\"standalone\":true,\"mine\":false}", "{\"standalone\":true,\"mine\":\"yes\"}", "{\"standalone\":1,\"mine\":1}" };
    for (size_t i = 0; i < 5; i++) { Json *j = json_parsez(mine[i]); CHECK(held_round_is_mine(j)); json_free(j); }
    for (size_t i = 0; i < 4; i++) { Json *j = json_parsez(theirs[i]); CHECK(!held_round_is_mine(j)); json_free(j); }
}
static void test_triage_outcome_text_covers_every_outcome(void) {
    struct { const char *outcome, *text; bool danger; } cases[] = {
        { "{\"completed\":true,\"prNumber\":12}", "Review completed; what it found stays on PR #12 for its author.", false },
        { "{\"completed\":true}", "Review completed; what it found stays on the pull request for its author.", false },
        { "{\"completed\":true,\"prNumber\":\"12\"}", "Review completed; what it found stays on the pull request for its author.", false },
        { "{\"converged\":true,\"approved\":true}", "Verdicts recorded; nothing was left to fix, so code-approved was added and the loop converged.", false },
        { "{\"approved\":true,\"fixing\":true}", "Verdicts recorded; nothing was left to fix, so code-approved was added.", false },
        { "{\"fixing\":\"session-id\"}", "Verdicts recorded; a fix session is running.", false },
        { "{\"reviewing\":true,\"deferred\":true}", "Verdicts recorded; nothing was left to fix, but the branch had moved, so the new commits are being reviewed.", false },
        { "{\"deferred\":true}", "Verdicts recorded; nothing was left to fix, but the branch had moved. The new commits are reviewed once the session settles idle.", false },
        { "{\"completed\":false,\"converged\":null,\"fixing\":\"\",\"deferred\":0}", "Verdicts recorded, but no fix session started. The session\xE2\x80\x99s log says why.", true },
        { "{}", "Verdicts recorded, but no fix session started. The session\xE2\x80\x99s log says why.", true },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *j = json_parsez(cases[i].outcome); bool danger = !cases[i].danger;
        CHECK_OWNED_STR(triage_outcome_text(j, &danger), cases[i].text); CHECK(danger == cases[i].danger);
        json_free(j);
    }
    bool danger = false;
    CHECK_OWNED_STR(triage_outcome_text(NULL, &danger), "Verdicts recorded, but no fix session started. The session\xE2\x80\x99s log says why."); CHECK(danger);
}
static void test_findings_subtitle_counts_reviews_and_pull_requests(void) {
    CHECK_OWNED_STR(findings_subtitle(0, 0), "nothing is waiting");
    CHECK_OWNED_STR(findings_subtitle(0, 3), "nothing is waiting");
    CHECK_OWNED_STR(findings_subtitle(1, 1), "1 review waiting for a decision");
    CHECK_OWNED_STR(findings_subtitle(2, 2), "2 reviews waiting for a decision");
    CHECK_OWNED_STR(findings_subtitle(2, 1), "2 reviews on 1 pull request waiting for a decision");
    CHECK_OWNED_STR(findings_subtitle(5, 3), "5 reviews on 3 pull requests waiting for a decision");
}

// MARK: - Events and transcript

static Event event_of(const char *text) {
    Json *j = json_parsez(text); Event e;
    CHECK(j != NULL); CHECK(event_parse(j, &e)); json_free(j);
    return e;
}
static void test_event_reads_every_field(void) {
    Event e = event_of("{\"seq\":4,\"kind\":\"result\",\"t\":\"2026-09-28T15:55:49.120Z\",\"text\":\"Done\",\"name\":\"Bash\",\"summary\":\"ls\",\"question\":\"Go?\","
                       "\"options\":[\"Yes\",\"No\"],\"attachments\":[{\"id\":\"u1\"}],\"costUsd\":0.25,\"durationMs\":1200,\"isError\":false}");
    CHECK_INT(e.seq, 4); CHECK_STR(e.kind, "result"); CHECK_STR(e.text, "Done"); CHECK_STR(e.name, "Bash"); CHECK_STR(e.summary, "ls"); CHECK_STR(e.question, "Go?");
    CHECK_INT(json_count(e.options), 2); CHECK_INT(json_count(e.attachments), 1);
    CHECK(e.has_cost && e.cost_usd == 0.25); CHECK(e.has_duration && e.duration_ms == 1200); CHECK_INT(e.is_error, 0);
    time_t t; CHECK(event_time(&e, &t)); CHECK_INT(t, 1790610949);
    event_free(&e); CHECK(e.raw == NULL && e.kind == NULL); event_free(NULL);
    e = event_of("{\"seq\":5,\"kind\":\"tool_error\",\"options\":{},\"attachments\":\"x\",\"costUsd\":null,\"isError\":true,\"t\":\"yesterday\"}");
    CHECK(e.options == NULL && e.attachments == NULL); CHECK(!e.has_cost && !e.has_duration); CHECK_INT(e.is_error, 1);
    CHECK(e.text == NULL && e.name == NULL);
    CHECK(!event_time(&e, &t)); event_free(&e);
    e = event_of("{\"seq\":6,\"kind\":\"text\"}"); CHECK_INT(e.is_error, -1); CHECK(!event_time(&e, &t)); event_free(&e);
}
static void test_event_needs_a_kind_and_a_sequence(void) {
    const char *bad[] = { "{\"kind\":\"text\"}", "{\"seq\":1}", "{\"seq\":\"1\",\"kind\":\"text\"}", "{\"seq\":1,\"kind\":3}", "[]", "null" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { Json *j = json_parsez(bad[i]); Event e; CHECK(!event_parse(j, &e)); CHECK(e.raw == NULL); json_free(j); }
}
static void test_event_detail_prefers_text_over_summary(void) {
    Event e = event_of("{\"seq\":1,\"kind\":\"tool\",\"summary\":\"git status\"}"); CHECK_STR(event_detail(&e), "git status"); event_free(&e);
    e = event_of("{\"seq\":1,\"kind\":\"text\",\"text\":\"Hi\",\"summary\":\"ignored\"}"); CHECK_STR(event_detail(&e), "Hi"); event_free(&e);
    e = event_of("{\"seq\":1,\"kind\":\"text\",\"text\":\"\",\"summary\":\"s\"}"); CHECK_STR(event_detail(&e), ""); event_free(&e);
    e = event_of("{\"seq\":1,\"kind\":\"cmd\"}"); CHECK(event_detail(&e) == NULL); event_free(&e);
}
static void test_event_visible_by_kind_and_content(void) {
    struct { const char *event; bool visible; } cases[] = {
        { "{\"seq\":1,\"kind\":\"status\",\"text\":\"running\"}", false },
        { "{\"seq\":1,\"kind\":\"status\"}", false },
        { "{\"seq\":1,\"kind\":\"tool\"}", true },
        { "{\"seq\":1,\"kind\":\"tool_error\"}", true },
        { "{\"seq\":1,\"kind\":\"result\"}", true },
        { "{\"seq\":1,\"kind\":\"ask\",\"question\":\"Which?\"}", true },
        { "{\"seq\":1,\"kind\":\"future\",\"text\":\"shown\"}", true },
        { "{\"seq\":1,\"kind\":\"future\"}", false },
        { "{\"seq\":1,\"kind\":\"git\",\"summary\":\"only a summary\"}", false },
        { "{\"seq\":1,\"kind\":\"setup\",\"text\":\"npm ERR!\"}", true },
        { "{\"seq\":1,\"kind\":\"cmd\",\"text\":\"$ php artisan migrate:fresh --seed\"}", true },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) { Event e = event_of(cases[i].event); CHECK(event_visible(&e) == cases[i].visible); event_free(&e); }
}
static void test_setup_output_stays_in_the_transcript(void) {
    // Hiding setup lines hid why a session failed during workspace setup.
    Json *events = json_parsez("[{\"seq\":1,\"kind\":\"cmd\",\"text\":\"$ php artisan migrate:fresh --seed\"},{\"seq\":2,\"kind\":\"setup\",\"text\":\"SQLSTATE[HY000] [2002] Connection refused\"},{\"seq\":3,\"kind\":\"status\",\"status\":\"failed\"}]");
    Transcript t; transcript_init(&t); transcript_append(&t, events);
    CHECK_INT(t.count, 3);
    CHECK(event_visible(&t.events[1])); CHECK_STR(event_detail(&t.events[1]), "SQLSTATE[HY000] [2002] Connection refused");
    CHECK(!event_visible(&t.events[2]));
    transcript_free(&t); json_free(events);
}
static void test_transcript_cursor_only_moves_forward(void) {
    Transcript t; transcript_init(&t);
    CHECK(t.count == 0 && t.cursor == 0 && t.events == NULL);
    Json *later = json_parsez("[{\"seq\":10,\"kind\":\"text\",\"text\":\"b\"},{\"seq\":12,\"kind\":\"text\",\"text\":\"c\"}]");
    Json *earlier = json_parsez("[{\"seq\":4,\"kind\":\"text\",\"text\":\"a\"},{\"seq\":10,\"kind\":\"text\",\"text\":\"dup\"}]");
    transcript_append(&t, later); CHECK_INT(t.cursor, 12);
    transcript_append(&t, earlier); CHECK_INT(t.cursor, 12); CHECK_INT(t.count, 3);
    CHECK_INT(t.events[0].seq, 4); CHECK_INT(t.events[1].seq, 10); CHECK_INT(t.events[2].seq, 12);
    // The first copy of a sequence wins.
    CHECK_STR(t.events[1].text, "b");
    // Unreadable entries and non-arrays are skipped.
    Json *junk = json_parsez("[{\"kind\":\"text\"},{\"seq\":\"13\",\"kind\":\"text\"},7,null]");
    transcript_append(&t, junk); CHECK_INT(t.count, 3); CHECK_INT(t.cursor, 12);
    transcript_append(&t, NULL); CHECK_INT(t.count, 3);
    Json *obj = json_parsez("{\"seq\":20,\"kind\":\"text\"}"); transcript_append(&t, obj); CHECK_INT(t.count, 3); json_free(obj);
    json_free(junk);
    CHECK_STR(t.events[0].text, "a");
    transcript_free(&t); CHECK(t.count == 0 && t.cursor == 0 && t.events == NULL); transcript_free(NULL);
    json_free(later); json_free(earlier);
}
static void test_transcript_grows_past_its_first_allocation(void) {
    Transcript t; transcript_init(&t);
    Json *events = json_array();
    for (int i = 200; i >= 1; i--) { Json *e = json_object(); json_set_num(e, "seq", i); json_set_str(e, "kind", "text"); json_set_str(e, "text", "x"); json_array_push(events, e); }
    transcript_append(&t, events);
    CHECK_INT(t.count, 200); CHECK_INT(t.cursor, 200);
    bool sorted = true; for (size_t i = 0; i < t.count; i++) if (t.events[i].seq != (int)i + 1) sorted = false;
    CHECK(sorted);
    transcript_append(&t, events); CHECK_INT(t.count, 200);
    transcript_free(&t); json_free(events);
}
static void test_saved_transcript_restores_events_and_cursor(void) {
    Json *events = json_parsez("[{\"seq\":2,\"kind\":\"text\",\"text\":\"b\",\"extra\":{\"k\":1}},{\"seq\":1,\"kind\":\"user\",\"text\":\"a\"}]");
    Transcript t; transcript_init(&t); transcript_append(&t, events);
    Json *saved = transcript_json(&t);
    CHECK_INT(json_count(saved), 2); CHECK_INT(json_num_or(json_get(json_at(saved, 0), "seq"), 0), 1);
    CHECK(json_equal(json_at(saved, 1), json_at(events, 0)));
    Transcript again; transcript_init(&again); transcript_append(&again, saved);
    CHECK_INT(again.count, 2); CHECK_INT(again.cursor, 2);
    Json *twice = transcript_json(&again); CHECK(json_equal(saved, twice)); json_free(twice);
    transcript_free(&again); json_free(saved); transcript_free(&t);
    transcript_init(&t); saved = transcript_json(&t); CHECK_INT(json_count(saved), 0); CHECK(json_is_array(saved)); json_free(saved);
    json_free(events);
}

// MARK: - Runtimes

static const char *CATALOG = "{\"default\":{\"providerId\":2,\"model\":\"opus\",\"effort\":\"high\"},\"providers\":["
    "{\"id\":1,\"label\":\"Codex\",\"available\":false,\"defaultModel\":\"gpt\",\"models\":[{\"id\":\"gpt\",\"label\":\"GPT\",\"efforts\":[\"low\"],\"defaultEffort\":\"low\"}]},"
    "{\"id\":2,\"label\":\"Claude\",\"available\":true,\"defaultModel\":\"opus\",\"models\":["
        "{\"id\":\"sonnet\",\"label\":\"\",\"efforts\":[\"low\",\"medium\"]},"
        "{\"id\":\"opus\",\"label\":\"Opus\",\"efforts\":[\"low\",\"high\"],\"defaultEffort\":\"high\"},"
        "{\"id\":\"haiku\"}]},"
    "{\"id\":3,\"label\":\"Old\",\"models\":[]},"
    "{\"id\":4,\"label\":\"\",\"defaultModel\":\"missing\",\"models\":[{\"id\":\"m\",\"efforts\":[]}]}]}";

static void test_runtime_catalog_reads_providers_and_models(void) {
    Json *j = json_parsez(CATALOG);
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    CHECK_INT(c.provider_count, 4);
    CHECK_INT(c.providers[0].available, 0); CHECK_INT(c.providers[1].available, 1); CHECK_INT(c.providers[2].available, -1);
    CHECK(!runtime_provider_available(&c.providers[0])); CHECK(runtime_provider_available(&c.providers[1])); CHECK(runtime_provider_available(&c.providers[2]));
    const RuntimeProvider *claude = runtime_catalog_provider(&c, 2);
    CHECK(claude == &c.providers[1]); CHECK(runtime_catalog_provider(&c, 9) == NULL);
    CHECK_INT(claude->model_count, 3); CHECK_STR(claude->default_model, "opus");
    // A model without a label goes by its id.
    CHECK_STR(runtime_model_title(&claude->models[0]), "sonnet"); CHECK_STR(runtime_model_title(&claude->models[1]), "Opus"); CHECK_STR(runtime_model_title(&claude->models[2]), "haiku");
    CHECK(claude->models[2].efforts == NULL && claude->models[2].effort_count == 0);
    CHECK(c.providers[3].models[0].efforts != NULL && c.providers[3].models[0].effort_count == 0);
    runtime_catalog_free(&c); CHECK(c.providers == NULL && c.provider_count == 0); runtime_catalog_free(NULL);
    json_free(j);
}
static void test_runtime_catalog_rejects_malformed_providers(void) {
    const char *bad[] = {
        "{}", "{\"providers\":{}}", "[]",
        "{\"providers\":[{\"label\":\"X\",\"models\":[]}]}",
        "{\"providers\":[{\"id\":\"1\",\"label\":\"X\",\"models\":[]}]}",
        "{\"providers\":[{\"id\":1,\"models\":[]}]}",
        "{\"providers\":[{\"id\":1,\"label\":\"X\"}]}",
        "{\"providers\":[{\"id\":1,\"label\":\"X\",\"models\":[{\"label\":\"no id\"}]}]}",
        "{\"providers\":[{\"id\":1,\"label\":\"X\",\"models\":[{\"id\":\"ok\"}]},{\"id\":2,\"label\":\"Y\",\"models\":[{\"id\":\"ok\"},{\"id\":5}]}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        Json *j = json_parsez(bad[i]); RuntimeCatalog c;
        CHECK(!runtime_catalog_parse(j, &c)); CHECK(c.providers == NULL && c.provider_count == 0);
        json_free(j);
    }
    // A default without a provider is no default.
    Json *j = json_parsez("{\"default\":{\"model\":\"opus\"},\"providers\":[]}");
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c)); CHECK(!c.has_default); CHECK(c.def.model == NULL); runtime_catalog_free(&c); json_free(j);
}
static void test_runtime_catalog_round_trips(void) {
    Json *j = json_parsez(CATALOG);
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    Json *saved = runtime_catalog_json(&c);
    CHECK(json_is_null(json_get(json_at(json_get(saved, "providers"), 2), "available")));
    CHECK_INT(json_count(json_at(json_get(saved, "providers"), 2)), 4);
    RuntimeCatalog again; CHECK(runtime_catalog_parse(saved, &again));
    char *a = serialized(runtime_catalog_json(&c)), *b = serialized(runtime_catalog_json(&again));
    CHECK_STR(a, b); free(a); free(b);
    CHECK(again.has_default && runtime_choice_equal(&again.def, &c.def));
    CHECK_INT(again.providers[2].available, -1); CHECK_INT(again.providers[0].available, 0);
    CHECK(again.providers[1].models[2].efforts == NULL);
    runtime_catalog_free(&again); json_free(saved);
    runtime_catalog_free(&c); json_free(j);
    j = json_parsez("{\"providers\":[]}"); CHECK(runtime_catalog_parse(j, &c));
    saved = runtime_catalog_json(&c); CHECK(json_is_null(json_get(saved, "default"))); CHECK_INT(json_count(saved), 2);
    json_free(saved); runtime_catalog_free(&c); json_free(j);
}
static void test_runtime_choice_falls_back_from_model_to_default_to_first(void) {
    Json *j = json_parsez(CATALOG);
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    RuntimeChoice ch;
    // A model without its own default effort takes its first.
    CHECK(runtime_catalog_choice(&c, 2, "sonnet", &ch)); CHECK_INT(ch.provider_id, 2); CHECK_STR(ch.model, "sonnet"); CHECK_STR(ch.effort, "low"); runtime_choice_free(&ch);
    CHECK(runtime_catalog_choice(&c, 2, "haiku", &ch)); CHECK_STR(ch.model, "haiku"); CHECK(ch.effort == NULL); runtime_choice_free(&ch);
    // A provider without models is still a choice, with nothing more to say.
    CHECK(runtime_catalog_choice(&c, 3, "x", &ch)); CHECK_INT(ch.provider_id, 3); CHECK(ch.model == NULL && ch.effort == NULL); runtime_choice_free(&ch);
    // A default model the provider does not list falls to its first; empty efforts give no effort.
    CHECK(runtime_catalog_choice(&c, 4, NULL, &ch)); CHECK_STR(ch.model, "m"); CHECK(ch.effort == NULL); runtime_choice_free(&ch);
    // An unavailable provider can still be chosen by hand.
    CHECK(runtime_catalog_choice(&c, 1, NULL, &ch)); CHECK_STR(ch.model, "gpt"); CHECK_STR(ch.effort, "low"); runtime_choice_free(&ch);
    ch.model = (char *)"untouched";
    CHECK(!runtime_catalog_choice(&c, 99, "opus", &ch)); CHECK_STR(ch.model, "untouched");
    runtime_choice_free(NULL);
    runtime_catalog_free(&c); json_free(j);
}
static void test_runtime_first_available_skips_only_providers_turned_off(void) {
    Json *j = json_parsez(CATALOG);
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    RuntimeChoice ch; CHECK(runtime_catalog_first_available(&c, &ch)); CHECK_INT(ch.provider_id, 2); CHECK_STR(ch.model, "opus"); CHECK_STR(ch.effort, "high"); runtime_choice_free(&ch);
    runtime_catalog_free(&c); json_free(j);
    // A provider that does not say is available, as older servers omit the field.
    j = json_parsez("{\"providers\":[{\"id\":1,\"label\":\"A\",\"available\":false,\"models\":[]},{\"id\":5,\"label\":\"B\",\"models\":[{\"id\":\"b\"}]}]}");
    CHECK(runtime_catalog_parse(j, &c)); CHECK(runtime_catalog_first_available(&c, &ch)); CHECK_INT(ch.provider_id, 5); CHECK_STR(ch.model, "b"); runtime_choice_free(&ch);
    runtime_catalog_free(&c); json_free(j);
    j = json_parsez("{\"providers\":[{\"id\":1,\"label\":\"A\",\"available\":false,\"models\":[]}]}");
    CHECK(runtime_catalog_parse(j, &c)); CHECK(!runtime_catalog_first_available(&c, &ch)); runtime_catalog_free(&c); json_free(j);
}
static void test_runtime_efforts_and_labels_for_known_and_unknown_choices(void) {
    Json *j = json_parsez(CATALOG);
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    RuntimeChoice opus = { 2, (char *)"opus", (char *)"high" }, haiku = { 2, (char *)"haiku", NULL }, gone = { 2, (char *)"gone", NULL },
                  nomodel = { 2, NULL, NULL }, stranger = { 9, (char *)"x", NULL }, bare = { 9, NULL, NULL }, unnamed = { 4, (char *)"m", NULL },
                  sonnet = { 2, (char *)"sonnet", NULL };
    size_t n = 99;
    const char *const *e = runtime_catalog_efforts(&c, &opus, &n); CHECK_INT(n, 2); CHECK_STR(e[1], "high");
    CHECK(runtime_catalog_efforts(&c, &haiku, &n) == NULL); CHECK_INT(n, 0);
    n = 99; CHECK(runtime_catalog_efforts(&c, &gone, &n) == NULL); CHECK_INT(n, 0);
    n = 99; CHECK(runtime_catalog_efforts(&c, &stranger, &n) == NULL); CHECK_INT(n, 0);
    CHECK(runtime_catalog_model(&c, &nomodel) == NULL); CHECK(runtime_catalog_model(&c, &opus) == &c.providers[1].models[1]);
    CHECK_OWNED_STR(runtime_catalog_label(&c, &opus), "Claude \xC2\xB7 Opus");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &sonnet), "Claude \xC2\xB7 sonnet");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &gone), "Claude \xC2\xB7 gone");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &nomodel), "Claude");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &stranger), "x");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &bare), "");
    CHECK_OWNED_STR(runtime_catalog_label(&c, &unnamed), "m");
    runtime_catalog_free(&c); json_free(j);
}
static void test_runtime_choice_copy_equality_and_arguments(void) {
    RuntimeChoice a = { 2, (char *)"opus", (char *)"high" }, c;
    runtime_choice_copy(&c, &a);
    CHECK(c.model != a.model); CHECK(runtime_choice_equal(&a, &c));
    RuntimeChoice other = { 2, (char *)"opus", NULL }, provider = { 3, (char *)"opus", (char *)"high" }, model = { 2, (char *)"sonnet", (char *)"high" };
    CHECK(!runtime_choice_equal(&a, &other)); CHECK(!runtime_choice_equal(&a, &provider)); CHECK(!runtime_choice_equal(&a, &model));
    RuntimeChoice n1 = { 1, NULL, NULL }, n2 = { 1, NULL, NULL }; CHECK(runtime_choice_equal(&n1, &n2));
    runtime_choice_free(&c); CHECK(c.model == NULL && c.provider_id == 0);
    runtime_choice_copy(&c, &n1); CHECK(c.model == NULL && c.effort == NULL && c.provider_id == 1); runtime_choice_free(&c);
    // Starts send "provider", as the client API names it; empty strings are left out.
    CHECK_OWNED_STR(serialized(runtime_choice_arguments(&a)), "{\"effort\":\"high\",\"model\":\"opus\",\"provider\":2}");
    RuntimeChoice empty = { 4, (char *)"", (char *)"" };
    CHECK_OWNED_STR(serialized(runtime_choice_arguments(&empty)), "{\"provider\":4}");
}

// MARK: - Pull request files

static void test_pull_file_reads_and_round_trips(void) {
    Json *j = json_parsez("{\"filename\":\"src/new.c\",\"previousFilename\":\"src/old.c\",\"status\":\"renamed\",\"additions\":0,\"deletions\":4,\"patch\":\"@@ -1 +1 @@\",\"url\":\"https://x/y\",\"sha\":\"abc\"}");
    PullFile f; CHECK(pull_file_parse(j, &f));
    CHECK_STR(f.filename, "src/new.c"); CHECK_STR(f.previous_filename, "src/old.c"); CHECK_STR(f.status, "renamed");
    CHECK_INT(f.additions, 0); CHECK_INT(f.deletions, 4); CHECK_STR(f.patch, "@@ -1 +1 @@"); CHECK_STR(f.url, "https://x/y");
    Json *saved = pull_file_json(&f);
    CHECK(json_is_null(json_get(saved, "sha")));
    PullFile again; CHECK(pull_file_parse(saved, &again));
    CHECK_STR(again.previous_filename, "src/old.c"); CHECK_INT(again.additions, 0); CHECK_INT(again.deletions, 4);
    Json *twice = pull_file_json(&again); CHECK(json_equal(saved, twice)); json_free(twice);
    pull_file_free(&again); json_free(saved);
    PullFile copy; pull_file_copy(&copy, &f);
    CHECK(copy.filename != f.filename && copy.patch != f.patch);
    pull_file_free(&f); CHECK(f.filename == NULL);
    CHECK_STR(copy.filename, "src/new.c"); CHECK_STR(copy.patch, "@@ -1 +1 @@"); CHECK_INT(copy.deletions, 4);
    pull_file_free(&copy); pull_file_free(NULL); json_free(j);
    // Counts the server left out stay unknown through a save.
    j = json_parsez("{\"filename\":\"a\",\"additions\":\"3\",\"deletions\":null}");
    CHECK(pull_file_parse(j, &f)); CHECK_INT(f.additions, -1); CHECK_INT(f.deletions, -1); CHECK(f.status == NULL && f.patch == NULL);
    saved = pull_file_json(&f); CHECK(json_is_null(json_get(saved, "additions")));
    pull_file_copy(&copy, &f); CHECK_INT(copy.additions, -1); pull_file_free(&copy);
    json_free(saved); pull_file_free(&f); json_free(j);
    const char *bad[] = { "{}", "{\"filename\":3}", "{\"filename\":null}", "\"a\"" };
    for (size_t i = 0; i < 4; i++) { j = json_parsez(bad[i]); CHECK(!pull_file_parse(j, &f)); CHECK(f.filename == NULL); json_free(j); }
}
static void test_pull_file_name_and_directory(void) {
    struct { const char *path, *name, *dir; } cases[] = {
        { "README.md", "README.md", "" },
        { "src/a.c", "a.c", "src" },
        { "a/b/c/d.txt", "d.txt", "a/b/c" },
        { "/rooted", "rooted", "" },
        { ".github/workflows/ci.yml", "ci.yml", ".github/workflows" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        PullFile f = { 0 }; f.filename = (char *)cases[i].path;
        CHECK_STR(pull_file_name(&f), cases[i].name);
        CHECK_OWNED_STR(pull_file_directory(&f), cases[i].dir);
    }
}
static void test_pull_files_page_reads_paging_and_rejects_without_files(void) {
    Json *j = json_parsez("{\"pr\":{\"headSha\":\"h\"},\"files\":[{\"filename\":\"a\"},{\"nope\":1},{\"filename\":\"b\"}],\"nextPage\":3,\"truncated\":true}");
    PullFilesPage p; CHECK(pull_files_page_parse(j, &p));
    CHECK_INT(p.file_count, 2); CHECK_STR(p.files[1].filename, "b"); CHECK_INT(p.next_page, 3); CHECK(p.truncated);
    CHECK_STR(json_str(json_get(p.pr, "headSha")), "h");
    pull_files_page_free(&p); CHECK(p.files == NULL && p.pr == NULL); pull_files_page_free(NULL); json_free(j);
    j = json_parsez("{\"files\":[],\"truncated\":\"yes\"}");
    CHECK(pull_files_page_parse(j, &p)); CHECK_INT(p.next_page, 0); CHECK(!p.truncated); CHECK(json_is_null(p.pr)); pull_files_page_free(&p); json_free(j);
    const char *bad[] = { "{}", "{\"files\":null}", "{\"files\":{}}", "[]", "null" };
    for (size_t i = 0; i < 5; i++) { j = json_parsez(bad[i]); CHECK(!pull_files_page_parse(j, &p)); CHECK(p.files == NULL); json_free(j); }
}
static void test_pull_file_list_starts_on_page_one_and_round_trips(void) {
    PullFileList l; pull_file_list_init(&l);
    CHECK_INT(l.next_page, 1); CHECK(json_is_null(l.pr)); CHECK_INT(l.file_count, 0); CHECK(!l.truncated);
    Json *args = pull_file_list_arguments(&l, "o/r", 7);
    CHECK_OWNED_STR(serialized(args), "{\"pr\":7,\"repo\":\"o/r\"}");
    Json *saved = pull_file_list_json(&l);
    CHECK_INT(json_num_or(json_get(saved, "nextPage"), 0), 1);
    PullFileList again; CHECK(pull_file_list_parse(saved, &again)); CHECK_INT(again.next_page, 1);
    pull_file_list_free(&again); json_free(saved);
    pull_file_list_free(&l); pull_file_list_free(NULL);
    Json *j = json_parsez("{\"pr\":{\"headSha\":\"h\",\"baseSha\":\"b\"},\"files\":[{\"filename\":\"a\",\"additions\":2}],\"nextPage\":4,\"truncated\":true}");
    CHECK(pull_file_list_parse(j, &l));
    saved = pull_file_list_json(&l);
    CHECK(pull_file_list_parse(saved, &again));
    CHECK_INT(again.next_page, 4); CHECK(again.truncated); CHECK_INT(again.file_count, 1); CHECK_INT(again.files[0].additions, 2);
    Json *twice = pull_file_list_json(&again); CHECK(json_equal(saved, twice)); json_free(twice);
    pull_file_list_free(&again); json_free(saved); pull_file_list_free(&l); json_free(j);
    const char *bad[] = { "{}", "{\"files\":{}}", "null" };
    for (size_t i = 0; i < 3; i++) { j = json_parsez(bad[i]); CHECK(!pull_file_list_parse(j, &l)); CHECK(l.pr == NULL && l.files == NULL); json_free(j); }
}
static void test_pull_file_list_arguments_pin_later_pages(void) {
    Json *j = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"baseSha\":\"b1\"},\"files\":[],\"nextPage\":3}");
    PullFileList l; CHECK(pull_file_list_parse(j, &l)); json_free(j);
    CHECK_OWNED_STR(serialized(pull_file_list_arguments(&l, "o/r", 7)), "{\"baseSha\":\"b1\",\"headSha\":\"h1\",\"page\":3,\"pr\":7,\"repo\":\"o/r\"}");
    pull_file_list_free(&l);
    // Without the revision to pin to, the shas are left out rather than sent empty.
    j = json_parsez("{\"pr\":null,\"files\":[],\"nextPage\":2}");
    CHECK(pull_file_list_parse(j, &l)); json_free(j);
    CHECK_OWNED_STR(serialized(pull_file_list_arguments(&l, "o/r", 7)), "{\"page\":2,\"pr\":7,\"repo\":\"o/r\"}");
    l.next_page = 0; CHECK(pull_file_list_arguments(&l, "o/r", 7) == NULL);
    pull_file_list_free(&l);
}
static void test_pull_file_list_append_keeps_the_first_revision_and_skips_repeats(void) {
    PullFileList l; pull_file_list_init(&l);
    Json *j1 = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"body\":\"first\"},\"files\":[{\"filename\":\"a\"},{\"filename\":\"a\",\"status\":\"dup\"},{\"filename\":\"b\"}],\"nextPage\":2,\"truncated\":true}");
    Json *j2 = json_parsez("{\"pr\":{\"headSha\":\"h2\",\"body\":\"second\"},\"files\":[{\"filename\":\"b\"},{\"filename\":\"c\"}],\"nextPage\":null,\"truncated\":false}");
    PullFilesPage p1, p2; CHECK(pull_files_page_parse(j1, &p1)); CHECK(pull_files_page_parse(j2, &p2));
    pull_file_list_append(&l, &p1);
    CHECK_INT(l.file_count, 2); CHECK(l.files[0].status == NULL); CHECK_INT(l.next_page, 2); CHECK(l.truncated);
    // The list owns copies, not the page's strings.
    CHECK(l.files[0].filename != p1.files[0].filename);
    pull_files_page_free(&p1);
    CHECK_STR(l.files[1].filename, "b");
    pull_file_list_append(&l, &p2);
    CHECK_INT(l.file_count, 3); CHECK_STR(l.files[2].filename, "c"); CHECK_INT(l.next_page, 0); CHECK(!l.truncated);
    CHECK_STR(json_str(json_get(l.pr, "body")), "first");
    pull_files_page_free(&p2); pull_file_list_free(&l);
    json_free(j1); json_free(j2);
}
static void test_pull_file_list_confirm_needs_the_same_revision(void) {
    Json *saved = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"body\":\"old\"},\"files\":[{\"filename\":\"a\"}],\"nextPage\":null,\"truncated\":true}");
    PullFileList l; CHECK(pull_file_list_parse(saved, &l)); json_free(saved);
    // Neither side has a base: still the same revision.
    Json *j = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"body\":\"new\"},\"files\":[{\"filename\":\"z\"}],\"nextPage\":2,\"truncated\":false}");
    PullFilesPage p; CHECK(pull_files_page_parse(j, &p)); json_free(j);
    CHECK(pull_file_list_confirm(&l, &p)); pull_files_page_free(&p);
    CHECK_STR(json_str(json_get(l.pr, "body")), "new");
    CHECK_INT(l.file_count, 1); CHECK_STR(l.files[0].filename, "a"); CHECK_INT(l.next_page, 0); CHECK(l.truncated);
    const char *other[] = { "{\"pr\":{\"headSha\":\"h1\",\"baseSha\":\"b\"},\"files\":[]}", "{\"pr\":{},\"files\":[]}", "{\"files\":[]}", "{\"pr\":{\"headSha\":\"H1\"},\"files\":[]}" };
    for (size_t i = 0; i < 4; i++) {
        j = json_parsez(other[i]); CHECK(pull_files_page_parse(j, &p)); json_free(j);
        CHECK(!pull_file_list_confirm(&l, &p)); pull_files_page_free(&p);
        CHECK_STR(json_str(json_get(l.pr, "body")), "new");
    }
    pull_file_list_free(&l);
}

// MARK: - Dates

static void test_board_date_parse_reads_utc_and_offsets(void) {
    struct { const char *text; long long epoch; } cases[] = {
        { "1970-01-01T00:00:00Z", 0 },
        { "2026-09-28T15:55:49Z", 1790610949 },
        { "2026-09-28T15:55:49.120Z", 1790610949 },
        { "2026-09-28T15:55:49,5Z", 1790610949 },
        { "2026-09-28T15:55:49.123456789Z", 1790610949 },
        { "2026-09-28t15:55:49z", 1790610949 },
        { "2026-09-28 15:55:49Z", 1790610949 },
        { "2026-09-28T15:55Z", 1790610900 },
        { "2026-09-28T17:55:49+02:00", 1790610949 },
        { "2026-09-28T17:55:49+0200", 1790610949 },
        { "2026-09-28T17:55:49+02", 1790610949 },
        { "2026-09-28T14:25:49-01:30", 1790610949 },
        { "2026-09-28T15:55:49.5-00:00", 1790610949 },
        { "2000-02-29T12:00:00Z", 951825600 },
        { "2000-03-01T00:00:00Z", 951868800 },
        { "2100-03-01T00:00:00Z", 4107542400LL },
        { "1969-12-31T23:59:59Z", -1 },
        { "1900-01-01T00:00:00Z", -2208988800LL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        time_t t = 12345;
        bool ok = board_date_parse(cases[i].text, &t);
        if (!ok) printf("  unread: %s\n", cases[i].text);
        CHECK(ok); CHECK_INT(t, cases[i].epoch);
    }
}
static void test_board_date_parse_rejects_what_is_not_a_timestamp(void) {
    const char *bad[] = {
        NULL, "", "2026-09-28", "2026-09-28T", "2026-09-28T15:55:49", "2026-09-28T15:55:49 Z", "2026-09-28T15:55:49Zx",
        "2026-9-28T15:55:49Z", "26-09-28T15:55:49Z", "2026/09/28T15:55:49Z", "2026-09-28X15:55:49Z", "2026-09-28T15-55-49Z",
        "2026-09-28T15:55:4Z", "2026-09-28T15:55:49.Z", "2026-09-28T15:55:49+2", "2026-09-28T15:55:49+02:0",
        "2026-00-28T15:55:49Z", "2026-13-28T15:55:49Z", "2026-09-00T15:55:49Z", "2026-09-32T15:55:49Z",
        "2026-09-28T24:00:00Z", "2026-09-28T15:60:00Z", "2026-09-28T15:55:61Z", "Mon, 28 Sep 2026 15:55:49 GMT",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        time_t t = 12345;
        bool ok = board_date_parse(bad[i], &t);
        if (ok) printf("  read: %s\n", bad[i] ? bad[i] : "(null)");
        CHECK(!ok); CHECK_INT(t, 12345);
    }
    // A leap second is read, landing on the next minute.
    time_t t; CHECK(board_date_parse("2016-12-31T23:59:60Z", &t)); CHECK_INT(t, 1483228800);
}

void models_tests(void) {
    test_run("device reads every field", test_device_reads_every_field);
    test_run("device rejects missing or mistyped fields", test_device_rejects_missing_or_mistyped_fields);
    test_run("device round trips and copies deeply", test_device_round_trips_and_copies_deeply);
    test_run("permission ranks and who may manage", test_permission_ranks_and_who_may_manage);
    test_run("admin tokens manage since the client API", test_admin_tokens_manage_since_the_client_api);
    test_run("discovery reads the client record and transcription", test_discovery_reads_the_client_record_and_transcription);
    test_run("discovery rejects a missing version or client", test_discovery_rejects_a_missing_version_or_client);
    test_run("voice notes off says what to do", test_voice_notes_off_says_what_to_do);
    test_run("routes read each operation of an OpenAPI document", test_routes_read_each_operation_of_an_openapi_document);
    test_run("routes reject what is neither OpenAPI nor a saved list", test_routes_reject_what_is_neither_openapi_nor_a_saved_list);
    test_run("routes round trip and copy deeply", test_routes_round_trip_and_copy_deeply);
    test_run("routes allow matches parameters on either side", test_routes_allow_matches_parameters_on_either_side);
    test_run("routes allow needs the method and enough permission", test_routes_allow_needs_the_method_and_enough_permission);
    test_run("connection round trips with its transcription", test_connection_round_trips_with_its_transcription);
    test_run("connection rejects a bad device or routes", test_connection_rejects_a_bad_device_or_routes);
    test_run("projects read label and title", test_projects_read_label_and_title);
    test_run("project copy is independent", test_project_copy_is_independent);
    test_run("session needs an id and a status", test_session_needs_an_id_and_a_status);
    test_run("session copy is independent", test_session_copy_is_independent);
    test_run("session accessors fall back when fields are missing", test_session_accessors_fall_back_when_fields_are_missing);
    test_run("session is active by status", test_session_is_active_by_status);
    test_run("session live input and queue", test_session_live_input_and_queue);
    test_run("session review loop flags", test_session_review_loop_flags);
    test_run("held triage needs findings and prefers the standalone review", test_held_triage_needs_findings_and_prefers_the_standalone_review);
    test_run("session pull number ignores what is not a pull request", test_session_pull_number_ignores_what_is_not_a_pull_request);
    test_run("held rounds of an empty list", test_held_rounds_of_an_empty_list);
    test_run("held rounds with equal holds keep the list order", test_held_rounds_with_equal_holds_keep_the_list_order);
    test_run("held round URL reuses the conversation's own link only for its pull request", test_held_round_url_reuses_the_conversations_own_link_only_for_its_pull_request);
    test_run("held round is mine unless a standalone review says otherwise", test_held_round_is_mine_unless_a_standalone_review_says_otherwise);
    test_run("triage outcome text covers every outcome", test_triage_outcome_text_covers_every_outcome);
    test_run("findings subtitle counts reviews and pull requests", test_findings_subtitle_counts_reviews_and_pull_requests);
    test_run("event reads every field", test_event_reads_every_field);
    test_run("event needs a kind and a sequence", test_event_needs_a_kind_and_a_sequence);
    test_run("event detail prefers text over summary", test_event_detail_prefers_text_over_summary);
    test_run("event visible by kind and content", test_event_visible_by_kind_and_content);
    test_run("setup output stays in the transcript", test_setup_output_stays_in_the_transcript);
    test_run("transcript cursor only moves forward", test_transcript_cursor_only_moves_forward);
    test_run("transcript grows past its first allocation", test_transcript_grows_past_its_first_allocation);
    test_run("saved transcript restores events and cursor", test_saved_transcript_restores_events_and_cursor);
    test_run("runtime catalog reads providers and models", test_runtime_catalog_reads_providers_and_models);
    test_run("runtime catalog rejects malformed providers", test_runtime_catalog_rejects_malformed_providers);
    test_run("runtime catalog round trips", test_runtime_catalog_round_trips);
    test_run("runtime choice falls back from model to default to first", test_runtime_choice_falls_back_from_model_to_default_to_first);
    test_run("runtime first available skips only providers turned off", test_runtime_first_available_skips_only_providers_turned_off);
    test_run("runtime efforts and labels for known and unknown choices", test_runtime_efforts_and_labels_for_known_and_unknown_choices);
    test_run("runtime choice copy, equality and arguments", test_runtime_choice_copy_equality_and_arguments);
    test_run("pull file reads and round trips", test_pull_file_reads_and_round_trips);
    test_run("pull file name and directory", test_pull_file_name_and_directory);
    test_run("pull files page reads paging and rejects without files", test_pull_files_page_reads_paging_and_rejects_without_files);
    test_run("pull file list starts on page one and round trips", test_pull_file_list_starts_on_page_one_and_round_trips);
    test_run("pull file list arguments pin later pages", test_pull_file_list_arguments_pin_later_pages);
    test_run("pull file list append keeps the first revision and skips repeats", test_pull_file_list_append_keeps_the_first_revision_and_skips_repeats);
    test_run("pull file list confirm needs the same revision", test_pull_file_list_confirm_needs_the_same_revision);
    test_run("board date parse reads UTC and offsets", test_board_date_parse_reads_utc_and_offsets);
    test_run("board date parse rejects what is not a timestamp", test_board_date_parse_rejects_what_is_not_a_timestamp);
}
