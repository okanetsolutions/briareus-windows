#include "meeting.h"
#include "meet_tools.h"
#include "meet_audio.h"
#include "screens.h"
#include "str.h"
#include "ws.h"
#include "api.h"
#include <process.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wincred.h>

// MARK: - The API keys

static const wchar_t *const KEY_TARGETS[] = { [MEETING_KEY_OPENAI] = L"Briareus OpenAI API key", [MEETING_KEY_ELEVENLABS] = L"Briareus ElevenLabs API key" };
static const wchar_t *const KEY_USERS[] = { [MEETING_KEY_OPENAI] = L"openai", [MEETING_KEY_ELEVENLABS] = L"elevenlabs" };

char *meeting_key_read(MeetingKey which, bool *failed) {
    if (failed) *failed = false;
    PCREDENTIALW cred = NULL;
    if (!CredReadW(KEY_TARGETS[which], CRED_TYPE_GENERIC, 0, &cred)) {
        if (GetLastError() != ERROR_NOT_FOUND && failed) *failed = true;
        return NULL;
    }
    char *key = cred->CredentialBlob && cred->CredentialBlobSize ? xstrndup((const char *)cred->CredentialBlob, cred->CredentialBlobSize) : NULL;
    CredFree(cred);
    return key;
}
bool meeting_key_save(MeetingKey which, const char *key) {
    CREDENTIALW cred; memset(&cred, 0, sizeof cred);
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR)KEY_TARGETS[which];
    cred.CredentialBlobSize = (DWORD)strlen(key);
    cred.CredentialBlob = (LPBYTE)key;
    // This device only, as the device token.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = (LPWSTR)KEY_USERS[which];
    cred.Comment = (LPWSTR)L"Briareus meeting assistant";
    return CredWriteW(&cred, 0) != 0;
}
bool meeting_key_remove(MeetingKey which) { return CredDeleteW(KEY_TARGETS[which], CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND; }
bool meeting_has_key(MeetingKey which) {
    char *key = meeting_key_read(which, NULL);
    bool has = key && *key;
    if (key) { SecureZeroMemory(key, strlen(key)); free(key); }
    return has;
}

// MARK: - Settings

#define SETTINGS_KEY L"Software\\Okanet\\Briareus\\Meeting"

static char *reg_string(const wchar_t *name) {
    wchar_t buffer[1024]; DWORD size = sizeof buffer, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_SZ, &type, buffer, &size) != ERROR_SUCCESS) return NULL;
    return wide_to_utf8(buffer);
}
static bool reg_bool(const wchar_t *name, bool fallback) {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) return fallback;
    return value != 0;
}
static void reg_set_string(HKEY key, const wchar_t *name, const char *value) {
    wchar_t *w = utf8_to_wide(value ? value : "");
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)w, (DWORD)((wcslen(w) + 1) * sizeof *w));
    free(w);
}
static void reg_set_bool(HKEY key, const wchar_t *name, bool value) { DWORD v = value; RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v); }

char *meeting_default_name(void) {
    wchar_t name[256]; DWORD n = 256;
    return GetUserNameW(name, &n) ? wide_to_utf8(name) : xstrdup("");
}
void meeting_settings_load(MeetingSettings *s) {
    memset(s, 0, sizeof *s);
    s->name = reg_string(L"name");
    if (!s->name || !*s->name) { free(s->name); s->name = meeting_default_name(); }
    s->wake_words = reg_string(L"wakeWords");
    if (!s->wake_words) s->wake_words = xstrfmt("%s, assistant, Briareus", s->name);
    s->eleven_voice = reg_string(L"elevenVoice");
    if (!s->eleven_voice) s->eleven_voice = xstrdup("");
    s->introduce = reg_bool(L"introduce", true);
    s->independent = reg_bool(L"independent", false);
}
void meeting_settings_save(const MeetingSettings *s) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    reg_set_string(key, L"name", s->name);
    reg_set_string(key, L"wakeWords", s->wake_words);
    reg_set_string(key, L"elevenVoice", s->eleven_voice);
    reg_set_bool(key, L"introduce", s->introduce);
    reg_set_bool(key, L"independent", s->independent);
    RegCloseKey(key);
}
void meeting_settings_free(MeetingSettings *s) { free(s->name); free(s->wake_words); free(s->eleven_voice); memset(s, 0, sizeof *s); }

// MARK: - History

