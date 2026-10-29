#include "sleep_internal.h"
#include "credentials.h"
#include "screens.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

#define SLEEP_CLASS L"BriareusSleep"
#define SLEEP_TIMER 0x534c
static struct {
    HWND main, window, focus;
    HHOOK hook;
    SleepIdle idle;
    SleepPointer pointer;
    SleepWall wall;
    ApiClient *client;
    char *device, *scope;
    int minutes, modals, failures;
    uint64_t stream_since;
    SleepFollow *follow;
    Request *sessions, *history, *files[SLEEP_PANES];
    uint64_t events_seen;
    uint64_t identities[SLEEP_PANES], revisions[SLEEP_PANES], retry, replay_at, history_at[SLEEP_PANES];
    SleepReplay replay;
    SleepRecords registry;
    SleepRefresh refresh;
    Json *unknown[SLEEP_PANES]; int unknown_cursor[SLEEP_PANES]; size_t unknown_next;
} g_sleep;

static uint64_t now_ms(void) { return GetTickCount64(); }
static char *scope_key(void) {
    Json *j = g_store.has_device ? device_json(&g_store.device) : json_null();
    char *key = json_serialize(j, true); json_free(j); return key;
}
static bool current(void) {
    if (g_sleep.client != g_store.client || !str_eq(g_sleep.device, g_store.device.id)) return false;
    char *key = scope_key(); bool same = str_eq(key, g_sleep.scope); free(key); return same;
}
static void pointer_reset(void) {
    POINT pt;
    if (GetCursorPos(&pt)) sleep_pointer_reset(&g_sleep.pointer, pt.x, pt.y);
    else g_sleep.pointer.known = false;
}
static void wake(void) {
    pointer_reset();
    g_sleep.idle.asleep = false; g_sleep.idle.input = now_ms();
    sleep_follow_cancel(g_sleep.follow); sleep_follow_release(g_sleep.follow); g_sleep.follow = NULL;
    request_cancel(&g_sleep.sessions); request_cancel(&g_sleep.history);
    for (int i = 0; i < SLEEP_PANES; i++) {
        request_cancel(&g_sleep.files[i]); sleep_replay_clear(&g_sleep.replay, i);
        g_sleep.history_at[i] = g_sleep.revisions[i] = g_sleep.identities[i] = 0;
    }
    sleep_wall_free(&g_sleep.wall);
    sleep_records_free(&g_sleep.registry);
    memset(&g_sleep.refresh, 0, sizeof g_sleep.refresh);
    for (int i = 0; i < SLEEP_PANES; i++) { json_free(g_sleep.unknown[i]); g_sleep.unknown[i] = NULL; }
    memset(g_sleep.unknown_cursor, 0, sizeof g_sleep.unknown_cursor); g_sleep.unknown_next = 0;
    g_sleep.failures = 0;
    if (g_sleep.window) ShowWindow(g_sleep.window, SW_HIDE);
    if (g_sleep.focus && IsWindow(g_sleep.focus) && IsWindowVisible(g_sleep.focus)) SetFocus(g_sleep.focus);
    g_sleep.focus = NULL;
    api_client_release(g_sleep.client); g_sleep.client = NULL;
    free(g_sleep.device); g_sleep.device = NULL; free(g_sleep.scope); g_sleep.scope = NULL;
}
void sleep_scope_changed(void) { if (g_sleep.idle.asleep && (!current() || IsIconic(g_sleep.main))) wake(); }
void sleep_deactivated(void) {
    pointer_reset();
    memset(g_sleep.idle.held, 0, sizeof g_sleep.idle.held);
    memset(g_sleep.idle.blocked, 0, sizeof g_sleep.idle.blocked);
}
void sleep_modal(bool begin) {
    pointer_reset();
    if (begin) { if (g_sleep.idle.asleep) wake(); g_sleep.modals++; }
    else if (g_sleep.modals) g_sleep.modals--;
    g_sleep.idle.input = now_ms();
}
int sleep_after(void) { return g_sleep.minutes; }
void sleep_set_after(int minutes) { g_sleep.minutes = sleep_minutes(minutes); settings_write_sleep(g_sleep.minutes); wake(); }
void sleep_settings_action(void) {
    const char *choices[] = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
    int chosen = app_choose("Sleep after", "Minutes without input on this computer.", choices, 7);
    if (chosen >= 0) sleep_set_after(sleep_choices[chosen]);
}
static int find_pane(const char *id) {
    if (str_empty(id)) return -1;
    for (int i = 0; i < SLEEP_PANES; i++) if (str_eq(session_id(&g_sleep.wall.panes[i].session), id)) return i;
    return -1;
}
static int record_index(const char *id) { return sleep_records_find(&g_sleep.registry, id); }
static void apply_line(int i, const Json *line, bool replay) {
    SleepPane *p = &g_sleep.wall.panes[i];
    int seq = json_int_or(json_get(line, "seq"), 0), record_slot = record_index(session_id(&p->session));
    if (!replay && record_slot >= 0) {
        if (seq <= g_sleep.registry.items[record_slot].cursor) return;
        g_sleep.registry.items[record_slot].cursor = seq; sleep_records_touch(&g_sleep.registry, record_slot);
    }
    SleepStep step = sleep_step(line, session_repo(&p->session));
    if (!replay && step.kind != SLEEP_NONE && p->replay) {
        Session copy = {0}; session_copy(&copy, &p->session);
        i = sleep_assign(&g_sleep.wall, &copy, now_ms(), false); session_free(&copy); p = &g_sleep.wall.panes[i];
    }
    if (!replay) {
        p->cursor = seq;
        if (step.kind == SLEEP_FILE) p->file_cursor = seq;
        if (step.kind == SLEEP_COMMAND) p->command_cursor = seq;
    }
    sleep_apply(&g_sleep.wall, i, &step, now_ms(), replay); sleep_step_free(&step);
}
static int record(const Session *s) { return sleep_records_put(&g_sleep.registry, s, &g_sleep.wall, false); }
static void remember_unknown(const Json *packet) {
    const char *id = json_str_nonempty(json_get(packet, "sessionId")); if (!id) return;
    size_t at = SLEEP_PANES, empty = SLEEP_PANES;
    for (size_t i = 0; i < SLEEP_PANES; i++) {
        if (!g_sleep.unknown[i] && empty == SLEEP_PANES) empty = i;
        if (str_eq(id, json_str(json_get(g_sleep.unknown[i], "sessionId")))) { at = i; break; }
    }
    if (at == SLEEP_PANES) at = empty < SLEEP_PANES ? empty : g_sleep.unknown_next++ % SLEEP_PANES;
    const Json *old_event = json_get(g_sleep.unknown[at], "event"), *next_event = json_get(packet, "event");
    bool same = str_eq(id, json_str(json_get(g_sleep.unknown[at], "sessionId")));
    int old = json_int_or(json_get(old_event, "seq"), 0), next = json_int_or(json_get(next_event, "seq"), 0);
    if (!same) g_sleep.unknown_cursor[at] = 0;
    if (next > g_sleep.unknown_cursor[at]) g_sleep.unknown_cursor[at] = next;
    bool useful = sleep_event_kind(next_event) != SLEEP_NONE, old_useful = sleep_event_kind(old_event) != SLEEP_NONE;
    bool replace = !same;
    if (useful) replace |= !old_useful || next > old;
    else if (!old_useful) replace |= next > old;
    if (replace) { json_free(g_sleep.unknown[at]); g_sleep.unknown[at] = json_clone(packet); }
    sleep_refresh_hint(&g_sleep.refresh, now_ms());
}
static void receive(const char *type, const Json *j) {
    if (str_eq(type, "session")) {
        Session s = {0}; if (!session_parse(j, &s)) return;
        if (record(&s) < 0) { session_free(&s); return; }
        int i = find_pane(session_id(&s));
        if (session_is_active(&s) || i >= 0) {
            bool replay = i >= 0 && g_sleep.wall.panes[i].replay && !session_is_active(&s);
            sleep_assign(&g_sleep.wall, &s, now_ms(), replay);
        }
        session_free(&s);
    } else if (str_eq(type, "session.deleted")) {
        const char *id = json_str(json_get(j, "id")); sleep_remove(&g_sleep.wall, id);
        sleep_records_delete(&g_sleep.registry, id);
        for (int i = 0; i < SLEEP_PANES; i++) if (str_eq(id, json_str(json_get(g_sleep.unknown[i], "sessionId")))) {
            json_free(g_sleep.unknown[i]); g_sleep.unknown[i] = NULL; g_sleep.unknown_cursor[i] = 0;
        }
    }
    else if (str_eq(type, "transcript")) {
        int i = find_pane(json_str(json_get(j, "sessionId")));
        const char *id = json_str(json_get(j, "sessionId"));
        int r = record_index(id);
        if (r < 0) { remember_unknown(j); return; }
        if (json_int_or(json_get(json_get(j, "event"), "seq"), 0) <= g_sleep.registry.items[r].cursor) return;
        if (i < 0) {
            for (size_t k = 0; k < g_sleep.registry.count; k++) if (str_eq(id, session_id(&g_sleep.registry.items[k].session))) {
                SleepStep step = sleep_step(json_get(j, "event"), session_repo(&g_sleep.registry.items[k].session));
                if (step.kind != SLEEP_NONE) i = sleep_assign(&g_sleep.wall, &g_sleep.registry.items[k].session, now_ms(), false);
                sleep_step_free(&step); break;
            }
        }
        if (i >= 0) {
            apply_line(i, json_get(j, "event"), false);
        }
    }
}
static void resolve_unknown(const Session *items, size_t count) {
    for (int i = 0; i < SLEEP_PANES; i++) {
        Json *packet = g_sleep.unknown[i]; if (!packet) continue;
        const char *id = json_str(json_get(packet, "sessionId"));
        for (size_t k = 0; k < count; k++) if (str_eq(id, session_id(&items[k]))) {
            int at = sleep_records_put(&g_sleep.registry, &items[k], &g_sleep.wall, true);
            if (at >= 0) {
                receive("transcript", packet);
                if (g_sleep.unknown_cursor[i] > g_sleep.registry.items[at].cursor) g_sleep.registry.items[at].cursor = g_sleep.unknown_cursor[i];
                int pane = find_pane(id);
                if (pane >= 0 && g_sleep.wall.panes[pane].cursor < g_sleep.unknown_cursor[i]) g_sleep.wall.panes[pane].cursor = g_sleep.unknown_cursor[i];
            }
            break;
        }
        json_free(packet); g_sleep.unknown[i] = NULL; g_sleep.unknown_cursor[i] = 0;
    }
}
static void sessions_done(void *owner, Request *r) {
    (void)owner; if (!current()) return;
    bool accepted = r->ok && (uint64_t)r->arg == g_sleep.events_seen;
    Session *items = NULL; size_t n = 0;
    if (accepted) accepted = sessions_parse(r->result, &items, &n);
    sleep_refresh_finished(&g_sleep.refresh, now_ms(), accepted);
    if (!accepted) return;
    for (size_t k = 0; k < n; k++) {
        int at = record(&items[k]);
        if (at < 0) {
            at = record_index(session_id(&items[k]));
            if (at >= 0) { session_free(&items[k]); session_copy(&items[k], &g_sleep.registry.items[at].session); }
        }
    }
    size_t selected[SLEEP_PANES]; size_t count = sleep_select(items, n, selected, true);
    bool replay = !count || sleep_replay_due(&g_sleep.wall, now_ms());
    // Reconcile deletions without trusting a reconnect to replay deletion events.
    for (int i = 0; i < SLEEP_PANES; i++) {
        const char *id = session_id(&g_sleep.wall.panes[i].session); bool found = false;
        for (size_t k = 0; k < n; k++) if (str_eq(id, session_id(&items[k]))) found = true;
        if (id && !found) sleep_remove(&g_sleep.wall, id);
    }
    if (replay) {
        bool already = false;
        for (int i = 0; i < SLEEP_PANES; i++) already |= g_sleep.wall.panes[i].replay;
        if (already) { resolve_unknown(items, n); sessions_free(items, n); return; }
        count = sleep_select(items, n, selected, false);
    }
    int cursors[SLEEP_PANES] = {0};
    for (size_t k = 0; k < count; k++) {
        int at = record_index(session_id(&items[selected[k]]));
        if (at >= 0) cursors[k] = g_sleep.registry.items[at].cursor;
    }
    if (replay) sleep_wall_free(&g_sleep.wall);
    for (size_t k = 0; k < count; k++) {
        int at = sleep_records_put(&g_sleep.registry, &items[selected[k]], &g_sleep.wall, true);
        if (at >= 0 && cursors[k] > g_sleep.registry.items[at].cursor) g_sleep.registry.items[at].cursor = cursors[k];
        sleep_assign(&g_sleep.wall, &items[selected[k]], now_ms(), replay);
    }
    resolve_unknown(items, n); sessions_free(items, n);
}
static void history_done(void *owner, Request *r) {
    (void)owner; int i = r->tag; SleepPane *p = &g_sleep.wall.panes[i];
    if (!current() || !r->ok || (uint64_t)r->arg != p->identity) return;
    if (!str_eq(json_str(json_get(r->args, "sessionId")), session_id(&p->session))) return;
    const Json *events = json_get(r->result, "events");
    if (p->replay) {
        sleep_replay_load(&g_sleep.replay, i, events, session_repo(&p->session));
        int at = record_index(session_id(&p->session));
        for (size_t k = 0; at >= 0 && k < json_count(events); k++) {
            int seq = json_int_or(json_get(json_at(events, k), "seq"), 0);
            if (seq > g_sleep.registry.items[at].cursor) g_sleep.registry.items[at].cursor = seq;
        }
    }
    else {
        bool bootstrap = !p->seeded;
        if (bootstrap) sleep_seed(&g_sleep.wall, i, events, now_ms());
        else for (size_t k = 0; k < json_count(events); k++) apply_line(i, json_at(events, k), false);
        int at = record_index(session_id(&p->session));
        for (size_t k = 0; k < json_count(events); k++) {
            int seq = json_int_or(json_get(json_at(events, k), "seq"), 0);
            if (seq > p->cursor) p->cursor = seq;
            if (at >= 0 && seq > g_sleep.registry.items[at].cursor) g_sleep.registry.items[at].cursor = seq;
        }
    }
}
static void file_done(void *owner, Request *r) {
    (void)owner; int i = r->tag; SleepPane *p = &g_sleep.wall.panes[i];
    if (!current() || (uint64_t)r->arg != p->revision) return;
    if (!r->ok && r->error.status == 404 && json_str_nonempty(json_get(r->args, "ref"))) {
        Request *next = store_call_background("repo_file", sleep_file_args(p, true), 15000, NULL, file_done, i, &g_sleep.files[i]);
        next->arg = r->arg; return;
    }
    sleep_loaded(p, (uint64_t)r->arg, r->ok ? r->result : NULL);
}
static void reads(uint64_t now) {
    for (int i = 0; i < SLEEP_PANES; i++) {
        SleepPane *p = &g_sleep.wall.panes[i]; if (!p->session.raw) continue;
        if (p->identity != g_sleep.identities[i]) {
            g_sleep.identities[i] = p->identity;
            sleep_replay_clear(&g_sleep.replay, i); g_sleep.history_at[i] = 0;
        }
        if (p->revision != g_sleep.revisions[i]) {
            request_cancel(&g_sleep.files[i]); g_sleep.revisions[i] = p->revision;
        }
        if (p->path && !p->loaded && !g_sleep.files[i]) {
            if (store_supports("repo_file")) {
                Request *r = store_call_background("repo_file", sleep_file_args(p, false), 15000, NULL, file_done, i, &g_sleep.files[i]); r->arg = (intptr_t)p->revision;
            } else sleep_loaded(p, p->revision, NULL);
        }
        if (!g_sleep.history && store_supports("session") && now >= g_sleep.history_at[i]) {
            Json *args = json_object(); json_set_str(args, "sessionId", session_id(&p->session)); json_set_num(args, "since", p->replay || !p->seeded ? 0 : p->cursor);
            Request *r = store_call_background("session", args, 15000, NULL, history_done, i, &g_sleep.history); r->arg = (intptr_t)p->identity;
            g_sleep.history_at[i] = now + 30000;
        }
    }
}
void sleep_pulse(uint64_t now) {
    if (g_sleep.idle.asleep && (!current() || IsIconic(g_sleep.main))) wake();
    if (!g_sleep.idle.asleep) {
        bool eligible = store_connected() && store_supports("events") && !g_sleep.modals && !IsIconic(g_sleep.main);
        if (!sleep_due(&g_sleep.idle, now, g_sleep.minutes, eligible)) return;
        g_sleep.client = api_client_retain(g_store.client); g_sleep.device = xstrdup(g_store.device.id); g_sleep.scope = scope_key();
        g_sleep.wall.last_live = now;
        g_sleep.focus = GetFocus(); g_sleep.retry = g_sleep.replay_at = 0;
        memset(&g_sleep.refresh, 0, sizeof g_sleep.refresh);
        pointer_reset();
        ShowWindow(g_sleep.window, SW_SHOWNA);
    }
    RECT rc; GetClientRect(g_sleep.main, &rc);
    SetWindowPos(g_sleep.window, HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_NOACTIVATE);
    if (!g_sleep.follow && now >= g_sleep.retry) {
        g_sleep.follow = sleep_follow_new(); g_sleep.stream_since = now;
        if (!g_sleep.follow) {
            if (g_sleep.failures < 6) g_sleep.failures++;
            g_sleep.retry = now + (uint64_t)poll_delay_ms(3000, g_sleep.failures, 0);
        }
    }
    if (g_sleep.follow) {
        SleepFollowInfo info; Json *queue = sleep_follow_drain(g_sleep.follow, &info);
        if (info.reconcile) { g_sleep.events_seen++; sleep_refresh_hint(&g_sleep.refresh, now); }
        for (size_t k = 0; k < json_count(queue); k++) {
            const Json *item = json_at(queue, k); const char *type = json_str(json_get(item, "type"));
            if (!str_eq(type, "transcript")) g_sleep.events_seen++;
            receive(type, json_get(item, "data"));
        }
        json_free(queue);
        if (now - g_sleep.stream_since >= 60000) g_sleep.failures = 0;
        if (info.ended) {
            sleep_follow_release(g_sleep.follow); g_sleep.follow = NULL;
            if (g_sleep.failures < 6) g_sleep.failures++;
            int delay = poll_delay_ms(3000, g_sleep.failures, info.retry_after);
            if (info.status >= 400 && info.status < 500 && delay < 60000) delay = 60000;
            g_sleep.retry = now + (uint64_t)delay; sleep_refresh_hint(&g_sleep.refresh, now);
            // Catch up displayed transcripts from their last accepted seq after a real disconnect.
            for (int i = 0; i < SLEEP_PANES; i++) g_sleep.history_at[i] = 0;
        }
    }
    if (!g_sleep.sessions && sleep_refresh_due(&g_sleep.refresh, now) && store_supports("sessions")) {
        sleep_refresh_started(&g_sleep.refresh, now);
        Request *r = store_call_background("sessions", json_object(), 15000, NULL, sessions_done, 0, &g_sleep.sessions); r->arg = (intptr_t)g_sleep.events_seen;
    }
    sleep_tick(&g_sleep.wall, now); reads(now);
    if (now >= g_sleep.replay_at) {
        g_sleep.replay_at = now + 2400;
        int i; const SleepStep *step = sleep_replay_next(&g_sleep.replay, &i);
        if (step && g_sleep.wall.panes[i].replay) sleep_apply(&g_sleep.wall, i, step, now, true);
    }
    InvalidateRect(g_sleep.window, NULL, FALSE);
}
static LRESULT CALLBACK input_hook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        MSG *m = (MSG *)lp; UINT msg = m->message;
        bool key = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        bool move = msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE;
        if (move && !sleep_pointer_moved(&g_sleep.pointer, m->pt.x, m->pt.y)) return CallNextHookEx(g_sleep.hook, code, wp, lp);
        bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || move;
        bool nc = msg >= WM_NCLBUTTONDOWN && msg <= WM_NCXBUTTONDBLCLK;
        if (key || mouse || nc) {
            unsigned button = 0;
            if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) button = (unsigned)m->wParam + 1;
            if (msg >= WM_LBUTTONDOWN && msg <= WM_LBUTTONDBLCLK) button = 257;
            if (msg >= WM_RBUTTONDOWN && msg <= WM_RBUTTONDBLCLK) button = 258;
            if (msg >= WM_MBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) button = 259;
            if (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK) button = HIWORD(m->wParam) == XBUTTON1 ? 260 : 261;
            if (msg >= WM_NCLBUTTONDOWN && msg <= WM_NCLBUTTONDBLCLK) button = 257;
            if (msg >= WM_NCRBUTTONDOWN && msg <= WM_NCRBUTTONDBLCLK) button = 258;
            if (msg >= WM_NCMBUTTONDOWN && msg <= WM_NCMBUTTONDBLCLK) button = 259;
            if (msg >= WM_NCXBUTTONDOWN && msg <= WM_NCXBUTTONDBLCLK) button = HIWORD(m->wParam) == XBUTTON1 ? 260 : 261;
            bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
            bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
            if (msg == WM_NCLBUTTONUP || msg == WM_NCRBUTTONUP || msg == WM_NCMBUTTONUP || msg == WM_NCXBUTTONUP) up = true;
            if (msg == WM_NCLBUTTONDOWN || msg == WM_NCRBUTTONDOWN || msg == WM_NCMBUTTONDOWN || msg == WM_NCXBUTTONDOWN) down = true;
            if (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK) down = true;
            bool asleep = g_sleep.idle.asleep;
            bool consume = sleep_input(&g_sleep.idle, now_ms(), button, down, up);
            if (asleep) wake();
            if (consume) { m->message = WM_NULL; m->wParam = m->lParam = 0; }
        }
    }
    return CallNextHookEx(g_sleep.hook, code, wp, lp);
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) { sleep_paint(hwnd, &g_sleep.wall); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_TIMER && wp == SLEEP_TIMER) { sleep_pulse(now_ms()); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void sleep_start(HWND main) {
    g_sleep.main = main; g_sleep.minutes = settings_read_sleep(); g_sleep.idle.input = now_ms();
    WNDCLASSW wc = {0}; wc.hInstance = GetModuleHandleW(NULL); wc.lpfnWndProc = window_proc; wc.lpszClassName = SLEEP_CLASS; wc.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassW(&wc);
    g_sleep.window = CreateWindowExW(0, SLEEP_CLASS, L"Session activity", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, main, NULL, wc.hInstance, NULL);
    pointer_reset();
    g_sleep.hook = SetWindowsHookExW(WH_GETMESSAGE, input_hook, NULL, GetCurrentThreadId());
    if (g_sleep.hook && g_sleep.window) SetTimer(g_sleep.window, SLEEP_TIMER, 80, NULL);
}
void sleep_stop(void) {
    if (g_sleep.hook) UnhookWindowsHookEx(g_sleep.hook);
    wake(); if (g_sleep.window) { KillTimer(g_sleep.window, SLEEP_TIMER); DestroyWindow(g_sleep.window); }
    memset(&g_sleep, 0, sizeof g_sleep);
}
