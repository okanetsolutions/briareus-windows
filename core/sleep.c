#include "sleep.h"
#include "repo.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

const int sleep_choices[7] = {0, 1, 2, 5, 10, 15, 30};
int sleep_minutes(int value) {
    for (int i = 0; i < 7; i++) if (value == sleep_choices[i]) return value;
    return 5;
}
static bool any_held(const uint32_t keys[9]) {
    for (int i = 0; i < 9; i++) if (keys[i]) return true;
    return false;
}
bool sleep_due(SleepIdle *s, uint64_t now, int minutes, bool eligible) {
    if (!eligible) { s->input = now; return false; }
    if (s->asleep || !minutes || any_held(s->held)) return false;
    s->asleep = now - s->input >= (uint64_t)sleep_minutes(minutes) * 60000;
    return s->asleep;
}
bool sleep_input(SleepIdle *s, uint64_t now, unsigned input, bool down, bool up) {
    bool consume = s->asleep || now < s->swallow || any_held(s->blocked);
    if (input && input < 288) {
        unsigned word = input / 32, bit = input % 32; uint32_t mask = UINT32_C(1) << bit;
        if (down) { s->held[word] |= mask; if (consume) s->blocked[word] |= mask; }
        if (up) {
            if (s->blocked[word] & mask) consume = true;
            s->held[word] &= ~mask; s->blocked[word] &= ~mask;
        }
    }
    if (s->asleep) s->swallow = now + 600;
    s->input = now; s->asleep = false;
    return consume;
}
static char *first(const char *text, size_t max) {
    if (!text) return NULL;
    while (*text && isspace((unsigned char)*text)) text++;
    size_t n = strcspn(text, "\r\n"); if (n > max) n = max;
    while (n && isspace((unsigned char)text[n - 1])) n--;
    return n ? xstrndup(text, n) : NULL;
}
char *sleep_path(const char *path, const char *repo) {
    char *p = first(path, 2048); if (!p) return NULL;
    // Reject truncation, Windows/URL paths, controls and traversal before any request.
    if (strlen(path) > 2048 || strpbrk(p, "\\:\"<>|?*")) { free(p); return NULL; }
    for (const unsigned char *c = (unsigned char *)p; *c; c++) if (*c < 32) { free(p); return NULL; }
    bool absolute = p[0] == '/';
    char *base = NULL;
    if (absolute) {
        if (!repo || !strchr(repo, '/')) { free(p); return NULL; }
        const char *slash = strchr(repo, '/');
        base = xstrfmt("%.*s__%s", (int)(slash - repo), repo, slash + 1);
    }
    Str out; str_init(&out); bool found = !absolute;
    char *at = p;
    while (*at) {
        while (*at == '/') at++;
        char *end = strchr(at, '/'); if (!end) end = at + strlen(at);
        size_t n = (size_t)(end - at);
        if (n == 2 && !memcmp(at, "..", 2)) { str_free(&out); free(base); free(p); return NULL; }
        if (base && n >= strlen(base) && !memcmp(at, base, strlen(base))) {
            const char *suffix = at + strlen(base); bool slot = suffix == end;
            if (end - suffix > 2 && suffix[0] == '_' && suffix[1] == '_') {
                slot = true;
                for (const char *c = suffix + 2; c < end; c++) if (!isdigit((unsigned char)*c)) slot = false;
            }
            if (slot) { found = true; str_free(&out); str_init(&out); at = *end ? end + 1 : end; continue; }
        }
        if (found && n && !(n == 1 && *at == '.')) {
            if (out.len) str_appendz(&out, "/");
            str_append(&out, at, n);
        }
        at = *end ? end + 1 : end;
    }
    free(base); free(p);
    if (!found || !out.len) { str_free(&out); return NULL; }
    return str_detach(&out);
}
SleepStep sleep_step(const Json *event, const char *repo) {
    SleepStep out = {0};
    const char *kind = json_str(json_get(event, "kind"));
    const char *name = json_str_or(json_get(event, "name"), "");
    const char *summary = json_str(json_get(event, "summary"));
    if (str_eq(kind, "cmd") || str_eq(kind, "git")) out.text = first(json_str(json_get(event, "text")), 512);
    else if (str_eq(kind, "tool")) {
        if (str_eq(name, "Bash") || str_eq(name, "Shell") || str_eq(name, "bash") || str_eq(name, "shell")) out.text = first(summary, 512);
        else {
            out.writes = str_eq(name, "Edit") || str_eq(name, "Write") || str_eq(name, "MultiEdit") || str_eq(name, "NotebookEdit");
            if (str_eq(name, "edit") || str_eq(name, "write") || str_eq(name, "patch")) out.writes = true;
            bool read = str_eq(name, "Read") || str_eq(name, "read") || str_eq(name, "view");
            if (!out.writes && !read) return out;
            char *path = summary ? xstrndup(summary, strcspn(summary, ",")) : NULL;
            out.text = sleep_path(path, repo); free(path);
            if (out.text) out.kind = SLEEP_FILE;
            return out;
        }
    }
    if (out.text) out.kind = SLEEP_COMMAND;
    return out;
}
void sleep_step_free(SleepStep *s) { free(s->text); memset(s, 0, sizeof *s); }
static void pane_free(SleepPane *p) {
    session_free(&p->session); free(p->path); free(p->pending); free(p->command); free(p->content); memset(p, 0, sizeof *p);
}
void sleep_wall_free(SleepWall *w) {
    for (int i = 0; i < SLEEP_PANES; i++) pane_free(&w->panes[i]);
    for (size_t i = 0; i < w->commands; i++) { free(w->ticker[i].project); free(w->ticker[i].text); }
    uint64_t serial = w->serial;
    memset(w, 0, sizeof *w); w->serial = serial;
}
void sleep_remove(SleepWall *w, const char *id) {
    for (int i = 0; i < SLEEP_PANES; i++) if (str_eq(session_id(&w->panes[i].session), id)) pane_free(&w->panes[i]);
}
static void drop_replay(SleepWall *w) {
    for (int i = 0; i < SLEEP_PANES; i++) if (w->panes[i].replay) pane_free(&w->panes[i]);
}
int sleep_assign(SleepWall *w, const Session *s, uint64_t now, bool replay) {
    if (str_empty(session_id(s)) || str_empty(session_repo(s))) return -1;
    if (!replay) drop_replay(w);
    int slot = 0;
    for (int i = 0; i < SLEEP_PANES; i++) {
        SleepPane *p = &w->panes[i];
        if (str_eq(session_id(&p->session), session_id(s))) {
            bool changed = !str_eq(session_repo(s), session_repo(&p->session));
            if (!str_eq(json_str(json_get(s->raw, "branch")), json_str(json_get(p->session.raw, "branch")))) changed = true;
            if (changed || p->replay != replay) { pane_free(p); p->revision = p->identity = ++w->serial; p->touched = now; p->replay = replay; }
            session_free(&p->session); session_copy(&p->session, s); return i;
        }
        if (!p->session.raw) slot = i;
        else if (w->panes[slot].session.raw && p->touched < w->panes[slot].touched) slot = i;
    }
    SleepPane *p = &w->panes[slot]; pane_free(p); session_copy(&p->session, s);
    p->revision = p->identity = ++w->serial; p->touched = now; p->replay = replay; return slot;
}
bool sleep_replay_due(const SleepWall *w, uint64_t now) {
    return !w->last_live || now - w->last_live >= 60000;
}
static void set_text(char **slot, const char *value) {
    char *copy = xstrdup(value); free(*slot); *slot = copy;
}
static void take_file(SleepWall *w, SleepPane *p, const char *path, bool writes, uint64_t now) {
    set_text(&p->path, path); free(p->pending); p->pending = NULL;
    free(p->content); p->content = NULL; p->loaded = false; p->typed = 0;
    p->writes = writes; p->since = now; p->revision = ++w->serial;
}
static bool can_switch(const SleepPane *p, bool writes, uint64_t now) {
    return !p->path || (p->loaded && p->typed >= (p->content ? strlen(p->content) : 0)) || now - p->since >= 6000 || (writes && !p->writes);
}
void sleep_apply(SleepWall *w, int index, const SleepStep *step, uint64_t now, bool replay) {
    if (index < 0 || index >= SLEEP_PANES || step->kind == SLEEP_NONE) return;
    SleepPane *p = &w->panes[index]; if (!p->session.raw || p->replay != replay) return;
    if (!replay) w->last_live = now;
    p->touched = now;
    if (step->kind == SLEEP_FILE) {
        if (str_eq(p->path, step->text)) { p->writes |= step->writes; return; }
        if (can_switch(p, step->writes, now)) take_file(w, p, step->text, step->writes, now);
        else if (!p->pending_writes || step->writes || !p->pending) { set_text(&p->pending, step->text); p->pending_writes = step->writes; }
    } else {
        set_text(&p->command, step->text);
        if (w->commands == SLEEP_TICKER) {
            free(w->ticker[0].project); free(w->ticker[0].text);
            memmove(w->ticker, w->ticker + 1, (SLEEP_TICKER - 1) * sizeof *w->ticker); w->commands--;
        }
        const char *repo = session_repo(&p->session), *name = strrchr(repo, '/');
        w->ticker[w->commands++] = (SleepCommand){xstrdup(name ? name + 1 : repo), xstrdup(step->text), replay};
    }
}
void sleep_tick(SleepWall *w, uint64_t now) {
    for (int i = 0; i < SLEEP_PANES; i++) {
        SleepPane *p = &w->panes[i];
        if (p->content) {
            size_t n = strlen(p->content); p->typed += 7; if (p->typed > n) p->typed = n;
            while (p->typed < n && ((unsigned char)p->content[p->typed] & 0xc0) == 0x80) p->typed++;
        }
        if (p->pending && can_switch(p, p->pending_writes, now)) {
            char *path = xstrdup(p->pending); take_file(w, p, path, p->pending_writes, now); free(path);
        }
    }
}
Json *sleep_file_args(const SleepPane *p, bool fallback) {
    Json *args = json_object(); json_set_str(args, "repo", session_repo(&p->session)); json_set_str(args, "path", p->path);
    const char *branch = json_str_nonempty(json_get(p->session.raw, "branch"));
    if (!fallback && branch) json_set_str(args, "ref", branch);
    return args;
}
bool sleep_loaded(SleepPane *p, uint64_t revision, const Json *answer) {
    if (p->revision != revision || !p->path) return false;
    RepoFile file = {0}; p->loaded = true;
    if (!repo_file_parse(answer, &file)) return true;
    if (file.content && !file.binary && !file.too_large && strlen(file.content) <= SLEEP_TEXT) {
        size_t n = 0; int lines = 0;
        while (file.content[n]) { if (file.content[n++] == '\n' && ++lines == 160) break; }
        set_text(&p->content, NULL); p->content = xstrndup(file.content, n); p->typed = 0;
    }
    repo_file_free(&file); return true;
}
static time_t recency(const Session *s) {
    time_t t = 0;
    if (board_date_parse(json_str(json_get(s->raw, "updatedAt")), &t)) return t;
    if (board_date_parse(json_str(json_get(s->raw, "endedAt")), &t)) return t;
    board_date_parse(json_str(json_get(s->raw, "createdAt")), &t); return t;
}
size_t sleep_select(const Session *items, size_t count, size_t out[SLEEP_PANES], bool working) {
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (str_empty(session_repo(&items[i]))) continue;
        if (working && !session_is_active(&items[i])) continue;
        time_t time = recency(&items[i]);
        size_t pos = n;
        while (pos && recency(&items[out[pos - 1]]) < time) pos--;
        if (pos >= SLEEP_PANES) continue;
        if (n < SLEEP_PANES) n++;
        for (size_t j = n - 1; j > pos; j--) out[j] = out[j - 1];
        out[pos] = i;
    }
    return n;
}