static wchar_t *history_path(void) {
    wchar_t *base = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base) != S_OK) return NULL;
    wchar_t *path = xmalloc(MAX_PATH * sizeof *path);
    swprintf(path, MAX_PATH, L"%ls\\Okanet\\Briareus", base);
    SHCreateDirectoryExW(NULL, path, NULL);
    wcscat(path, L"\\meetings.jsonl");
    CoTaskMemFree(base);
    return path;
}
Json *meeting_history(void) {
    Json *all = json_array();
    wchar_t *path = history_path();
    FILE *f = path ? _wfopen(path, L"rb") : NULL;
    free(path);
    if (!f) return all;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        Json *j = json_parsez(line);
        if (json_is_object(j)) json_array_push(all, j); else json_free(j);
    }
    fclose(f);
    return all;
}
static void history_append(const MeetRecord *r) {
    wchar_t *path = history_path();
    FILE *f = path ? _wfopen(path, L"ab") : NULL;
    free(path);
    if (!f) return;
    Json *j = meet_record_json(r);
    char *text = json_serialize(j, false);
    fprintf(f, "%s\n", text);
    free(text); json_free(j);
    fclose(f);
}
void meeting_history_clear(void) { wchar_t *path = history_path(); if (path) DeleteFileW(path); free(path); }

// MARK: - The meeting

enum { WM_MEET_EVENT = WM_APP + 71, WM_MEET_ENDED };
enum { TIMER_TICK = 2 };
/// How long a board read may take, within the 120 s the agent waits for a tool.
#define BOARD_TIMEOUT_MS 110000

/// One tool call the agent made: its calls to the server, made together, and their answers as they come.
typedef struct {
    char *id; MeetTool tool; Json *args; DWORD asked;
    MeetCall calls[MEET_TOOL_MAX_CALLS]; size_t count, done;
    Json *answers[MEET_TOOL_MAX_CALLS];
    Request *reqs[MEET_TOOL_MAX_CALLS];
} Lookup;

static struct {
    MeetingState state;
    HWND hwnd;
    MeetPersona persona; MeetingSettings settings;
    char *repo, *title, *source, *error;
    WebSocket *ws;
    MeetAudio *audio;
    HANDLE reader, sender;
    volatile LONG sending;
    bool ready, muted;
    MeetLog log; CRITICAL_SECTION log_lock;
    char *log_repo;          // the project the log is about, kept after the meeting for its transcript
    CRITICAL_SECTION lock;   // the generation and the socket, between the connecting thread and the UI thread
    LONG gen;                // each meeting's own; messages and threads of an earlier one are ignored
    MeetRecord record;
    DWORD joined_tick;
    Lookup **lookups; size_t lookup_count;   // the tool calls under way
} M;

/// With BRIAREUS_MEET_LOG naming a file, every message to and from ElevenLabs is written there, cut short, to see what
/// it did. Off otherwise: the meeting's words stay out of files.
static FILE *g_log;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static void debug_open(void) {
    wchar_t path[MAX_PATH];
    if (g_log || !GetEnvironmentVariableW(L"BRIAREUS_MEET_LOG", path, MAX_PATH)) return;
    g_log = _wfopen(path, L"ab");
}
static void debug(const char *who, const char *text, size_t len) {
    if (!g_log) return;
    AcquireSRWLockExclusive(&g_log_lock);
    SYSTEMTIME t; GetLocalTime(&t);
    size_t shown = len > 300 ? 300 : len;
    fprintf(g_log, "%02d:%02d:%02d.%03d %s %.*s%s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, who, (int)shown, text ? text : "", len > shown ? "\xE2\x80\xA6" : "");
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_log_lock);
}
static void debugz(const char *who, const char *text) { debug(who, text, text ? strlen(text) : 0); }

static void post(LONG gen, UINT msg, void *payload) { if (!M.hwnd || !PostMessageW(M.hwnd, msg, (WPARAM)gen, (LPARAM)payload)) free(payload); }
static void changed(void) { Pane *p = app_detail_pane(); if (p) pane_header_changed(p); }
static void send_on(WebSocket *ws, char *event) {
    if (!event) return;
    if (!strstr(event, "user_audio_chunk")) debugz("send", event);
    ws_send(ws, event);
    free(event);
}
static void send_owned(char *event) { send_on(M.ws, event); }

