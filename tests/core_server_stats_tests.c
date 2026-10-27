#include "server_stats.h"
#include "api.h"
#include "str.h"
#include "test.h"
#include "suites.h"
#include "fixtures/stats/server.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void server_fixture(void) {
    Json *j = json_parsez(SERVER_STATS_FIXTURE); ServerStats s;
    CHECK(server_stats_parse(j, &s)); json_free(j);
    CHECK_INT(s.host.cpus.value, 4); CHECK_INT(s.heap_used.value, 10); CHECK_INT(s.heap_total.value, 20);
    CHECK_STR(s.node, "22.1.0"); CHECK_INT(s.sessions.processes.value, 7);
    CHECK_INT(s.at_ms.value, 2000); CHECK_INT(s.disk_count, 2);
    StatTile tiles[6]; server_stats_tiles(&s, tiles);
    CHECK_STR(tiles[0].value, "80%"); CHECK(tiles[0].fraction.value == 0.8);
    CHECK_STR(tiles[1].value, "950 B"); CHECK(tiles[1].fraction.value == 0.95);
    CHECK_STR(tiles[2].value, "3.80"); CHECK(tiles[2].fraction.value == 0.95);
    CHECK_STR(tiles[3].value, "1d 1h"); CHECK_STR(tiles[4].value, "40%"); CHECK(tiles[4].fraction.value == 0.1);
    CHECK_STR(tiles[5].value, "2"); CHECK(tiles[5].fraction.value == 0.3); stats_tiles_free(tiles, 6);
    StatTile tile = server_stats_disk_tile(&s.disks[1]); CHECK_STR(tile.label, "Files and recordings");
    CHECK_STR(tile.value, "10 B"); CHECK(tile.fraction.value == 0.1); stats_tiles_free(&tile, 1);
    CHECK_INT(s.lines[0].valid_count, 1); CHECK(!s.lines[0].points[0].number.present);
    CHECK_INT(s.lines[1].points[0].number.value, 10); CHECK_INT(s.lines[1].points[1].number.value, 20);
    CHECK_INT(s.lines[2].latest.value, 950); CHECK_INT(s.lines[3].latest.value, 310);
    CHECK_INT(s.lines[1].gap_ms, 20000);
    StatsSeries copy = stats_series_copy(&s.lines[1]); CHECK(copy.points != s.lines[1].points);
    CHECK_INT(copy.count, 2); CHECK_INT(copy.latest.value, 20); stats_series_free(&copy); server_stats_free(&s);
    CHECK_OWNED_STR(stats_uptime((StatNumber){3720, true}), "1h 2m");
    CHECK_OWNED_STR(stats_uptime((StatNumber){40, true}), "40s"); CHECK_OWNED_STR(stats_uptime((StatNumber){60, true}), "1m");
    CHECK_OWNED_STR(stats_uptime((StatNumber){NAN, true}), "—"); CHECK_OWNED_STR(stats_uptime((StatNumber){0}), "—");
    CHECK_OWNED_STR(stats_age((StatNumber){1000, true}, 41), "sample age 40s");
    CHECK_OWNED_STR(stats_age((StatNumber){1000, true}, 0), "sample age unavailable");
}
static void partial_invalid_and_history(void) {
    ServerStats s; const char *invalid[] = {"null", "{}", "{\"host\":{},\"process\":{},\"sessions\":{},\"disks\":[],\"history\":null}"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        Json *j = json_parsez(invalid[i]); CHECK(!server_stats_parse(j, &s)); server_stats_free(&s); json_free(j);
    }
    Json *j = json_parsez("{\"host\":{\"memoryBytes\":10,\"freeMemoryBytes\":20},\"process\":{},\"sessions\":{},\"disks\":[{},false,{\"label\":\"custom\",\"totalBytes\":10,\"freeBytes\":1,\"usedBytes\":-1}],\"history\":[]}");
    CHECK(server_stats_parse(j, &s)); CHECK_INT(s.disk_count, 1); CHECK(!s.disks[0].used.present);
    StatTile tiles[6]; server_stats_tiles(&s, tiles); CHECK_STR(tiles[1].value, "—"); CHECK_STR(tiles[5].value, "—");
    stats_tiles_free(tiles, 6); CHECK_INT(s.lines[0].count, 0); server_stats_free(&s);
    Json *host = (Json *)json_get(j, "host"); json_set_num(host, "cpuPercent", INFINITY); json_set_num(host, "uptimeSeconds", -1);
    json_set_num(host, "memoryBytes", NAN); json_set_num(host, "cpus", 0);
    CHECK(server_stats_parse(j, &s)); CHECK(!s.cpu.present); CHECK(!s.uptime.present); CHECK(!s.host.memory.present); server_stats_free(&s); json_free(j);
    j = json_parsez("{\"host\":{\"cpus\":4,\"memoryBytes\":1000},\"process\":{\"cpuPercent\":800},\"sessions\":{\"open\":0},\"disks\":[],\"history\":["
        "{\"at\":4000000,\"cpuPercent\":800,\"memoryBytes\":0},{\"at\":4000000,\"cpuPercent\":800},{\"at\":3900000,\"cpuPercent\":-1,\"freeMemoryBytes\":2000},"
        "{\"at\":1000},{\"at\":0},{\"at\":1.5},{\"at\":null}]}");
    CHECK(server_stats_parse(j, &s)); CHECK_INT(s.lines[0].count, 2); CHECK_INT(s.lines[0].first_ms, 3900000);
    CHECK(!s.lines[1].points[0].number.present); CHECK_INT(s.lines[1].latest.value, 200); CHECK(!s.lines[2].latest.present);
    CHECK(s.open_sessions.present); CHECK_INT(s.open_sessions.value, 0); server_stats_tiles(&s, tiles);
    CHECK_STR(tiles[4].value, "800%"); CHECK(tiles[4].fraction.value == 1); CHECK_STR(tiles[5].value, "0");
    stats_tiles_free(tiles, 6); server_stats_free(&s); json_free(j);
}
static void busiest_and_known_records(void) {
    Session known[2] = {{0}};
    known[0].raw = json_parsez("{\"id\":\"c\",\"title\":\"Fix build\",\"repo\":\"one/repo\",\"status\":\"idle\"}");
    known[1].raw = json_parsez("{\"id\":\"c\",\"title\":\"Duplicate\",\"repo\":\"wrong/repo\"}");
    Json *j = json_parsez(OPEN_STATS_FIXTURE); OpenStats s;
    CHECK(open_stats_parse(j, known, 2, &s)); json_free(j);
    CHECK_INT(s.count, 3); CHECK_STR(s.items[0].id, "c"); CHECK_STR(s.items[1].id, "a"); CHECK_STR(s.items[2].id, "b");
    CHECK_STR(session_display_title(&s.items[0].record), "Fix build"); CHECK_STR(session_repo(&s.items[0].record), "one/repo");
    CHECK_STR(session_id(&s.items[1].record), "a"); CHECK_STR(session_display_title(&s.items[1].record), "a");
    CHECK(json_is_null(json_get(s.items[1].record.raw, "repo"))); CHECK(!s.items[2].figures.cpu.present);
    CHECK_INT(s.at_ms.value, 2000); open_stats_free(&s); session_free(&known[0]); session_free(&known[1]);
    j = json_parsez("{\"host\":{},\"sessions\":[{\"sessionId\":\"z\",\"cpuPercent\":0},{\"sessionId\":\"d\",\"cpuPercent\":0},{\"sessionId\":\"z\",\"cpuPercent\":9},{\"sessionId\":\"n\",\"cpuPercent\":-1}]}");
    CHECK(open_stats_parse(j, NULL, 0, &s)); CHECK_INT(s.count, 3); CHECK_STR(s.items[0].id, "d"); CHECK_STR(s.items[1].id, "z");
    CHECK(s.items[0].figures.cpu.present); CHECK(!s.items[2].figures.cpu.present); open_stats_free(&s); json_free(j);
    j = json_parsez("{\"host\":{},\"sessions\":[]}"); CHECK(open_stats_parse(j, NULL, 0, &s)); CHECK_INT(s.count, 0); open_stats_free(&s); json_free(j);
    j = json_parsez("{\"host\":{}}"); CHECK(!open_stats_parse(j, NULL, 0, &s)); json_free(j);
}
static void sampling_gaps(void) {
    const char *hosts[] = {"{\"intervalMs\":500}", "{\"intervalMs\":10000}", "{\"intervalMs\":3000000000}",
        "{}", "{\"intervalMs\":null}", "{\"intervalMs\":0}", "{\"intervalMs\":-1}", "{\"intervalMs\":1.5}", "{\"intervalMs\":\"500\"}"};
    const double gaps[] = {1000, 20000, 6000000000.0, 10000, 10000, 10000, 10000, 10000, 10000};
    for (size_t i = 0; i < sizeof hosts / sizeof *hosts; i++) {
        Json *j = json_parsez("{\"process\":{},\"sessions\":{},\"disks\":[],\"stats\":{\"sessionId\":\"s1\"},\"history\":["
            "{\"at\":1000,\"hostCpuPercent\":20,\"cpuPercent\":10,\"memoryBytes\":10},"
            "{\"at\":4000,\"hostCpuPercent\":30,\"cpuPercent\":20,\"memoryBytes\":20}]}");
        json_object_set(j, "host", json_parsez(hosts[i]));
        ServerStats server; SessionStats session;
        CHECK(server_stats_parse(j, &server)); CHECK(session_stats_parse(j, "s1", &session));
        for (int k = 0; k < 4; k++) CHECK(server.lines[k].gap_ms == gaps[i]);
        for (int unit = STATS_PERCENT; unit <= STATS_BYTES; unit++) {
            StatsSeries line = session_stats_series(&session, (StatsUnit)unit);
            CHECK(line.gap_ms == gaps[i]); CHECK_INT(line.valid_count, 2);
            if (i == 0) CHECK(line.points[1].at_ms - line.points[0].at_ms > line.gap_ms);
            stats_series_free(&line);
        }
        if (i == 0) {
            CHECK_INT(stats_poll_ms(&server.host), 2000); CHECK_INT(stats_poll_ms(&session.host), 2000);
            CHECK(server.lines[0].points[1].at_ms - server.lines[0].points[0].at_ms > server.lines[0].gap_ms);
        }
        server_stats_free(&server); session_stats_free(&session); json_free(j);
    }
}
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    CHECK_STR(method, "GET"); CHECK_STR(url, (const char *)ctx);
    *status = 200; *type = xstrdup("application/json"); *retry = NULL; *response = xstrdup("{}"); *len = 2; return true;
}
static void routes(void) {
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://example.com", &address));
    ApiClient *c = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error); server_address_free(&address);
    const char *ops[] = {"server_stats", "sessions_stats"}, *urls[] = {"https://example.com/api/v1/stats/server", "https://example.com/api/v1/stats/sessions"};
    for (int i = 0; i < 2; i++) {
        api_client_set_transport(c, transport, (void *)urls[i]); Json *args = json_object(); Json *result = api_call(c, ops[i], args, 0, &error);
        CHECK(result != NULL); json_free(result); json_free(args); api_error_clear(&error);
    }
    api_client_release(c);
}
void core_server_stats_tests(void) {
    test_run("Server endpoint fixture derives machine process disk and history views", server_fixture);
    test_run("Server partial and malformed metrics preserve gaps zeros raw values and gauges", partial_invalid_and_history);
    test_run("Server and session history gaps follow sampling cadence independently of polling limits", sampling_gaps);
    test_run("Open resources sort busiest with stable ties and retain saved navigation identity", busiest_and_known_records);
    test_run("Server resources named reads use exact API wire routes", routes);
}
