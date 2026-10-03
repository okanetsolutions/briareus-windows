#include "meeting.h"
#include "meet_tools.h"
#include "meet_audio.h"
#include "screens.h"
#include "str.h"
#include "ws.h"
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
    s->voice = reg_string(L"voice");
    if (!s->voice || !*s->voice) { free(s->voice); s->voice = xstrdup("marin"); }
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
    reg_set_string(key, L"voice", s->voice);
    reg_set_string(key, L"elevenVoice", s->eleven_voice);
    reg_set_bool(key, L"introduce", s->introduce);
    reg_set_bool(key, L"independent", s->independent);
    RegCloseKey(key);
}
void meeting_settings_free(MeetingSettings *s) { free(s->name); free(s->wake_words); free(s->voice); free(s->eleven_voice); memset(s, 0, sizeof *s); }

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

enum { WM_MEET_EVENT = WM_APP + 71, WM_MEET_READY, WM_MEET_ENDED, WM_VOICE_ENDED };
enum { TIMER_TICK = 2 };
/// ElevenLabs closes a socket after 20 s without a message.
#define KEEP_ALIVE_SECONDS 10

/// One tool call the model made: its calls to the server, made together, and their answers as they come.
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
    CRITICAL_SECTION lock;   // the generation and the socket, between the connecting thread and the UI thread
    LONG gen;                // each meeting's own; messages and threads of an earlier one are ignored
    MeetRecord record;
    DWORD joined_tick;
    unsigned ticks;
    Lookup **lookups; size_t lookup_count;   // the tool calls under way
} M;

static void post(LONG gen, UINT msg, void *payload) { if (!M.hwnd || !PostMessageW(M.hwnd, msg, (WPARAM)gen, (LPARAM)payload)) free(payload); }
static void changed(void) { Pane *p = app_detail_pane(); if (p) pane_header_changed(p); }
static void send_owned(char *event) { if (event) { ws_send(M.ws, event); free(event); } }

static void log_add(int speaker, const char *text) {
    EnterCriticalSection(&M.log_lock);
    meet_log_add(&M.log, speaker, text);
    LeaveCriticalSection(&M.log_lock);
}
static char *log_tail(size_t max) {
    EnterCriticalSection(&M.log_lock);
    char *t = meet_log_tail(&M.log, max);
    LeaveCriticalSection(&M.log_lock);
    return t;
}

// MARK: - The ElevenLabs voice

/// GPT-Realtime's text answers, said in the user's ElevenLabs voice. One socket at a time: ElevenLabs cannot drop text
/// it was given, so someone talking over the assistant cuts the socket and a new one opens. Each socket has a thread that
/// connects and reads; once its socket is no longer the current one, it touches nothing but its own.
static struct {
    CRITICAL_SECTION lock;
    bool on;                 // this meeting speaks with ElevenLabs
    char *key, *voice;
    LONG gen;                // the meeting's
    LONG conn;               // the current socket's number
    WebSocket *ws;           // the current socket, once open
    bool connecting;
    Str pending;             // text to say before the socket opened
    bool pending_turn, pending_flush;
    bool turn;               // the next text starts an answer
    bool used;               // the socket was given text, so it may still be speaking
    bool skip;               // the answer was talked over: the rest of it is not said
    double chars;            // said this meeting
} V;

typedef struct { LONG conn, gen; char *key, *voice; } VoiceConnect;
static void voice_send_locked(char *event) { if (event) { ws_send(V.ws, event); free(event); } }