/// A whole line of the transcript, shown at once in the project's Meeting tab.
static void log_line(int speaker, const char *text) {
    EnterCriticalSection(&M.log_lock);
    meet_log_line(&M.log, speaker, text);
    LeaveCriticalSection(&M.log_lock);
    Pane *p = app_detail_pane();
    if (p) pane_relayout(p);
}
static char *log_tail(size_t max) {
    EnterCriticalSection(&M.log_lock);
    char *t = meet_log_tail(&M.log, max);
    LeaveCriticalSection(&M.log_lock);
    return t;
}

// MARK: - The agent in ElevenLabs

/// One call to ElevenLabs' REST API; the answer's JSON, or NULL with `*error` saying why.
static Json *eleven_call(const char *method, const char *path, const char *body, const char *key, int *status, char **error) {
    char *url = xstrfmt("https://%s%s", ELEVEN_HOST, path);
    const char *headers[] = { "xi-api-key", key, "Content-Type", "application/json", "Accept", "application/json", NULL, NULL };
    char *type = NULL, *retry = NULL, *response = NULL, *why = NULL; size_t len = 0;
    *status = 0; *error = NULL;
    bool got = api_winhttp_transport(NULL, method, url, headers, body, body ? strlen(body) : 0, 30000, status, &type, &retry, &response, &len, &why);
    char *line = xstrfmt("%s %s -> %d", method, path, *status);
    debugz("rest", line); free(line);
    free(url); free(type); free(retry);
    Json *answer = got && response ? json_parse(response, len) : NULL;
    if (!got) *error = xstrfmt("ElevenLabs could not be reached: %s", why ? why : "no answer.");
    else if (*status < 200 || *status >= 300) {
        // ElevenLabs says why in detail.message or detail.
        const Json *detail = json_get(answer, "detail");
        const char *message = json_str_nonempty(json_get(detail, "message"));
        if (!message) message = json_str_nonempty(detail);
        if (!message && json_is_array(detail)) message = json_str_nonempty(json_get(json_at(detail, 0), "msg"));
        *error = *status == 401 ? xstrdup("ElevenLabs refused the API key.")
               : xstrfmt("ElevenLabs answered HTTP %d%s%s", *status, message ? ": " : ".", message ? message : "");
        json_free(answer); answer = NULL;
    }
    free(response); free(why);
    return answer;
}

/// FNV-1a over the tools' bodies: a change of tools updates them in ElevenLabs once.
static char *tools_version(char **bodies) {
    unsigned long long h = 1469598103934665603ULL;
    for (int t = 0; t < MEET_TOOL_COUNT; t++) for (const unsigned char *c = (const unsigned char *)bodies[t]; *c; c++) { h ^= *c; h *= 1099511628211ULL; }
    return xstrfmt("%016llx", h);
}
static void reg_save(const wchar_t *name, const char *value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    reg_set_string(key, name, value);
    RegCloseKey(key);
}

/// Creates or brings up to date the project's tools and the agent in the user's workspace; the agent's id, or NULL with
/// `*error`. The ids are kept in the registry, so a meeting usually updates only the agent.
static char *agent_ready(const char *key, const MeetPersona *p, char **error) {
    char *bodies[MEET_TOOL_COUNT], *ids[MEET_TOOL_COUNT];
    for (int t = 0; t < MEET_TOOL_COUNT; t++) { bodies[t] = meet_tool_body((MeetTool)t); ids[t] = NULL; }
    char *version = tools_version(bodies), *saved_version = reg_string(L"elevenToolsVersion"), *saved = reg_string(L"elevenTools");
    Json *saved_ids = saved ? json_parsez(saved) : NULL;
    free(saved);
    bool current = str_eq(version, saved_version);
    char *agent_id = NULL;
    int status;
    for (int t = 0; t < MEET_TOOL_COUNT && !*error; t++) {
        const char *id = json_str_nonempty(json_get(saved_ids, meet_tool_name((MeetTool)t)));
        if (id && current) { ids[t] = xstrdup(id); continue; }
        Json *answer = NULL;
        if (id) {
            char *path = xstrfmt("/v1/convai/tools/%s", id);
            answer = eleven_call("PATCH", path, bodies[t], key, &status, error);
            free(path);
            if (answer) ids[t] = xstrdup(id);
            // A tool deleted in ElevenLabs is made again.
            else if (status == 404) { free(*error); *error = NULL; }
        }
        if (!ids[t] && !*error) {
            answer = eleven_call("POST", "/v1/convai/tools", bodies[t], key, &status, error);
            ids[t] = answer ? meet_created_id(answer) : NULL;
            if (answer && !ids[t]) *error = xstrdup("ElevenLabs did not say the new tool's id.");
        }
        json_free(answer);
    }
    if (!*error) {
        Json *map = json_object();
        for (int t = 0; t < MEET_TOOL_COUNT; t++) json_set_str(map, meet_tool_name((MeetTool)t), ids[t]);
        char *text = json_serialize(map, false);
        reg_save(L"elevenTools", text); reg_save(L"elevenToolsVersion", version);
        free(text); json_free(map);
        char *body = meet_agent_body(p, ids, MEET_TOOL_COUNT), *saved_agent = reg_string(L"elevenAgent");
        if (saved_agent && *saved_agent) {
            char *path = xstrfmt("/v1/convai/agents/%s", saved_agent);
            Json *answer = eleven_call("PATCH", path, body, key, &status, error);
            free(path);
            if (answer) agent_id = xstrdup(saved_agent);
            else if (status == 404) { free(*error); *error = NULL; }
            json_free(answer);
        }
        if (!agent_id && !*error) {
            Json *answer = eleven_call("POST", "/v1/convai/agents/create", body, key, &status, error);
            agent_id = answer ? meet_created_id(answer) : NULL;
            if (answer && !agent_id) *error = xstrdup("ElevenLabs did not say the new agent's id.");
            if (agent_id) reg_save(L"elevenAgent", agent_id);
            json_free(answer);
        }
        free(body); free(saved_agent);
    }
    for (int t = 0; t < MEET_TOOL_COUNT; t++) { free(bodies[t]); free(ids[t]); }
    free(version); free(saved_version); json_free(saved_ids);
    return agent_id;
}

