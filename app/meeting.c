#include "meeting.h"
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
enum { TIMER_POLL_AGENT = 1, TIMER_TICK = 2, TIMER_CLOSE_WAIT = 3 };
#define ANSWER_TIMEOUT_MS (10 * 60 * 1000)
#define PROGRESS_AFTER_MS 20000
/// ElevenLabs closes a socket after 20 s without a message.
#define KEEP_ALIVE_SECONDS 10

/// One question for the agent, from a delegation or a function call.
typedef struct { char *id, *request; DWORD asked; } Ask;

static struct {
    MeetingState state;
    HWND hwnd;
    MeetModel model;
    MeetPersona persona; MeetingSettings settings;
    char *session_id, *title, *source, *error;
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
    double cost_at_join, cost_now; bool has_cost;
    // The agent: the questions waiting, the one asked, and where the conversation's events were read up to.
    Ask *asks; size_t ask_count;
    bool asking, progress_sent;
    int number; char *marker;
    MeetReplyScan scan;
    int cursor;
    Request *req_message, *req_poll;
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
        bool ok = meet_event_parse(M.model, text, len, e);
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
        char *event = meet_audio_event(M.model, pcm, n);
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
    WebSocket *ws = ws_connect("api.openai.com", meet_ws_path(M.model), headers, &error);
    SecureZeroMemory(auth, strlen(auth)); free(auth);
    SecureZeroMemory(c->key, strlen(c->key)); free(c->key);
    if (!ws) { free(c->setup); free(c); post(gen, WM_MEET_ENDED, error); return 0; }
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

// MARK: - Asking the agent

static void next_ask(void);
static void finish_ask(const char *answer) {
    if (!M.ask_count) return;
    Ask *a = &M.asks[0];
    char *spoken = meet_spoken(answer, MEET_SPOKEN_MAX);
    if (!*spoken) { free(spoken); spoken = xstrdup("The agent finished without saying anything."); }
    char *events[2] = { 0 };
    size_t n = meet_result_events(M.model, a->id, spoken, events);
    for (size_t i = 0; i < n; i++) send_owned(events[i]);
    M.record.answers++;
    M.record.answer_seconds += (GetTickCount() - a->asked) / 1000.0;
    free(spoken); free(a->id); free(a->request);
    memmove(M.asks, M.asks + 1, (M.ask_count - 1) * sizeof *M.asks);
    M.ask_count--;
    M.asking = false;
    KillTimer(M.hwnd, TIMER_POLL_AGENT);
    request_cancel(&M.req_poll); request_cancel(&M.req_message);
    meet_reply_free(&M.scan);
    changed();
    next_ask();
}
static void read_cost(const Json *session) {
    double cost;
    if (!json_num(json_get(session, "costUsd"), &cost)) return;
    if (!M.has_cost) { M.cost_at_join = cost; M.has_cost = true; }
    M.cost_now = cost;
}
static void poll_done(void *owner, Request *req) {
    (void)owner;
    if (M.state == MEETING_OFF || !M.asking) return;
    if (req->ok) {
        const Json *events = json_get(req->result, "events");
        read_cost(json_get(req->result, "session"));
        for (size_t i = 0; i < json_count(events); i++) { int seq = json_int_or(json_get(json_at(events, i), "seq"), 0); if (seq > M.cursor) M.cursor = seq; }
        meet_reply_feed(&M.scan, events, M.marker);
        if (M.scan.done) { finish_ask(M.scan.reply.data ? M.scan.reply.data : ""); return; }
    }
    DWORD waited = GetTickCount() - M.asks[0].asked;
    if (waited > ANSWER_TIMEOUT_MS) { finish_ask("The agent has not answered after ten minutes; it may still be working on it."); return; }
    if (!M.progress_sent && waited > PROGRESS_AFTER_MS) {
        M.progress_sent = true;
        send_owned(meet_progress_event(M.model, M.asks[0].id, "The agent is still working on it. Nothing has been answered yet."));
    }
    SetTimer(M.hwnd, TIMER_POLL_AGENT, 1500, NULL);
}
static void poll_agent(void) {
    if (M.req_poll) return;
    Json *args = json_object();
    json_set_str(args, "sessionId", M.session_id);
    json_set_num(args, "since", M.cursor);
    store_call("session", args, 0, NULL, poll_done, 0, &M.req_poll);
}
static void message_done(void *owner, Request *req) {
    (void)owner;
    if (M.state == MEETING_OFF || !M.asking) return;
    if (!req->ok) {
        char *why = request_error_text(req);
        char *answer = xstrfmt("I could not reach the agent: %s", why);
        finish_ask(answer);
        free(answer); free(why);
        return;
    }
    poll_agent();
}
static void next_ask(void) {
    if (M.asking || !M.ask_count || M.state != MEETING_LIVE) return;
    M.asking = true; M.progress_sent = false;
    M.number++;
    free(M.marker); M.marker = meet_agent_marker(M.number);
    meet_reply_init(&M.scan);
    char *transcript = log_tail(4000);
    char *text = meet_agent_message(M.number, &M.persona, M.asks[0].request, transcript);
    free(transcript);
    Json *args = json_object();
    json_set_str(args, "sessionId", M.session_id);
    json_set_str(args, "text", text);
    free(text);
    M.record.requests++;
    store_call("message", args, 0, NULL, message_done, 0, &M.req_message);
    changed();
}

// MARK: - Events on the UI thread

static void finish(const char *error);
static void on_event(MeetEvent *e) {
    if (e->has_usage) meet_usage_merge(&M.record.usage, &e->usage, e->usage_snapshot);
    switch (e->kind) {
    case MEET_EV_HEARD: log_add(MEET_SPEAKER_MEETING, e->text); break;
    case MEET_EV_HEARD_TURN: {
        char *spaced = xstrfmt(" %s", e->text);
        log_add(MEET_SPEAKER_MEETING, spaced);
        free(spaced);
        if (!M.muted && meet_should_answer(&M.persona, e->text)) send_owned(meet_answer_now_event(M.model));
        break;
    }
    case MEET_EV_SAID: log_add(MEET_SPEAKER_ASSISTANT, e->text); break;
    case MEET_EV_ASK:
        M.asks = xrealloc(M.asks, (M.ask_count + 1) * sizeof *M.asks);
        M.asks[M.ask_count].id = e->id; M.asks[M.ask_count].request = e->request; M.asks[M.ask_count].asked = GetTickCount();
        e->id = e->request = NULL;
        M.ask_count++;
        next_ask();
        break;
    case MEET_EV_ERROR:
        // Before the session took its setup an error ends it; later ones (a refused command) only show.
        if (!M.ready) finish(e->text);
        else set_string(&M.error, e->text);
        break;
    case MEET_EV_CLOSED: if (M.state == MEETING_LEAVING) finish(NULL); break;
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
    send_owned(meet_greeting_event(M.model, &M.persona));
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
        if (wp == TIMER_POLL_AGENT) { KillTimer(hwnd, TIMER_POLL_AGENT); if (M.asking) poll_agent(); }
        else if (wp == TIMER_TICK) { if (++M.ticks % KEEP_ALIVE_SECONDS == 0) voice_keep_alive(); changed(); }
        else if (wp == TIMER_CLOSE_WAIT) { KillTimer(hwnd, TIMER_CLOSE_WAIT); if (M.state == MEETING_LEAVING) finish(NULL); }
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
    KillTimer(M.hwnd, TIMER_POLL_AGENT); KillTimer(M.hwnd, TIMER_TICK); KillTimer(M.hwnd, TIMER_CLOSE_WAIT);
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
    request_cancel(&M.req_message); request_cancel(&M.req_poll);
    if (was_live) {
        M.record.seconds = (GetTickCount() - M.joined_tick) / 1000.0;
        M.record.agent_cost = M.has_cost && M.cost_now > M.cost_at_join ? M.cost_now - M.cost_at_join : 0;
        history_append(&M.record);
    }
    for (size_t i = 0; i < M.ask_count; i++) { free(M.asks[i].id); free(M.asks[i].request); }
    free(M.asks); M.asks = NULL; M.ask_count = 0; M.asking = false;
    meet_reply_free(&M.scan);
    free(M.marker); M.marker = NULL;
    free(M.session_id); M.session_id = NULL; free(M.title); M.title = NULL; free(M.source); M.source = NULL;
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

bool meeting_join(const Session *session, MeetModel model, unsigned pid, const char *source) {
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
    // GPT-Realtime speaks with the user's ElevenLabs voice when one is set.
    char *eleven_key = NULL;
    if (model == MEET_REALTIME && *M.settings.eleven_voice) {
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
    M.model = model; M.record.model = model; M.record.started = (double)time(NULL);
    M.audio = audio; M.muted = false; M.ready = false;
    M.session_id = xstrdup(session_id(session));
    M.title = xstrdup(session_display_title(session));
    M.source = xstrdup(source ? source : "every app");
    M.has_cost = false; read_cost(session->raw);
    M.number = 0;
    M.persona = (MeetPersona){ M.settings.name, M.settings.voice, M.settings.wake_words, M.title, M.settings.introduce, M.settings.independent,
                               eleven_key ? M.settings.eleven_voice : NULL };
    M.ticks = 0;
    meet_log_free(&M.log); meet_log_init(&M.log);
    // The agent's reply is looked for from where the saved transcript ends.
    Transcript saved; transcript_init(&saved);
    char *cache_key = xstrfmt("transcript:%s", M.session_id);
    Json *lines = cache_lines(g_store.cache, cache_key);
    transcript_append(&saved, lines);
    M.cursor = saved.cursor;
    json_free(lines); free(cache_key); transcript_free(&saved);
    M.state = MEETING_CONNECTING;
    set_string(&M.error, note);
    free(note);
    Connect *c = xcalloc(1, sizeof *c);
    c->key = key; c->gen = M.gen;
    c->setup = meet_setup_event(model, &M.persona);
    if (eleven_key) voice_start(eleven_key, M.settings.eleven_voice, M.gen);
    M.reader = (HANDLE)_beginthreadex(NULL, 0, connect_main, c, 0, NULL);
    changed();
    return true;
}

void meeting_leave(void) {
    if (M.state == MEETING_OFF || M.state == MEETING_LEAVING) return;
    if (M.state == MEETING_CONNECTING || !M.ready) { finish(NULL); return; }
    M.state = MEETING_LEAVING;
    InterlockedExchange(&M.sending, 0);
    meet_audio_set_muted(M.audio, true);
    // GPT-Live finalizes its usage on session.close; Realtime has nothing to wait for.
    char *close = meet_close_event(M.model);
    if (close) { ws_send(M.ws, close); free(close); SetTimer(M.hwnd, TIMER_CLOSE_WAIT, 5000, NULL); changed(); }
    else finish(NULL);
}
void meeting_shutdown(void) { if (M.state != MEETING_OFF) finish(NULL); }

MeetingState meeting_state(void) { return M.state; }
bool meeting_for(const char *session_id_) { return M.state != MEETING_OFF && str_eq(M.session_id, session_id_); }
MeetModel meeting_model(void) { return M.model; }
void meeting_set_muted(bool muted) { M.muted = muted; if (M.audio) meet_audio_set_muted(M.audio, muted); changed(); }
bool meeting_muted(void) { return M.muted; }
void meeting_answer_now(void) { if (M.state == MEETING_LIVE) send_owned(meet_answer_now_event(M.model)); }

char *meeting_status(void) {
    if (M.state == MEETING_OFF) return xstrdup("");
    Str s; str_init(&s);
    str_appendf(&s, "\xF0\x9F\x8E\x99 %s%s \xC2\xB7 %s", meet_model_label(M.model), M.persona.eleven_voice ? " + ElevenLabs" : "", M.source);
    if (M.state == MEETING_CONNECTING) str_appendz(&s, " \xC2\xB7 connecting\xE2\x80\xA6");
    else if (M.state == MEETING_LEAVING) str_appendz(&s, " \xC2\xB7 leaving\xE2\x80\xA6");
    else {
        unsigned secs = (GetTickCount() - M.joined_tick) / 1000;
        MeetUsage usage = M.record.usage;
        usage.spoken_chars = voice_chars();
        double cost = meet_usage_cost(M.model, &usage) + (M.has_cost && M.cost_now > M.cost_at_join ? M.cost_now - M.cost_at_join : 0);
        str_appendf(&s, " \xC2\xB7 %u:%02u \xC2\xB7 $%.3f", secs / 60, secs % 60, cost);
        str_appendz(&s, M.muted ? " \xC2\xB7 muted" : M.asking ? " \xC2\xB7 asking the agent\xE2\x80\xA6" : M.audio && meet_audio_speaking(M.audio) ? " \xC2\xB7 speaking" : " \xC2\xB7 listening");
    }
    if (M.error) str_appendf(&s, " \xC2\xB7 %s", M.error);
    return str_detach(&s);
}
char *meeting_transcript(void) { return M.hwnd ? log_tail(1 << 20) : xstrdup(""); }