static unsigned __stdcall voice_main(void *arg) {
    VoiceConnect *c = arg;
    const char *headers[] = { "xi-api-key", c->key, NULL, NULL };
    char *error = NULL;
    WebSocket *ws = ws_connect(ELEVEN_HOST, eleven_ws_path(), headers, &error);
    SecureZeroMemory(c->key, strlen(c->key)); free(c->key);
    EnterCriticalSection(&V.lock);
    bool current = c->conn == V.conn;
    if (current) {
        V.connecting = false;
        if (ws) {
            V.ws = ws;
            voice_send_locked(eleven_open_event(c->voice));
            if (V.pending.len) { voice_send_locked(eleven_text_event(c->voice, V.pending.data, V.pending_turn)); V.used = true; }
            if (V.pending_flush) voice_send_locked(eleven_flush_event());
            str_free(&V.pending); V.pending_turn = V.pending_flush = false;
        }
    }
    LeaveCriticalSection(&V.lock);
    free(c->voice);
    if (!ws || !current) {
        ws_free(ws);
        if (current) { char *why = xstrfmt("ElevenLabs: %s", error ? error : "could not connect."); post(c->gen, WM_VOICE_ENDED, why); }
        free(error); free(c);
        return 0;
    }
    for (;;) {
        char *text; size_t len;
        if (ws_receive(ws, &text, &len) <= 0) break;
        MeetEvent *e = xcalloc(1, sizeof *e);
        bool ok = eleven_event_parse(text, len, e);
        free(text);
        if (ok && e->kind == MEET_EV_AUDIO) {
            EnterCriticalSection(&V.lock);
            if (c->conn == V.conn) meet_audio_play(M.audio, (const int16_t *)e->audio, e->audio_len / 2);
            LeaveCriticalSection(&V.lock);
        } else if (ok && e->kind == MEET_EV_ERROR) { post(c->gen, WM_MEET_EVENT, e); continue; }
        meet_event_free(e); free(e);
    }
    // Cut by an interruption or the meeting's end, the socket is no longer current; otherwise ElevenLabs closed it.
    EnterCriticalSection(&V.lock);
    bool mine = V.ws == ws;
    if (mine) { V.ws = NULL; V.used = false; }
    LeaveCriticalSection(&V.lock);
    ws_free(ws);
    if (mine) post(c->gen, WM_VOICE_ENDED, NULL);
    free(c);
    return 0;
}
static void voice_open_locked(void) {
    VoiceConnect *c = xcalloc(1, sizeof *c);
    c->conn = ++V.conn; c->gen = V.gen; c->key = xstrdup(V.key); c->voice = xstrdup(V.voice);
    V.connecting = true; V.used = false;
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, voice_main, c, 0, NULL);
    if (thread) CloseHandle(thread);
    else { V.connecting = false; SecureZeroMemory(c->key, strlen(c->key)); free(c->key); free(c->voice); free(c); }
}
/// Cuts the current socket, unsaid speech and all.
static void voice_cut_locked(void) {
    V.conn++;
    if (V.ws) { ws_abort(V.ws); V.ws = NULL; }
    V.connecting = V.used = false;
    str_free(&V.pending); V.pending_turn = V.pending_flush = false;
}

static void voice_start(char *key, const char *voice, LONG gen) {
    EnterCriticalSection(&V.lock);
    V.on = true; V.key = key; V.voice = xstrdup(voice); V.gen = gen;
    V.turn = true; V.skip = false; V.chars = 0;
    voice_open_locked();
    LeaveCriticalSection(&V.lock);
}
static void voice_stop(void) {
    EnterCriticalSection(&V.lock);
    if (V.on) {
        voice_cut_locked();
        V.on = false;
        SecureZeroMemory(V.key, strlen(V.key)); free(V.key); V.key = NULL;
        free(V.voice); V.voice = NULL;
    }
    LeaveCriticalSection(&V.lock);
}
/// Part of an answer, said as it comes.
static void voice_say(const char *text) {
    if (!text || !*text) return;
    EnterCriticalSection(&V.lock);
    if (V.on && !V.skip) {
        bool turn = V.turn;
        V.turn = false;
        V.chars += (double)meet_char_count(text);
        if (V.ws) { voice_send_locked(eleven_text_event(V.voice, text, turn)); V.used = true; }
        else {
            if (!V.pending.len) V.pending_turn = turn;
            str_appendz(&V.pending, text);
            if (!V.connecting) voice_open_locked();
        }
    }
    LeaveCriticalSection(&V.lock);
}
/// The answer is whole: what ElevenLabs holds back for context is said now.
static void voice_answer_done(void) {
    EnterCriticalSection(&V.lock);
    if (V.on && !V.skip) { if (V.ws) voice_send_locked(eleven_flush_event()); else if (V.pending.len) V.pending_flush = true; }
    V.turn = true;
    LeaveCriticalSection(&V.lock);
}
static void voice_answer_started(void) {
    EnterCriticalSection(&V.lock);
    V.skip = false; V.turn = true;
    LeaveCriticalSection(&V.lock);
}
/// Someone talks: the answer under way stops, and a socket that was given text is replaced by a fresh one.
static void voice_interrupt(void) {
    EnterCriticalSection(&V.lock);
    if (V.on) {
        V.skip = true;
        if (V.used || V.pending.len) { voice_cut_locked(); voice_open_locked(); }
    }
    LeaveCriticalSection(&V.lock);
}
static void voice_keep_alive(void) {
    EnterCriticalSection(&V.lock);
    if (V.ws) voice_send_locked(eleven_keep_alive_event());
    LeaveCriticalSection(&V.lock);
}
static double voice_chars(void) {
    EnterCriticalSection(&V.lock);
    double n = V.chars;
    LeaveCriticalSection(&V.lock);
    return n;
}