/// The socket's reader: speech goes straight to the cable and pings are answered here, everything else goes to the UI
/// thread. The UI thread joins it before the devices close.
static void read_events(WebSocket *ws, LONG gen) {
    for (;;) {
        char *text; size_t len;
        int got = ws_receive(ws, &text, &len);
        if (got <= 0) break;
        MeetEvent *e = xcalloc(1, sizeof *e);
        bool ok = meet_event_parse(text, len, e);
        if (ok && e->kind != MEET_EV_AUDIO && e->kind != MEET_EV_PING) debug("recv", text, len);
        free(text);
        if (!ok) { free(e); continue; }
        if (e->kind == MEET_EV_AUDIO) { meet_audio_play(M.audio, (const int16_t *)e->audio, e->audio_len / 2); meet_event_free(e); free(e); continue; }
        if (e->kind == MEET_EV_PING) { send_on(ws, meet_pong_event(e->event_id)); meet_event_free(e); free(e); continue; }
        if (e->kind == MEET_EV_INTERRUPTED) meet_audio_flush(M.audio);
        post(gen, WM_MEET_EVENT, e);
    }
    post(gen, WM_MEET_ENDED, NULL);
}
/// The meeting's sender: what the assistant hears, at the pace it was heard, every 100 ms.
static unsigned __stdcall sender_main(void *arg) {
    (void)arg;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    unsigned long long sent = 0;
    int16_t *pcm = xmalloc(MEET_SAMPLE_RATE * sizeof *pcm);
    while (M.sending) {
        Sleep(100);
        QueryPerformanceCounter(&now);
        unsigned long long due = (unsigned long long)((now.QuadPart - start.QuadPart) * MEET_SAMPLE_RATE / freq.QuadPart);
        size_t n = due > sent ? (size_t)(due - sent) : 0;
        if (n > MEET_SAMPLE_RATE) n = MEET_SAMPLE_RATE;
        if (!n) continue;
        meet_audio_take_input(M.audio, pcm, n);
        sent += n;
        char *event = meet_audio_event(pcm, n);
        bool ok = ws_send(M.ws, event);
        free(event);
        if (!ok) break;
    }
    free(pcm);
    return 0;
}