void sleep_replay_clear(SleepReplay *r, int pane) {
    for (size_t k = 0; k < r->count[pane]; k++) sleep_step_free(&r->steps[pane][k]);
    r->count[pane] = r->position[pane] = 0;
}
void sleep_replay_load(SleepReplay *r, int pane, const Json *events, const char *repo) {
    sleep_replay_clear(r, pane);
    // Walk backwards so even a long transcript retains only the latest useful steps.
    for (size_t k = json_count(events); k > 0 && r->count[pane] < SLEEP_STEPS; k--) {
        SleepStep step = sleep_step(json_at(events, k - 1), repo);
        if (step.kind != SLEEP_NONE) r->steps[pane][r->count[pane]++] = step;
    }
    size_t n = r->count[pane];
    for (size_t k = 0; k < n / 2; k++) {
        SleepStep swap = r->steps[pane][k]; r->steps[pane][k] = r->steps[pane][n - k - 1]; r->steps[pane][n - k - 1] = swap;
    }
}
const SleepStep *sleep_replay_next(SleepReplay *r, int *pane) {
    for (int k = 0; k < SLEEP_PANES; k++) {
        size_t i = r->turn++ % SLEEP_PANES;
        if (!r->count[i]) continue;
        *pane = (int)i; size_t at = r->position[i];
        r->position[i] = (at + 1) % r->count[i]; return &r->steps[i][at];
    }
    return NULL;
}

void sleep_seed(SleepWall *w, int pane, const Json *events, uint64_t now) {
    SleepPane *p = &w->panes[pane];
    const Json *latest[8]; size_t count = 0;
    for (size_t k = json_count(events); k > 0 && count < 8; k--) {
        const Json *e = json_at(events, k - 1); SleepStep step = sleep_step(e, session_repo(&p->session));
        if (step.kind != SLEEP_NONE) latest[count++] = e;
        sleep_step_free(&step);
    }
    uint64_t live = w->last_live;
    while (count) {
        const Json *e = latest[--count]; int seq = json_int_or(json_get(e, "seq"), 0);
        SleepStep step = sleep_step(e, session_repo(&p->session));
        int *cursor = step.kind == SLEEP_FILE ? &p->file_cursor : &p->command_cursor;
        if (seq > *cursor) { *cursor = seq; sleep_apply(w, pane, &step, now, p->replay); }
        sleep_step_free(&step);
    }
    w->last_live = live; p->seeded = true;
}
