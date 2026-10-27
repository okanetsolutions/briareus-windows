#include "stats.h"
#include "api.h"
#include "str.h"
#include "test.h"
#include "suites.h"
#include "fixtures/stats/session.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static StatNumber known(double n) { return (StatNumber){n, true}; }
static void fixture_views(void) {
    Json *j = json_parsez(SESSION_STATS_FIXTURE); SessionStats s;
    CHECK(session_stats_parse(j, "s1", &s)); json_free(j);
    CHECK(s.open); CHECK(s.at_ms.present); CHECK_INT(s.at_ms.value, 1000);
    CHECK(s.roles[STATS_AGENT]); CHECK(!s.roles[STATS_SERVE]); CHECK(s.roles[STATS_BROWSER]);
    CHECK(!s.role[STATS_BROWSER].cpu.present); CHECK_INT(s.history_count, 2);
    CHECK_INT(s.disk_at_ms.value, 900); CHECK_INT(stats_poll_ms(&s.host), 5000);
    CHECK_STR(stats_role_label(STATS_AGENT), "Agent"); CHECK_STR(stats_role_label(-1), "");
    StatTile tiles[4]; session_stats_tiles(&s, tiles);
    CHECK_STR(tiles[0].value, "200%"); CHECK(tiles[0].fraction.value == 0.5);
    CHECK(tiles[1].fraction.value == 0.5); CHECK_STR(tiles[2].value, "5"); CHECK_STR(tiles[3].value, "2.0 KB");
    CHECK(strstr(tiles[2].detail, "Agent") < strstr(tiles[2].detail, "Shared browser"));
    CHECK(stats_cpu_share(&s.role[STATS_AGENT], &s.host).value == 0.375);
    stats_tiles_free(tiles, 4);
    StatsSeries cpu = session_stats_series(&s, STATS_PERCENT), memory = session_stats_series(&s, STATS_BYTES);
    CHECK_INT(cpu.count, 2); CHECK_INT(cpu.valid_count, 1); CHECK(!cpu.points[0].number.present);
    CHECK_INT(cpu.points[1].at_ms, 2000); CHECK_INT(cpu.points[1].number.value, 50);
    CHECK_INT(memory.valid_count, 2); CHECK_INT(memory.latest.value, 20); CHECK_INT(memory.maximum, 20);
    CHECK_INT(cpu.first_ms, 1000); CHECK_INT(cpu.last_ms, 2000);
    CHECK_OWNED_STR(stats_axis(STATS_PERCENT, cpu.latest), "50%");
    CHECK_OWNED_STR(stats_axis(STATS_BYTES, memory.latest), "20 B");
    stats_series_free(&cpu); stats_series_free(&memory); session_stats_free(&s);
}
static void missing_and_malformed(void) {
    const char *invalid[] = {"null", "{}", "{\"host\":{},\"history\":[]}", "{\"host\":{},\"stats\":false,\"history\":[]}",
        "{\"host\":{},\"stats\":{},\"history\":[]}", "{\"host\":{},\"stats\":{\"sessionId\":\"other\"},\"history\":[]}",
        "{\"host\":{},\"stats\":null,\"history\":{}}"};
    SessionStats s;
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        Json *j = json_parsez(invalid[i]); CHECK(!session_stats_parse(j, "s1", &s)); CHECK(s.history == NULL); json_free(j);
    }
    Json *j = json_parsez("{\"host\":{},\"stats\":null,\"history\":[{\"at\":1000,\"cpuPercent\":900}]}");
    CHECK(session_stats_parse(j, "s1", &s)); CHECK(!s.open); CHECK_INT(s.history_count, 0);
    StatsSeries series = session_stats_series(&s, STATS_PERCENT); CHECK_INT(series.count, 0); stats_series_free(&series);
    session_stats_free(&s); json_free(j);
    j = json_parsez("{\"host\":{\"cpus\":0,\"memoryBytes\":0},\"stats\":{\"sessionId\":\"s1\",\"cpuPercent\":0,\"memoryBytes\":0,\"processes\":0},\"history\":[]}");
    CHECK(session_stats_parse(j, "s1", &s)); CHECK(s.open); CHECK(s.total.cpu.present); CHECK(s.total.memory.present); CHECK(s.total.processes.present);
    CHECK(!stats_cpu_share(&s.total, &s.host).present); CHECK(!stats_share(s.total.memory, s.host.memory).present);
    StatTile tiles[4]; session_stats_tiles(&s, tiles); CHECK_STR(tiles[0].value, "0%"); CHECK_STR(tiles[1].value, "0 B"); CHECK_STR(tiles[2].value, "0");
    CHECK(!tiles[3].fraction.present); CHECK(!s.disk_bytes.present); stats_tiles_free(tiles, 4); session_stats_free(&s);
    Json *raw = (Json *)json_get(j, "stats");
    json_set_num(raw, "cpuPercent", INFINITY); json_set_num(raw, "memoryBytes", -1); json_set_num(raw, "processes", 1.5);
    json_set_num(raw, "at", 1e20); Json *disk = json_object(); json_set_num(disk, "bytes", NAN); json_set_num(disk, "measuredAt", -2); json_object_set(raw, "disk", disk);
    Json *host = (Json *)json_get(j, "host"); json_set_num(host, "cpus", NAN); json_set_num(host, "memoryBytes", 1e20);
    CHECK(session_stats_parse(j, "s1", &s)); CHECK(!s.total.cpu.present); CHECK(!s.total.memory.present); CHECK(!s.total.processes.present);
    CHECK(!s.at_ms.present); CHECK(!s.disk_bytes.present); CHECK(!s.disk_at_ms.present); CHECK(!s.host.cpus.present); CHECK(!s.host.memory.present);
    session_stats_free(&s); json_free(j);
}
static void history_and_units(void) {
    Json *j = json_parsez("{\"host\":{},\"stats\":{\"sessionId\":\"s1\",\"at\":4000001},\"history\":["
        "{\"at\":4000000,\"cpuPercent\":-1,\"memoryBytes\":-1},{\"at\":4000000,\"cpuPercent\":30},"
        "{\"at\":3999000,\"cpuPercent\":0,\"memoryBytes\":0},{\"at\":1000,\"cpuPercent\":10},{\"at\":-10},{\"at\":null},{\"at\":1.5}]}");
    SessionStats s; CHECK(session_stats_parse(j, "s1", &s)); json_free(j);
    CHECK_INT(s.history_count, 2); CHECK_INT(s.history[0].at_ms, 3999000);
    CHECK(s.history[0].figures.cpu.present); CHECK_INT(s.history[0].figures.cpu.value, 0);
    CHECK(!s.history[1].figures.memory.present); session_stats_free(&s);
    CHECK_OWNED_STR(stats_bytes(known(0)), "0 B"); CHECK_OWNED_STR(stats_bytes(known(512)), "512 B");
    CHECK_OWNED_STR(stats_bytes(known(1536)), "1.5 KB"); CHECK_OWNED_STR(stats_bytes(known(24.0 * 1024 * 1024 * 1024)), "24 GB");
    CHECK_OWNED_STR(stats_bytes((StatNumber){0}), "—"); CHECK_OWNED_STR(stats_bytes(known(INFINITY)), "—");
    CHECK_OWNED_STR(stats_percent(known(0.4)), "0.4%"); CHECK_OWNED_STR(stats_percent(known(0.96)), "1%");
    CHECK_OWNED_STR(stats_percent(known(NAN)), "—"); CHECK_OWNED_STR(stats_percent(known(-1)), "—");
    CHECK_OWNED_STR(stats_processes(known(1), true), "1 process"); CHECK_OWNED_STR(stats_processes(known(2), true), "2 processes");
    CHECK_OWNED_STR(stats_processes((StatNumber){0}, false), "—"); CHECK_OWNED_STR(stats_processes((StatNumber){0}, true), "Processes unavailable");
    CHECK_OWNED_STR(stats_time(known(3600000)), "01:00 UTC"); CHECK_OWNED_STR(stats_time((StatNumber){0}), "unavailable");
    CHECK_INT(stats_tone((StatNumber){0}), STAT_MUTED); CHECK_INT(stats_tone(known(0)), STAT_OK);
    CHECK_INT(stats_tone(known(0.749)), STAT_OK); CHECK_INT(stats_tone(known(0.75)), STAT_WARN); CHECK_INT(stats_tone(known(0.9)), STAT_DANGER);
    CHECK(!stats_share(known(NAN), known(10)).present); CHECK(!stats_share(known(1), known(INFINITY)).present);
    CHECK(!stats_share((StatNumber){0}, known(10)).present); CHECK(stats_share(known(100), known(10)).value == 1);
    StatsHost host = {0}; CHECK_INT(stats_poll_ms(NULL), 5000); CHECK_INT(stats_poll_ms(&host), 5000);
    host.interval_ms = known(100); CHECK_INT(stats_poll_ms(&host), 2000);
    host.interval_ms = known(10000); CHECK_INT(stats_poll_ms(&host), 10000);
    host.interval_ms = known(1e20); CHECK_INT(stats_poll_ms(&host), INT_MAX);
    CHECK(stats_retry_deadline(100, 0, 1, -1) == 102); CHECK(stats_retry_deadline(100, 0, 9, -1) == 160);
    CHECK(stats_retry_deadline(100, 300, 2, 0) == 300); CHECK(stats_retry_deadline(100, 0, 1, 5.5) == 106);
    CHECK(stats_retry_deadline(100, 0, 1, 3000000) == 3000100);
}
static bool route_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    (*(int *)ctx)++; CHECK_STR(method, "GET"); CHECK_STR(url, "https://example.com/api/v1/sessions/a%20b%2F%3F/stats");
    *status = 200; *type = xstrdup("application/json"); *retry = NULL; *response = xstrdup(SESSION_STATS_FIXTURE); *len = strlen(*response); return true;
}
static void wire_route(void) {
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://example.com", &address));
    ApiClient *c = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error); server_address_free(&address);
    int calls = 0; api_client_set_transport(c, route_transport, &calls);
    Json *args = json_object(); Json *result = api_call(c, "session_stats", args, 0, &error);
    CHECK(result == NULL); CHECK_INT(error.status, 400); CHECK_INT(calls, 0); api_error_clear(&error);
    json_set_str(args, "sessionId", "a b/?"); result = api_call(c, "session_stats", args, 0, &error);
    CHECK(result != NULL); CHECK_INT(calls, 1); json_free(result); json_free(args); api_error_clear(&error); api_client_release(c);
}
void core_stats_tests(void) {
    test_run("session resource fixture derives Swift tiles roles and history", fixture_views);
    test_run("session resources distinguish closed, malformed, unavailable and measured zero", missing_and_malformed);
    test_run("resource history units tones and polling reject unsafe values", history_and_units);
    test_run("session resources GET encodes identity and rejects missing arguments", wire_route);
}