/// Readies the agent, connects on a thread of its own, then reads. A meeting left meanwhile is not touched again.
/// The thread owns its copies: the meeting may finish while it still readies the agent.
typedef struct { char *key, *name, *wake_words, *project, *voice; bool introduce, independent; LONG gen; } Connect;
static unsigned __stdcall connect_main(void *arg) {
    Connect *c = arg;
    LONG gen = c->gen;
    char *error = NULL;
    MeetPersona persona = { c->name, c->wake_words, c->project, c->voice, c->introduce, c->independent };
    char *agent = agent_ready(c->key, &persona, &error);
    WebSocket *ws = NULL;
    if (agent) {
        // A private agent's conversation opens on a signed URL; should that fail, on the key in a header.
        int status;
        char *query = xstrfmt("/v1/convai/conversation/get-signed-url?agent_id=%s", agent), *signing_error = NULL;
        Json *signed_answer = eleven_call("GET", query, NULL, c->key, &status, &signing_error);
        char *path = meet_signed_ws_path(signed_answer);
        bool signed_url = path != NULL;
        if (!path) path = meet_agent_ws_path(agent);
        json_free(signed_answer); free(query); free(signing_error);
        const char *headers[] = { signed_url ? NULL : "xi-api-key", c->key, NULL, NULL };
        char *why = NULL;
        ws = ws_connect(ELEVEN_HOST, path, headers, &why);
        debugz("connect", ws ? "open" : why ? why : "failed");
        if (!ws) error = xstrfmt("ElevenLabs: %s", why ? why : "could not connect.");
        free(why); free(path); free(agent);
    }
    SecureZeroMemory(c->key, strlen(c->key)); free(c->key);
    free(c->name); free(c->wake_words); free(c->project); free(c->voice); free(c);
    if (!ws) { post(gen, WM_MEET_ENDED, error ? error : xstrdup("The agent could not be readied.")); return 0; }
    EnterCriticalSection(&M.lock);
    bool current = gen == M.gen && M.state == MEETING_CONNECTING;
    if (current) M.ws = ws;
    LeaveCriticalSection(&M.lock);
    if (!current) { ws_free(ws); return 0; }
    send_on(ws, meet_start_event());
    read_events(ws, gen);
    return 0;
}

// MARK: - The project's tools

static void lookup_free(Lookup *l) {
    for (size_t i = 0; i < l->count; i++) { request_cancel(&l->reqs[i]); json_free(l->answers[i]); }
    meet_calls_free(l->calls, l->count);
    json_free(l->args); free(l->id); free(l);
}
static void lookup_answer(Lookup *l, char *output) {
    send_owned(meet_tool_result_event(l->id, output, strstr(output, "\"error\"") == output + 1));
    free(output);
    M.record.answers++;
    M.record.answer_seconds += (GetTickCount() - l->asked) / 1000.0;
    for (size_t i = 0; i < M.lookup_count; i++) if (M.lookups[i] == l) { memmove(M.lookups + i, M.lookups + i + 1, (M.lookup_count - i - 1) * sizeof *M.lookups); M.lookup_count--; break; }
    lookup_free(l);
    changed();
}
static void lookup_done(void *owner, Request *req) {
    Lookup *l = owner;
    size_t i = (size_t)req->tag;
    if (M.state == MEETING_OFF || i >= l->count) return;
    // Only the first call's answer is needed; a later one that failed is left out.
    if (req->ok) {
        l->answers[i] = json_clone(req->result);
        // The board read for a meeting is the project screen's too.
        if (str_eq(req->operation, "pulls")) { char *key = xstrfmt("pulls:%s", M.repo); cache_store(g_store.cache, req->result, key); free(key); }
    }
    else if (i == 0) {
        char *why = request_error_text(req);
        lookup_answer(l, meet_tool_error(why));
        free(why);
        return;
    }
    if (++l->done < l->count) return;
    lookup_answer(l, meet_tool_summary(l->tool, l->args, M.repo, (const Json *const *)l->answers, l->count));
}
static void on_tool(MeetEvent *e) {
    M.record.requests++;
    Lookup *l = xcalloc(1, sizeof *l);
    l->id = e->id; e->id = NULL;
    l->asked = GetTickCount();
    l->args = json_parsez(e->request ? e->request : "{}");
    if (!l->args) l->args = json_object();
    M.lookups = xrealloc(M.lookups, (M.lookup_count + 1) * sizeof *M.lookups);
    M.lookups[M.lookup_count++] = l;
    char *refusal = NULL;
    log_line(MEET_SPEAKER_LOOKUP, e->text);
    if (!meet_tool_find(e->text, &l->tool)) { lookup_answer(l, meet_tool_error("No such tool: only the project's read-only tools are offered.")); return; }
    l->count = meet_tool_calls(l->tool, l->args, M.repo, l->calls, &refusal);
    if (!l->count) { lookup_answer(l, refusal ? refusal : meet_tool_error("Nothing to read.")); return; }
    if (!store_supports(l->calls[0].op)) { lookup_answer(l, meet_tool_error("This device's token cannot read that on the server.")); return; }
    for (size_t i = 0; i < l->count; i++) {
        // The call's arguments go to the request; the plan keeps none.
        Json *args = l->calls[i].args; l->calls[i].args = NULL;
        if (!store_supports(l->calls[i].op)) { json_free(args); l->done++; continue; }
        // A large project's board takes GitHub a while to read: the one the project screen last synced answers at once.
        bool board = str_eq(l->calls[i].op, "pulls");
        if (board) {
            char *key = xstrfmt("pulls:%s", M.repo);
            l->answers[i] = cache_value(g_store.cache, key);
            free(key);
            if (l->answers[i]) { json_free(args); l->done++; continue; }
        }
        store_call(l->calls[i].op, args, board ? BOARD_TIMEOUT_MS : 0, l, lookup_done, (int)i, &l->reqs[i]);
    }
    if (l->done == l->count) { lookup_answer(l, meet_tool_summary(l->tool, l->args, M.repo, (const Json *const *)l->answers, l->count)); return; }
    changed();
}