/// The socket's reader: speech goes straight to the cable (text to ElevenLabs, when it speaks), everything else to the
/// UI thread. The UI thread joins it before the devices close.
static void read_events(WebSocket *ws, LONG gen) {
    for (;;) {
        char *text; size_t len;
        int got = ws_receive(ws, &text, &len);
        if (got <= 0) break;
        MeetEvent *e = xcalloc(1, sizeof *e);
        bool ok = meet_event_parse(text, len, e);
        free(text);
        if (!ok) { free(e); continue; }
        if (e->kind == MEET_EV_AUDIO) {
            meet_audio_play(M.audio, (const int16_t *)e->audio, e->audio_len / 2);
            if (!e->has_usage) { meet_event_free(e); free(e); continue; }
        }
        if (e->kind == MEET_EV_SPEECH_STARTED) { meet_audio_flush(M.audio); voice_interrupt(); }
        else if (e->kind == MEET_EV_RESPONSE) voice_answer_started();
        else if (e->kind == MEET_EV_SAID) voice_say(e->text);
        else if (e->kind == MEET_EV_SAID_DONE) voice_answer_done();
        post(gen, WM_MEET_EVENT, e);
    }
    post(gen, WM_MEET_ENDED, NULL);
}
/// The meeting's sender: what the assistant hears, at the pace it was heard, every 40 ms.
static unsigned __stdcall sender_main(void *arg) {
    (void)arg;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    unsigned long long sent = 0;
    int16_t *pcm = xmalloc(MEET_SAMPLE_RATE * sizeof *pcm);
    while (M.sending) {
        Sleep(40);
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

/// Connects on a thread of its own, then reads. A meeting left while it connected is not touched again.
typedef struct { char *key, *setup; LONG gen; } Connect;
static unsigned __stdcall connect_main(void *arg) {
    Connect *c = arg;
    LONG gen = c->gen;
    char *auth = xstrfmt("Bearer %s", c->key);
    const char *headers[] = { "Authorization", auth, NULL, NULL };
    char *error = NULL;
    WebSocket *ws = ws_connect("api.openai.com", meet_ws_path(MEET_REALTIME), headers, &error);
    SecureZeroMemory(auth, strlen(auth)); free(auth);
    SecureZeroMemory(c->key, strlen(c->key)); free(c->key);
    if (!ws) { free(c->setup); free(c); char *why = xstrfmt("OpenAI: %s", error ? error : "could not connect."); free(error); post(gen, WM_MEET_ENDED, why); return 0; }
    EnterCriticalSection(&M.lock);
    bool current = gen == M.gen && M.state == MEETING_CONNECTING;
    if (current) M.ws = ws;
    LeaveCriticalSection(&M.lock);
    if (!current) { ws_free(ws); free(c->setup); free(c); return 0; }
    ws_send(ws, c->setup);
    free(c->setup); free(c);
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
    char *events[2] = { 0 };
    size_t n = meet_tool_output_events(l->id, output, events);
    for (size_t i = 0; i < n; i++) send_owned(events[i]);
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
    if (req->ok) l->answers[i] = json_clone(req->result);
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
    if (!meet_tool_find(e->text, &l->tool)) { lookup_answer(l, meet_tool_error("No such tool: only the project's read-only tools are offered.")); return; }
    l->count = meet_tool_calls(l->tool, l->args, M.repo, l->calls, &refusal);
    if (!l->count) { lookup_answer(l, refusal ? refusal : meet_tool_error("Nothing to read.")); return; }
    if (!store_supports(l->calls[0].op)) { lookup_answer(l, meet_tool_error("This device's token cannot read that on the server.")); return; }
    for (size_t i = 0; i < l->count; i++) {
        // The call's arguments go to the request; the plan keeps none.
        Json *args = l->calls[i].args; l->calls[i].args = NULL;
        if (!store_supports(l->calls[i].op)) { json_free(args); l->done++; continue; }
        store_call(l->calls[i].op, args, 0, l, lookup_done, (int)i, &l->reqs[i]);
    }
    changed();
}

// MARK: - Events on the UI thread

static void finish(const char *error);
static void on_event(MeetEvent *e) {
    if (e->has_usage) meet_usage_merge(&M.record.usage, &e->usage, e->usage_snapshot);
    switch (e->kind) {
    case MEET_EV_HEARD_TURN: {
        char *spaced = xstrfmt(" %s", e->text);
        log_add(MEET_SPEAKER_MEETING, spaced);
        free(spaced);
        if (!M.muted && meet_should_answer(&M.persona, e->text)) send_owned(meet_answer_now_event());
        break;
    }
    case MEET_EV_SAID: log_add(MEET_SPEAKER_ASSISTANT, e->text); break;
    case MEET_EV_TOOL: on_tool(e); break;
    case MEET_EV_ERROR:
        // Before the session took its setup an error ends it; later ones (a refused command) only show.
        if (!M.ready) finish(e->text);
        else set_string(&M.error, e->text);
        break;
    default: break;
    }
    changed();
}
static void on_ready(void) {
    if (M.ready || M.state != MEETING_CONNECTING) return;
    M.ready = true;
    M.state = MEETING_LIVE;
    M.joined_tick = GetTickCount();
    M.sending = 1;
    M.sender = (HANDLE)_beginthreadex(NULL, 0, sender_main, NULL, 0, NULL);
    send_owned(meet_greeting_event(&M.persona));
    SetTimer(M.hwnd, TIMER_TICK, 1000, NULL);
    changed();
}

static LRESULT CALLBACK meeting_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool current = (LONG)wp == M.gen && M.state != MEETING_OFF;
    switch (msg) {
    case WM_MEET_EVENT: {
        MeetEvent *e = (MeetEvent *)lp;
        if (current) {
            if (e->kind == MEET_EV_READY) on_ready(); else on_event(e);
        }
        meet_event_free(e); free(e);
        return 0;
    }
    case WM_MEET_ENDED: {
        char *error = (char *)lp;
        if (current) finish(M.state == MEETING_LEAVING ? NULL : error ? error : "The connection to OpenAI ended.");
        free(error);
        return 0;
    }
    case WM_VOICE_ENDED: {
        // A voice that cannot connect as the meeting starts ends it; later the next answer opens a new socket.
        char *error = (char *)lp;
        if (current && error) { if (!M.ready) finish(error); else { set_string(&M.error, error); changed(); } }
        free(error);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TICK) { if (++M.ticks % KEEP_ALIVE_SECONDS == 0) voice_keep_alive(); changed(); }
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
    InitializeCriticalSection(&V.lock);
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
    // The voice's thread plays only while its socket is current, which it no longer is.
    M.record.usage.spoken_chars = voice_chars();
    voice_stop();
    meet_audio_stop(M.audio); M.audio = NULL;
    for (size_t i = 0; i < M.lookup_count; i++) lookup_free(M.lookups[i]);
    free(M.lookups); M.lookups = NULL; M.lookup_count = 0;
    if (was_live) {
        M.record.seconds = (GetTickCount() - M.joined_tick) / 1000.0;
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
    bool failed = false;
    char *key = meeting_key_read(MEETING_KEY_OPENAI, &failed);
    if (!key || !*key) {
        free(key);
        app_alert("Meeting assistant", failed ? "The OpenAI API key could not be read from Credential Manager."
                                              : "Add your OpenAI API key first: \xE2\x9A\x99 Settings \xE2\x86\x92 Meeting assistant.");
        return false;
    }
    ensure_window();
    char *error = NULL, *note = NULL;
    meeting_settings_load(&M.settings);
    // It speaks with the user's ElevenLabs voice when one is set.
    char *eleven_key = NULL;
    if (*M.settings.eleven_voice) {
        eleven_key = meeting_key_read(MEETING_KEY_ELEVENLABS, &failed);
        if (!eleven_key || !*eleven_key) {
            free(eleven_key); SecureZeroMemory(key, strlen(key)); free(key); meeting_settings_free(&M.settings);
            app_alert("Meeting assistant", failed ? "The ElevenLabs API key could not be read from Credential Manager."
                                                  : "Add your ElevenLabs API key, or clear the ElevenLabs voice: \xE2\x9A\x99 Settings \xE2\x86\x92 Meeting assistant.");
            return false;
        }
    }
    MeetAudio *audio = meet_audio_start(pid, &error, &note);
    if (!audio) {
        app_alert("Meeting assistant", error);
        free(error); SecureZeroMemory(key, strlen(key)); free(key); meeting_settings_free(&M.settings);
        if (eleven_key) { SecureZeroMemory(eleven_key, strlen(eleven_key)); free(eleven_key); }
        return false;
    }
    memset(&M.record, 0, sizeof M.record);
    M.record.model = MEET_REALTIME; M.record.started = (double)time(NULL);
    M.audio = audio; M.muted = false; M.ready = false;
    M.repo = xstrdup(project->repo);
    const char *title = project_title(project);
    M.title = str_eq(title, project->repo) ? xstrdup(title) : xstrfmt("%s (%s)", title, project->repo);
    M.source = xstrdup(source ? source : "every app");
    M.persona = (MeetPersona){ M.settings.name, M.settings.voice, M.settings.wake_words, M.title, M.settings.introduce, M.settings.independent,
                               eleven_key ? M.settings.eleven_voice : NULL };
    M.ticks = 0;
    meet_log_free(&M.log); meet_log_init(&M.log);
    M.state = MEETING_CONNECTING;
    set_string(&M.error, note);
    free(note);
    Connect *c = xcalloc(1, sizeof *c);
    c->key = key; c->gen = M.gen;
    c->setup = meet_setup_event(&M.persona);
    if (eleven_key) voice_start(eleven_key, M.settings.eleven_voice, M.gen);
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
    str_appendf(&s, "\xF0\x9F\x8E\x99 %s%s \xC2\xB7 %s", meet_model_label(MEET_REALTIME), M.persona.eleven_voice ? " + ElevenLabs" : "", M.source);
    if (M.state == MEETING_CONNECTING) str_appendz(&s, " \xC2\xB7 connecting\xE2\x80\xA6");
    else if (M.state == MEETING_LEAVING) str_appendz(&s, " \xC2\xB7 leaving\xE2\x80\xA6");
    else {
        unsigned secs = (GetTickCount() - M.joined_tick) / 1000;
        MeetUsage usage = M.record.usage;
        usage.spoken_chars = voice_chars();
        str_appendf(&s, " \xC2\xB7 %u:%02u \xC2\xB7 $%.3f", secs / 60, secs % 60, meet_usage_cost(MEET_REALTIME, &usage));
        str_appendz(&s, M.muted ? " \xC2\xB7 muted" : M.lookup_count ? " \xC2\xB7 looking it up\xE2\x80\xA6" : M.audio && meet_audio_speaking(M.audio) ? " \xC2\xB7 speaking" : " \xC2\xB7 listening");
    }
    if (M.error) str_appendf(&s, " \xC2\xB7 %s", M.error);
    return str_detach(&s);
}
char *meeting_transcript(void) { return M.hwnd ? log_tail(1 << 20) : xstrdup(""); }

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