// MARK: - Events on the UI thread

static void finish(const char *error);
static void on_ready(void) {
    if (M.ready || M.state != MEETING_CONNECTING) return;
    M.ready = true;
    M.state = MEETING_LIVE;
    M.joined_tick = GetTickCount();
    M.sending = 1;
    M.sender = (HANDLE)_beginthreadex(NULL, 0, sender_main, NULL, 0, NULL);
    SetTimer(M.hwnd, TIMER_TICK, 1000, NULL);
    changed();
}
static void on_event(MeetEvent *e) {
    switch (e->kind) {
    case MEET_EV_READY: on_ready(); break;
    case MEET_EV_HEARD_TURN: log_line(MEET_SPEAKER_MEETING, e->text); break;
    case MEET_EV_SAID: log_line(MEET_SPEAKER_ASSISTANT, e->text); break;
    case MEET_EV_TOOL: on_tool(e); break;
    case MEET_EV_ERROR:
        // Before the conversation started an error ends it; later ones only show.
        if (!M.ready) finish(e->text);
        else set_string(&M.error, e->text);
        break;
    default: break;
    }
    changed();
}

static LRESULT CALLBACK meeting_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool current = (LONG)wp == M.gen && M.state != MEETING_OFF;
    switch (msg) {
    case WM_MEET_EVENT: {
        MeetEvent *e = (MeetEvent *)lp;
        if (current) on_event(e);
        meet_event_free(e); free(e);
        return 0;
    }
    case WM_MEET_ENDED: {
        char *error = (char *)lp;
        if (current) finish(M.state == MEETING_LEAVING ? NULL : error ? error : "ElevenLabs ended the conversation.");
        free(error);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TICK) changed();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void ensure_window(void) {
    if (M.hwnd) return;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = meeting_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"BriareusMeeting";
    RegisterClassW(&wc);
    M.hwnd = CreateWindowExW(0, L"BriareusMeeting", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    InitializeCriticalSection(&M.log_lock);
    InitializeCriticalSection(&M.lock);
}

/// Ends the meeting: threads joined, devices closed, the record saved when it got going, and `error` shown.
static void finish(const char *error) {
    if (M.state == MEETING_OFF) return;
    bool was_live = M.ready;
    EnterCriticalSection(&M.lock);
    M.state = MEETING_OFF;
    M.gen++;
    WebSocket *ws = M.ws;
    M.ws = NULL;
    LeaveCriticalSection(&M.lock);
    KillTimer(M.hwnd, TIMER_TICK);
    InterlockedExchange(&M.sending, 0);
    if (M.sender) { WaitForSingleObject(M.sender, INFINITE); CloseHandle(M.sender); M.sender = NULL; }
    // A connected reader ends as its socket is cut, and is waited for, since it plays into the devices; one still
    // connecting finds the meeting gone and closes its own socket.
    if (ws) ws_abort(ws);
    if (M.reader) { if (ws) WaitForSingleObject(M.reader, INFINITE); CloseHandle(M.reader); M.reader = NULL; }
    ws_free(ws);
    meet_audio_stop(M.audio); M.audio = NULL;
    for (size_t i = 0; i < M.lookup_count; i++) lookup_free(M.lookups[i]);
    free(M.lookups); M.lookups = NULL; M.lookup_count = 0;
    if (was_live) {
        M.record.seconds = M.record.usage.seconds = (GetTickCount() - M.joined_tick) / 1000.0;
        history_append(&M.record);
    }
    free(M.repo); M.repo = NULL; free(M.title); M.title = NULL; free(M.source); M.source = NULL;
    free(M.error); M.error = NULL;
    meeting_settings_free(&M.settings);
    memset(&M.persona, 0, sizeof M.persona);
    changed();
    if (error) {
        char *message = was_live ? xstrfmt("The meeting ended: %s", error) : xstrdup(error);
        app_alert("Meeting assistant", message);
        free(message);
    }
}

bool meeting_join(const Project *project, unsigned pid, const char *source) {
    if (M.state != MEETING_OFF) { app_alert("Meeting assistant", "A meeting is already running. Leave it first."); return false; }
    ensure_window();
    debug_open();
    meeting_settings_load(&M.settings);
    if (!*M.settings.eleven_voice) {
        meeting_settings_free(&M.settings);
        app_alert("Meeting assistant", "Add your ElevenLabs voice ID first: \xE2\x9A\x99 Settings \xE2\x86\x92 Meeting assistant.");
        return false;
    }
    bool failed = false;
    char *key = meeting_key_read(MEETING_KEY_ELEVENLABS, &failed);
    if (!key || !*key) {
        free(key); meeting_settings_free(&M.settings);
        app_alert("Meeting assistant", failed ? "The ElevenLabs API key could not be read from Credential Manager."
                                              : "Add your ElevenLabs API key first: \xE2\x9A\x99 Settings \xE2\x86\x92 Meeting assistant.");
        return false;
    }
    char *error = NULL, *note = NULL;
    MeetAudio *audio = meet_audio_start(pid, &error, &note);
    if (!audio) {
        app_alert("Meeting assistant", error);
        free(error); SecureZeroMemory(key, strlen(key)); free(key); meeting_settings_free(&M.settings);
        return false;
    }
    memset(&M.record, 0, sizeof M.record);
    M.record.model = MEET_AGENT; M.record.started = (double)time(NULL);
    M.audio = audio; M.muted = false; M.ready = false;
    M.repo = xstrdup(project->repo);
    const char *title = project_title(project);
    M.title = str_eq(title, project->repo) ? xstrdup(title) : xstrfmt("%s (%s)", title, project->repo);
    M.source = xstrdup(source ? source : "every app");
    M.persona = (MeetPersona){ M.settings.name, M.settings.wake_words, M.title, M.settings.eleven_voice, M.settings.introduce, M.settings.independent };
    EnterCriticalSection(&M.log_lock);
    meet_log_free(&M.log); meet_log_init(&M.log);
    set_string(&M.log_repo, project->repo);
    LeaveCriticalSection(&M.log_lock);
    M.state = MEETING_CONNECTING;
    set_string(&M.error, note);
    free(note);
    Connect *c = xcalloc(1, sizeof *c);
    c->key = key; c->gen = M.gen;
    c->name = xstrdup(M.settings.name); c->wake_words = xstrdup(M.settings.wake_words); c->project = xstrdup(M.title);
    c->voice = xstrdup(M.settings.eleven_voice); c->introduce = M.settings.introduce; c->independent = M.settings.independent;
    M.reader = (HANDLE)_beginthreadex(NULL, 0, connect_main, c, 0, NULL);
    changed();
    return true;
}

void meeting_leave(void) { if (M.state != MEETING_OFF) finish(NULL); }
void meeting_shutdown(void) { if (M.state != MEETING_OFF) finish(NULL); }

MeetingState meeting_state(void) { return M.state; }
bool meeting_for(const char *repo) { return M.state != MEETING_OFF && str_eq(M.repo, repo); }
void meeting_set_muted(bool muted) { M.muted = muted; if (M.audio) meet_audio_set_muted(M.audio, muted); changed(); }
bool meeting_muted(void) { return M.muted; }
void meeting_answer_now(void) { if (M.state == MEETING_LIVE) send_owned(meet_answer_now_event()); }

char *meeting_status(void) {
    if (M.state == MEETING_OFF) return xstrdup("");
    Str s; str_init(&s);
    str_appendf(&s, "\xF0\x9F\x8E\x99 %s \xC2\xB7 %s", meet_model_label(MEET_AGENT), M.source);
    if (M.state == MEETING_CONNECTING) str_appendz(&s, " \xC2\xB7 connecting\xE2\x80\xA6");
    else {
        unsigned secs = (GetTickCount() - M.joined_tick) / 1000;
        MeetUsage usage = { .seconds = secs };
        str_appendf(&s, " \xC2\xB7 %u:%02u \xC2\xB7 $%.3f", secs / 60, secs % 60, meet_usage_cost(MEET_AGENT, &usage));
        str_appendz(&s, M.muted ? " \xC2\xB7 muted" : M.lookup_count ? " \xC2\xB7 looking it up\xE2\x80\xA6" : M.audio && meet_audio_speaking(M.audio) ? " \xC2\xB7 speaking" : " \xC2\xB7 listening");
    }
    if (M.error) str_appendf(&s, " \xC2\xB7 %s", M.error);
    return str_detach(&s);
}
char *meeting_transcript(void) { return M.hwnd ? log_tail(1 << 20) : xstrdup(""); }
char *meeting_transcript_for(const char *repo) {
    if (!M.hwnd) return NULL;
    EnterCriticalSection(&M.log_lock);
    char *t = str_eq(M.log_repo, repo) ? meet_log_tail(&M.log, 1 << 20) : NULL;
    LeaveCriticalSection(&M.log_lock);
    if (t && !*t && !meeting_for(repo)) { free(t); t = NULL; }
    return t;
}

// MARK: - The menu

enum { MEET_ITEM_JOIN = 100, MEET_ITEM_MUTE = 900, MEET_ITEM_ANSWER, MEET_ITEM_COPY, MEET_ITEM_LEAVE, MEET_ITEM_SETTINGS };
static void append_menu(HMENU menu, UINT flags, UINT_PTR id, const char *text) { wchar_t *w = utf8_to_wide(text); AppendMenuW(menu, flags, id, w); free(w); }
static void copy_text(HWND owner, const char *text) {
    wchar_t *w = utf8_to_wide(text);
    size_t bytes = (wcslen(w) + 1) * sizeof *w;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h && OpenClipboard(owner)) {
        void *dst = GlobalLock(h);
        if (dst) { memcpy(dst, w, bytes); GlobalUnlock(h); }
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h)) h = NULL;
        CloseClipboard();
    }
    if (h) GlobalFree(h);
    free(w);
}
bool meeting_menu(const Project *project, HWND owner, POINT pt) {
    HMENU menu = CreatePopupMenu();
    MeetApp *apps = NULL; size_t app_count = 0;
    if (M.state != MEETING_OFF) {
        char *status = meeting_status();
        append_menu(menu, MF_STRING | MF_GRAYED, 0, meeting_for(project->repo) ? status : "A meeting about another project is running");
        free(status);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        bool live = M.state == MEETING_LIVE;
        append_menu(menu, MF_STRING | (M.muted ? MF_CHECKED : 0) | (live ? 0 : MF_GRAYED), MEET_ITEM_MUTE, "Mute the assistant");
        append_menu(menu, MF_STRING | (live && !M.muted ? 0 : MF_GRAYED), MEET_ITEM_ANSWER, "Answer now");
        append_menu(menu, MF_STRING, MEET_ITEM_COPY, "Copy the meeting transcript");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        append_menu(menu, MF_STRING, MEET_ITEM_LEAVE, "Leave the meeting");
    } else {
        apps = meet_apps_running(&app_count);
        append_menu(menu, MF_STRING | MF_GRAYED, 0, "Join, listening to");
        for (size_t i = 0; i < app_count && i < 50; i++) append_menu(menu, MF_STRING, MEET_ITEM_JOIN + 1 + i, apps[i].label);
        append_menu(menu, MF_STRING, MEET_ITEM_JOIN, "Every app except Briareus");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    append_menu(menu, MF_STRING, MEET_ITEM_SETTINGS, "Meeting assistant settings\xE2\x80\xA6");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, owner, NULL);
    DestroyMenu(menu);
    switch (chosen) {
    case 0: break;
    case MEET_ITEM_MUTE: meeting_set_muted(!M.muted); break;
    case MEET_ITEM_ANSWER: meeting_answer_now(); break;
    case MEET_ITEM_COPY: { char *t = meeting_transcript(); copy_text(owner, t); free(t); break; }
    case MEET_ITEM_LEAVE: meeting_leave(); break;
    case MEET_ITEM_SETTINGS: app_show_detail(meeting_settings_screen_new()); break;
    default:
        if (chosen >= MEET_ITEM_JOIN && chosen <= MEET_ITEM_JOIN + 50) {
            int i = chosen - MEET_ITEM_JOIN - 1;
            bool app = i >= 0 && (size_t)i < app_count;
            meeting_join(project, app ? apps[i].pid : 0, app ? apps[i].label : "every app");
        }
    }
    meet_apps_free(apps, app_count);
    return chosen != 0;
}
