// SFTP sessions: sftp.exe with its standard input and output on pipes (no console, so ssh asks for passwords and host
// keys through SSH_ASKPASS, which is this program), a reader thread per session that hands what it prints to the UI
// thread through a message-only window, and a queue of commands written one at a time. After each command the session
// writes `version`, whose echo and answer mark where the command's own output ended: sftp echoes every command it reads
// from a pipe as "sftp> <command>", and its errors arrive on the same pipe, in order. Each session runs in a job object,
// so closing it (or the app) ends sftp and the ssh it started.
#include "sftp_session.h"
#include "dialogs.h"
#include "screens.h"
#include "sftp.h"
#include "str.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { READ_CHUNK = 16384, MAX_LISTENERS = 16, LOG_LINES = 12 };
enum { WM_SFTP_DATA = WM_APP + 51, WM_SFTP_EOF };
static const wchar_t EVENTS_CLASS[] = L"BriareusSftpEvents";

typedef enum { C_PWD, C_LIST, C_GET, C_PUT, C_MKDIR, C_RENAME, C_RM, C_RMDIR } CmdKind;
typedef struct Cmd {
    CmdKind kind;
    char *line;           // what is written to sftp, without the newline
    char *path, *path2;   // the remote path it is about (and a rename's new one, an upload's folder)
    wchar_t *local;       // a transfer's local path
    char *what;           // what the header says while it runs, for transfers and changes
    struct Cmd *next;
} Cmd;

struct SftpSession {
    int id, spawn;
    char *key, *group, *label, *user, *host, *target;
    int port;
    HANDLE process, job, in_write;
    SftpState state;
    Str partial;                    // a line still arriving
    char *log[LOG_LINES]; int log_count;
    Cmd *cmd, *queue;               // the command running, and those waiting behind it
    int phase;                      // 0: its echo is awaited, 1: its output, 2: the marker's answer
    Str out;                        // the running command's output lines
    char *home, *selected, *reveal;
    SftpNode *root;
    char *status; bool status_failed;
    wchar_t *last_download;
};

typedef struct { int session, spawn; size_t len; char data[]; } Chunk;
typedef struct { int session, spawn; HANDLE out_read; } ReaderArgs;

static SftpSession **g_sessions; static size_t g_count, g_cap;
static SftpSession *g_active;
static int g_next_id = 1, g_next_spawn = 1;
static HWND g_events;
static struct { void (*fn)(void *); void *ctx; } g_listeners[MAX_LISTENERS];

static void notify(void) {
    for (int i = 0; i < MAX_LISTENERS; i++) if (g_listeners[i].fn) g_listeners[i].fn(g_listeners[i].ctx);
}
void sftp_add_listener(void (*fn)(void *ctx), void *ctx) {
    for (int i = 0; i < MAX_LISTENERS; i++) if (!g_listeners[i].fn) { g_listeners[i].fn = fn; g_listeners[i].ctx = ctx; return; }
}
void sftp_remove_listener(void (*fn)(void *ctx), void *ctx) {
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (g_listeners[i].fn == fn && g_listeners[i].ctx == ctx) { g_listeners[i].fn = NULL; g_listeners[i].ctx = NULL; }
}
static SftpSession *by_id(int id) {
    for (size_t i = 0; i < g_count; i++) if (g_sessions[i]->id == id) return g_sessions[i];
    return NULL;
}

// MARK: - The tree

static SftpNode *node_new(const char *name, const char *path, bool dir) {
    SftpNode *n = xcalloc(1, sizeof *n);
    n->name = xstrdup(name); n->path = xstrdup(path); n->dir = dir;
    return n;
}
static void node_free(SftpNode *n) {
    if (!n) return;
    for (size_t i = 0; i < n->kid_count; i++) node_free(n->kids[i]);
    free(n->kids); free(n->name); free(n->path); free(n->when); free(n->error);
    free(n);
}
static SftpNode *find_in(SftpNode *n, const char *path) {
    if (!n) return NULL;
    if (str_eq(n->path, path)) return n;
    if (!sftp_path_within(path, n->path)) return NULL;
    for (size_t i = 0; i < n->kid_count; i++) { SftpNode *f = find_in(n->kids[i], path); if (f) return f; }
    return NULL;
}
SftpNode *sftp_node(SftpSession *s, const char *path) { return path ? find_in(s->root, path) : NULL; }
SftpNode *sftp_root(SftpSession *s) { return s->root; }
/// Folders (and links, which may be folders) first, then by name, case aside.
static int node_cmp(const void *a, const void *b) {
    const SftpNode *x = *(SftpNode *const *)a, *y = *(SftpNode *const *)b;
    bool fx = x->dir || x->link, fy = y->dir || y->link;
    if (fx != fy) return fx ? -1 : 1;
    int c = _stricmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

// MARK: - Commands

static void cmd_free(Cmd *c) {
    if (!c) return;
    free(c->line); free(c->path); free(c->path2); free(c->local); free(c->what);
    free(c);
}
static void write_line(SftpSession *s, const char *line) {
    if (!s->in_write) return;
    char *text = xstrfmt("%s\nversion\n", line);
    DWORD wrote;
    if (!WriteFile(s->in_write, text, (DWORD)strlen(text), &wrote, NULL)) { CloseHandle(s->in_write); s->in_write = NULL; }
    free(text);
}
/// Starts the next command when none runs.
static void pump(SftpSession *s) {
    if (s->cmd || !s->queue || s->state == SFTP_CLOSED) return;
    s->cmd = s->queue; s->queue = s->cmd->next; s->cmd->next = NULL;
    s->phase = 0;
    str_free(&s->out); str_init(&s->out);
    write_line(s, s->cmd->line);
}
static Cmd *enqueue(SftpSession *s, CmdKind kind, char *line, const char *path, const char *path2, const wchar_t *local, char *what) {
    if (!line) { free(what); return NULL; }
    Cmd *c = xcalloc(1, sizeof *c);
    c->kind = kind; c->line = line; c->what = what;
    c->path = path ? xstrdup(path) : NULL; c->path2 = path2 ? xstrdup(path2) : NULL;
    c->local = local ? _wcsdup(local) : NULL;
    Cmd **tail = &s->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = c;
    pump(s);
    return c;
}
/// `<verb> <quoted> [<quoted>]`, or NULL when a path cannot be written on sftp's command line.
static char *command(const char *verb, const char *a, const char *b) {
    char *qa = sftp_quote(a), *qb = b ? sftp_quote(b) : NULL;
    char *line = qa && (!b || qb) ? (b ? xstrfmt("%s %s %s", verb, qa, qb) : xstrfmt("%s %s", verb, qa)) : NULL;
    free(qa); free(qb);
    return line;
}
/// A local path as sftp reads it: forward slashes (both Git's MSYS sftp and Windows' own take "C:/...").
static char *local_arg(const wchar_t *local) {
    char *u = wide_to_utf8(local);
    for (char *p = u; *p; p++) if (*p == '\\') *p = '/';
    return u;
}
static void relist(SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (n && (n->loaded || n->expanded) && !n->loading) sftp_list(s, path);
}

void sftp_list(SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n || n->loading || s->state == SFTP_CLOSED) return;
    // The trailing slash lists a link's target folder, and fails for a link to a file.
    char *dir = str_eq(path, "/") ? xstrdup("/") : xstrfmt("%s/", path);
    char *line = command("ls -la", dir, NULL);
    free(dir);
    if (!line) { set_string(&n->error, "This name cannot be given to sftp."); n->loaded = true; return; }
    n->loading = true;
    enqueue(s, C_LIST, line, path, NULL, NULL, NULL);
}
void sftp_toggle(SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n || !(n->dir || n->link)) return;
    n->expanded = !n->expanded;
    if (n->expanded && !n->loaded) sftp_list(s, path);
    notify();
}
/// The entry of `n` on the way down to `path`. When the listing does not show it (a folder that can be entered but not
/// listed, as Linux's /home can be, or a mount point such as Git's /c) it is added, so the tree still reaches it.
static SftpNode *step_toward(SftpNode *n, const char *path) {
    for (size_t i = 0; i < n->kid_count; i++) if (sftp_path_within(path, n->kids[i]->path)) return n->kids[i];
    const char *rest = path + strlen(n->path);
    while (*rest == '/') rest++;
    if (!*rest) return NULL;
    const char *end = strchr(rest, '/');
    char *name = end ? xstrndup(rest, (size_t)(end - rest)) : xstrdup(rest);
    char *full = sftp_join(n->path, name);
    SftpNode *k = node_new(name, full, true);
    free(name); free(full);
    n->kids = xrealloc(n->kids, (n->kid_count + 1) * sizeof *n->kids);
    n->kids[n->kid_count++] = k;
    qsort(n->kids, n->kid_count, sizeof *n->kids, node_cmp);
    return k;
}
/// Opens the folders down to `s->reveal` as far as they are listed, listing the next one; done once it is reached.
static void reveal_more(SftpSession *s) {
    SftpNode *n = s->root;
    while (n && s->reveal) {
        n->expanded = true;
        if (!n->loaded) { sftp_list(s, n->path); return; }
        if (str_eq(n->path, s->reveal)) { set_string(&s->reveal, NULL); return; }
        n = step_toward(n, s->reveal);
    }
}
void sftp_reveal(SftpSession *s, const char *path) {
    if (!path) return;
    set_string(&s->selected, path);
    set_string(&s->reveal, path);
    reveal_more(s);
    notify();
}

void sftp_download(SftpSession *s, const char *remote, const wchar_t *local, bool folder) {
    char *l = local_arg(local);
    char *line = command(folder ? "get -r" : "get", remote, l);
    free(l);
    char *what = xstrfmt("Downloading %s\xE2\x80\xA6", sftp_basename(remote));
    if (!enqueue(s, C_GET, line, remote, NULL, local, what)) { set_string(&s->status, "That name cannot be given to sftp."); s->status_failed = true; }
    notify();
}
void sftp_upload(SftpSession *s, const wchar_t *local, const char *remote_dir, bool folder) {
    char *l = local_arg(local);
    const char *base = strrchr(l, '/');
    char *target = sftp_join(remote_dir, base ? base + 1 : l);
    char *line = command(folder ? "put -r" : "put", l, target);
    char *what = xstrfmt("Uploading %s\xE2\x80\xA6", base ? base + 1 : l);
    if (!enqueue(s, C_PUT, line, target, remote_dir, local, what)) { set_string(&s->status, "That name cannot be given to sftp."); s->status_failed = true; }
    free(l); free(target);
    notify();
}
void sftp_mkdir(SftpSession *s, const char *path) {
    enqueue(s, C_MKDIR, command("mkdir", path, NULL), path, NULL, NULL, xstrfmt("Creating %s\xE2\x80\xA6", sftp_basename(path)));
    notify();
}
void sftp_rename(SftpSession *s, const char *from, const char *to) {
    enqueue(s, C_RENAME, command("rename", from, to), from, to, NULL, xstrfmt("Renaming %s\xE2\x80\xA6", sftp_basename(from)));
    notify();
}
void sftp_remove(SftpSession *s, const char *path, bool folder) {
    enqueue(s, folder ? C_RMDIR : C_RM, command(folder ? "rmdir" : "rm", path, NULL), path, NULL, NULL, xstrfmt("Deleting %s\xE2\x80\xA6", sftp_basename(path)));
    notify();
}

// MARK: - Answers

/// The lines sftp prints about a transfer or a change going well; anything else it printed is a complaint.
static bool chatter(const char *line) {
    static const char *const ok[] = { "Fetching ", "Retrieving ", "Uploading ", "Entering ", "Removing ", "Renaming " };
    if (!*line) return true;
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (str_has_prefix(line, ok[i])) return true;
    return false;
}
/// The complaints in a command's output, one per line; NULL when there were none.
static char *complaints(const char *out) {
    Str e; str_init(&e);
    size_t n; char **lines = str_split(out ? out : "", '\n', &n);
    for (size_t i = 0; i < n; i++) if (!chatter(lines[i])) { if (e.len) str_appendc(&e, '\n'); str_appendz(&e, lines[i]); }
    str_array_free(lines, n);
    if (!e.len) { str_free(&e); return NULL; }
    return str_detach(&e);
}
static void set_status(SftpSession *s, char *text, bool failed) {
    free(s->status); s->status = text; s->status_failed = failed;
}

static void list_done(SftpSession *s, Cmd *c, const char *out) {
    SftpNode *n = sftp_node(s, c->path);
    if (!n) return;
    n->loading = false; n->loaded = true;
    size_t count; char **lines = str_split(out ? out : "", '\n', &count);
    SftpNode **kids = NULL; size_t kn = 0;
    Str errors; str_init(&errors);
    for (size_t i = 0; i < count; i++) {
        SftpEntry e;
        if (sftp_parse_entry(lines[i], &e)) {
            // A folder listed again keeps what was open under it.
            SftpNode *k = NULL;
            for (size_t j = 0; j < n->kid_count; j++)
                if (n->kids[j] && str_eq(n->kids[j]->name, e.name) && n->kids[j]->dir == e.dir) { k = n->kids[j]; n->kids[j] = NULL; break; }
            if (!k) { char *path = sftp_join(n->path, e.name); k = node_new(e.name, path, e.dir); free(path); }
            k->link = e.link; k->size = e.size;
            set_string(&k->when, e.when);
            kids = xrealloc(kids, (kn + 1) * sizeof *kids); kids[kn++] = k;
            sftp_entry_free(&e);
        } else if (*lines[i] && !str_has_prefix(lines[i], "total ")) {
            if (errors.len) str_appendc(&errors, '\n');
            str_appendz(&errors, lines[i]);
        }
    }
    str_array_free(lines, count);
    for (size_t j = 0; j < n->kid_count; j++) {
        // An entry the listing does not show but the way home passes (see step_toward) stays.
        if (n->kids[j] && s->home && sftp_path_within(s->home, n->kids[j]->path)) { kids = xrealloc(kids, (kn + 1) * sizeof *kids); kids[kn++] = n->kids[j]; }
        else node_free(n->kids[j]);
    }
    free(n->kids);
    if (kids) qsort(kids, kn, sizeof *kids, node_cmp);
    n->kids = kids; n->kid_count = kn;
    if (!kn && errors.len && n->link) {
        // A link to a file: it has nothing to open.
        n->expanded = false; n->link = true;
        set_string(&n->error, NULL);
    } else set_string(&n->error, !kn && errors.len ? errors.data : NULL);
    str_free(&errors);
    // Opening the way down to a folder asked for.
    if (s->reveal && sftp_path_within(s->reveal, n->path)) reveal_more(s);
}

static void finish(SftpSession *s) {
    Cmd *c = s->cmd;
    s->cmd = NULL;
    char *out = str_detach(&s->out); str_init(&s->out);
    switch (c->kind) {
    case C_PWD: {
        const char *mark = out ? strstr(out, "Remote working directory: ") : NULL;
        char *home = NULL;
        if (mark) { mark += strlen("Remote working directory: "); const char *e = strchr(mark, '\n'); home = e ? xstrndup(mark, (size_t)(e - mark)) : xstrdup(mark); }
        if (!home || home[0] != '/') { free(home); home = xstrdup("/"); }
        free(s->home); s->home = home;
        node_free(s->root);
        s->root = node_new("/", "/", true);
        sftp_reveal(s, home);
        break;
    }
    case C_LIST: list_done(s, c, out); break;
    case C_GET: {
        char *why = complaints(out);
        char *local = wide_to_utf8(c->local);
        if (why) set_status(s, xstrfmt("Could not download %s: %s", sftp_basename(c->path), why), true);
        else {
            set_status(s, xstrfmt("Downloaded %s to %s", sftp_basename(c->path), local), false);
            free(s->last_download); s->last_download = _wcsdup(c->local);
        }
        free(why); free(local);
        break;
    }
    case C_PUT: {
        char *why = complaints(out);
        if (why) set_status(s, xstrfmt("Could not upload %s: %s", sftp_basename(c->path), why), true);
        else set_status(s, xstrfmt("Uploaded %s to %s", sftp_basename(c->path), c->path2), false);
        free(why);
        relist(s, c->path2);
        break;
    }
    case C_MKDIR: case C_RENAME: case C_RM: case C_RMDIR: {
        char *why = complaints(out);
        static const char *const verbs[] = { [C_MKDIR] = "create", [C_RENAME] = "rename", [C_RM] = "delete", [C_RMDIR] = "delete" };
        static const char *const done[] = { [C_MKDIR] = "Created", [C_RENAME] = "Renamed", [C_RM] = "Deleted", [C_RMDIR] = "Deleted" };
        if (why) set_status(s, xstrfmt("Could not %s %s: %s", verbs[c->kind], sftp_basename(c->path), why), true);
        else if (c->kind == C_RENAME) set_status(s, xstrfmt("Renamed %s to %s", sftp_basename(c->path), sftp_basename(c->path2)), false);
        else set_status(s, xstrfmt("%s %s", done[c->kind], c->path), false);
        free(why);
        if (!why && (c->kind == C_RM || c->kind == C_RMDIR || c->kind == C_RENAME) && s->selected && sftp_path_within(s->selected, c->path)) {
            char *parent = sftp_parent(c->path);
            free(s->selected); s->selected = c->kind == C_RENAME ? xstrdup(c->path2) : parent; if (c->kind == C_RENAME) free(parent);
        }
        char *parent = sftp_parent(c->path);
        relist(s, parent);
        if (c->kind == C_RENAME) { char *to = sftp_parent(c->path2); if (!str_eq(to, parent)) relist(s, to); free(to); }
        free(parent);
        break;
    }
    }
    free(out);
    cmd_free(c);
    pump(s);
    notify();
}

static void log_line(SftpSession *s, const char *line) {
    if (!*line) return;
    if (s->log_count == LOG_LINES) { free(s->log[0]); memmove(s->log, s->log + 1, (LOG_LINES - 1) * sizeof *s->log); s->log_count--; }
    s->log[s->log_count++] = xstrdup(line);
}
static void on_line(SftpSession *s, const char *line) {
    if (str_has_prefix(line, "sftp> ")) {
        if (s->cmd && s->phase == 0) {
            s->phase = 1;
            if (s->state == SFTP_CONNECTING) { s->state = SFTP_READY; notify(); }
            return;
        }
        if (s->cmd && s->phase == 1 && str_eq(line + 6, "version")) { s->phase = 2; return; }
    }
    if (s->cmd && s->phase == 2 && str_has_prefix(line, "SFTP protocol version")) { finish(s); return; }
    if (s->cmd && s->phase == 1) { if (s->out.len) str_appendc(&s->out, '\n'); str_appendz(&s->out, line); return; }
    log_line(s, line);
}
static void on_data(SftpSession *s, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            char *line = str_detach(&s->partial); str_init(&s->partial);
            if (!line) line = xstrdup("");
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            on_line(s, line);
            free(line);
        } else str_appendc(&s->partial, data[i]);
    }
}

/// The program ended: what it was doing is dropped, the tree stays to look at until a reconnect.
static void ended(SftpSession *s) {
    if (s->partial.len) { char *rest = str_detach(&s->partial); str_init(&s->partial); on_line(s, rest); free(rest); }
    bool was_ready = s->state == SFTP_READY;
    s->state = SFTP_CLOSED;
    if (s->in_write) { CloseHandle(s->in_write); s->in_write = NULL; }
    if (s->job) { CloseHandle(s->job); s->job = NULL; }
    if (s->process) { CloseHandle(s->process); s->process = NULL; }
    if (s->cmd && s->cmd->what) set_status(s, xstrfmt("%s stopped: the connection closed.", s->cmd->what), true);
    else if (was_ready) set_status(s, xstrdup("The connection closed."), true);
    cmd_free(s->cmd); s->cmd = NULL;
    while (s->queue) { Cmd *next = s->queue->next; cmd_free(s->queue); s->queue = next; }
    // Folders waiting for a listing stop spinning.
    SftpNode *stack[256]; int top = 0;
    if (s->root) stack[top++] = s->root;
    while (top) {
        SftpNode *n = stack[--top];
        n->loading = false;
        for (size_t i = 0; i < n->kid_count && top < 256; i++) stack[top++] = n->kids[i];
    }
    notify();
}

// MARK: - The program

static LRESULT CALLBACK events_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SFTP_DATA: {
        Chunk *c = (Chunk *)lp;
        SftpSession *s = by_id(c->session);
        if (s && s->spawn == c->spawn) on_data(s, c->data, c->len);
        free(c);
        return 0;
    }
    case WM_SFTP_EOF: {
        SftpSession *s = by_id((int)wp);
        if (s && s->spawn == (int)lp && s->state != SFTP_CLOSED) ended(s);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static bool ensure_events(void) {
    if (g_events) return true;
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = events_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = EVENTS_CLASS;
    RegisterClassExW(&wc);
    g_events = CreateWindowExW(0, EVENTS_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    return g_events != NULL;
}

static DWORD WINAPI reader_main(void *param) {
    ReaderArgs *a = param;
    char *buf = xmalloc(READ_CHUNK);
    DWORD got;
    while (ReadFile(a->out_read, buf, READ_CHUNK, &got, NULL) && got) {
        Chunk *c = malloc(sizeof *c + got);
        if (!c) break;
        c->session = a->session; c->spawn = a->spawn; c->len = got;
        memcpy(c->data, buf, got);
        if (!PostMessageW(g_events, WM_SFTP_DATA, 0, (LPARAM)c)) free(c);
    }
    PostMessageW(g_events, WM_SFTP_EOF, (WPARAM)a->session, (LPARAM)a->spawn);
    CloseHandle(a->out_read);
    free(buf); free(a);
    return 0;
}

/// This process's environment with ssh pointed at Briareus.exe for its prompts, and sftp's output in UTF-8.
wchar_t *sftp_child_environment(void) {
    static const wchar_t *const drop[] = { L"SSH_ASKPASS=", L"SSH_ASKPASS_REQUIRE=", L"BRIAREUS_ASKPASS=", L"LC_ALL=" };
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t extra[MAX_PATH + 128];
    int extra_len = _snwprintf(extra, MAX_PATH + 128, L"SSH_ASKPASS=%ls%lcSSH_ASKPASS_REQUIRE=force%lcBRIAREUS_ASKPASS=1%lcLC_ALL=C.UTF-8%lc", exe, 0, 0, 0, 0);
    if (extra_len < 0) return NULL;
    wchar_t *env = GetEnvironmentStringsW();
    size_t total = 0;
    for (const wchar_t *p = env; p && *p; p += wcslen(p) + 1) total += wcslen(p) + 1;
    wchar_t *out = xmalloc((total + (size_t)extra_len + 2) * sizeof *out), *w = out;
    for (const wchar_t *p = env; p && *p; p += wcslen(p) + 1) {
        bool skip = false;
        for (size_t i = 0; i < sizeof drop / sizeof *drop; i++) if (!_wcsnicmp(p, drop[i], wcslen(drop[i]))) skip = true;
        if (skip) continue;
        size_t n = wcslen(p) + 1;
        memcpy(w, p, n * sizeof *w); w += n;
    }
    if (env) FreeEnvironmentStringsW(env);
    memcpy(w, extra, (size_t)extra_len * sizeof *w); w += extra_len;
    *w = 0;
    return out;
}

static bool spawn(SftpSession *s, char **error) {
    if (!ensure_events()) { *error = xstrdup("Could not create the SFTP session's message window."); return false; }
    wchar_t client[MAX_PATH];
    if (!term_find_program(L"sftp.exe", client, MAX_PATH)) {
        *error = xstrdup("sftp.exe was not found. Install Git for Windows, or the OpenSSH Client under Settings \xE2\x86\x92 System \xE2\x86\x92 Optional features.");
        return false;
    }
    TermTarget target = { s->key, s->group, s->label, s->user, s->host, s->port };
    char *problem = term_target_problem(&target);
    if (problem) { *error = problem; return false; }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    if (!CreatePipe(&in_read, &in_write, &sa, 65536) || !CreatePipe(&out_read, &out_write, &sa, 65536)) {
        *error = xstrdup("Could not create the pipes for sftp.");
        if (in_read) { CloseHandle(in_read); CloseHandle(in_write); }
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    // Only the two pipe ends go to sftp, whatever else this process holds open.
    HANDLE inherit[2] = { in_read, out_write };
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    STARTUPINFOEXW si; ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read; si.StartupInfo.hStdOutput = out_write; si.StartupInfo.hStdError = out_write;
    si.lpAttributeList = xmalloc(attr_size);
    bool ok = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)
           && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    char *exe = wide_to_utf8(client);
    char *line = xstrfmt("\"%s\" -P %d -o User=%s -o ServerAliveInterval=30 -- %s", exe, s->port, s->user, s->host);
    wchar_t *cmd = utf8_to_wide(line);
    free(exe); free(line);
    wchar_t *env = sftp_child_environment();
    wchar_t *dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &dir))) dir = NULL;
    // ssh's prompts are a process of their own, started from the background: let them come to the front.
    AllowSetForegroundWindow(ASFW_ANY);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    DWORD create_error = 0;
    if (ok) {
        ok = CreateProcessW(client, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                            env, dir, &si.StartupInfo, &pi);
        if (!ok) create_error = GetLastError();
    }
    free(cmd); free(env);
    if (dir) CoTaskMemFree(dir);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    CloseHandle(in_read); CloseHandle(out_write);
    if (!ok) {
        CloseHandle(in_write); CloseHandle(out_read);
        *error = xstrfmt("Could not start sftp.exe (error %lu).", (unsigned long)create_error);
        return false;
    }
    // Everything sftp starts (ssh, the prompts) ends with the job.
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits; ZeroMemory(&limits, sizeof limits);
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) || !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job); job = NULL;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    s->spawn = g_next_spawn++;
    s->process = pi.hProcess; s->job = job; s->in_write = in_write;
    s->state = SFTP_CONNECTING;
    for (int i = 0; i < s->log_count; i++) free(s->log[i]);
    s->log_count = 0;
    str_free(&s->partial); str_init(&s->partial);
    ReaderArgs *a = xmalloc(sizeof *a);
    a->session = s->id; a->spawn = s->spawn; a->out_read = out_read;
    HANDLE th = CreateThread(NULL, 0, reader_main, a, 0, NULL);
    if (th) CloseHandle(th);
    else { CloseHandle(out_read); free(a); }
    // The first command waits in the pipe until the connection is up; its echo says it is.
    enqueue(s, C_PWD, xstrdup("pwd"), NULL, NULL, NULL, NULL);
    return true;
}

static void stop(SftpSession *s) {
    if (s->in_write) { CloseHandle(s->in_write); s->in_write = NULL; }
    if (s->job) { CloseHandle(s->job); s->job = NULL; }
    else if (s->process) TerminateProcess(s->process, 1);
    if (s->process) { CloseHandle(s->process); s->process = NULL; }
    s->spawn = 0;
}

SftpSession *sftp_open(const TermTarget *target, char **error) {
    *error = NULL;
    SftpSession *s = xcalloc(1, sizeof *s);
    s->id = g_next_id++;
    s->key = xstrdup(target->key ? target->key : ""); s->group = xstrdup(target->group ? target->group : "");
    s->label = xstrdup(target->label ? target->label : target->host ? target->host : "SFTP");
    s->user = xstrdup(target->user ? target->user : ""); s->host = xstrdup(target->host ? target->host : "");
    s->port = target->port;
    s->target = xstrfmt("%s@%s:%d", s->user, s->host, s->port);
    str_init(&s->partial); str_init(&s->out);
    if (!spawn(s, error)) {
        free(s->key); free(s->group); free(s->label); free(s->user); free(s->host); free(s->target);
        free(s);
        return NULL;
    }
    if (g_count == g_cap) { g_cap = g_cap ? g_cap * 2 : 8; g_sessions = xrealloc(g_sessions, g_cap * sizeof *g_sessions); }
    g_sessions[g_count++] = s;
    g_active = s;
    notify();
    return s;
}
bool sftp_reconnect(SftpSession *s, char **error) {
    *error = NULL;
    if (!s || s->state != SFTP_CLOSED) return true;
    stop(s);
    node_free(s->root); s->root = NULL;
    set_status(s, NULL, false);
    if (!spawn(s, error)) return false;
    notify();
    return true;
}
static void destroy_session(SftpSession *s) {
    stop(s);
    cmd_free(s->cmd);
    while (s->queue) { Cmd *next = s->queue->next; cmd_free(s->queue); s->queue = next; }
    for (int i = 0; i < s->log_count; i++) free(s->log[i]);
    node_free(s->root);
    str_free(&s->partial); str_free(&s->out);
    free(s->key); free(s->group); free(s->label); free(s->user); free(s->host); free(s->target);
    free(s->home); free(s->selected); free(s->reveal); free(s->status); free(s->last_download);
    free(s);
}
void sftp_close(SftpSession *s) {
    if (!s) return;
    size_t at = g_count;
    for (size_t i = 0; i < g_count; i++) if (g_sessions[i] == s) at = i;
    if (at == g_count) return;
    memmove(g_sessions + at, g_sessions + at + 1, (g_count - at - 1) * sizeof *g_sessions);
    g_count--;
    if (g_active == s) {
        // The next session of the same project, else the one before it.
        g_active = NULL;
        for (size_t i = at; i < g_count && !g_active; i++) if (str_eq(g_sessions[i]->group, s->group)) g_active = g_sessions[i];
        for (size_t i = at; i-- > 0 && !g_active;) if (str_eq(g_sessions[i]->group, s->group)) g_active = g_sessions[i];
    }
    destroy_session(s);
    notify();
}
void sftp_shutdown(void) {
    for (size_t i = 0; i < g_count; i++) destroy_session(g_sessions[i]);
    free(g_sessions); g_sessions = NULL; g_count = g_cap = 0; g_active = NULL;
}

// MARK: - Accessors

size_t sftp_count(void) { return g_count; }
SftpSession *sftp_at(size_t index) { return index < g_count ? g_sessions[index] : NULL; }
SftpSession *sftp_find(const char *key) {
    for (size_t i = 0; i < g_count; i++) if (str_eq(g_sessions[i]->key, key)) return g_sessions[i];
    return NULL;
}
size_t sftp_count_for(const char *key) {
    size_t n = 0;
    for (size_t i = 0; i < g_count; i++) if (str_eq(g_sessions[i]->key, key)) n++;
    return n;
}
size_t sftp_live_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_count; i++) if (g_sessions[i]->state != SFTP_CLOSED) n++;
    return n;
}
SftpSession *sftp_active(void) { return g_active; }
void sftp_set_active(SftpSession *s) { if (s && s != g_active) { g_active = s; notify(); } }
const char *sftp_group(const SftpSession *s) { return s->group; }
const char *sftp_key(const SftpSession *s) { return s->key; }
const char *sftp_label(const SftpSession *s) { return s->label; }
const char *sftp_target(const SftpSession *s) { return s->target; }
SftpState sftp_state(const SftpSession *s) { return s->state; }
const char *sftp_home(const SftpSession *s) { return s->home; }
const char *sftp_connect_log(const SftpSession *s) {
    static char buf[2048];
    buf[0] = 0;
    if (s->state != SFTP_CLOSED || s->root) return NULL;
    size_t at = 0;
    for (int i = 0; i < s->log_count; i++) {
        int n = snprintf(buf + at, sizeof buf - at, "%s%s", at ? "\n" : "", s->log[i]);
        if (n < 0 || (size_t)n >= sizeof buf - at) break;
        at += (size_t)n;
    }
    return buf[0] ? buf : NULL;
}
const char *sftp_selected(const SftpSession *s) { return s->selected; }
void sftp_select(SftpSession *s, const char *path) { set_string(&s->selected, path); notify(); }
const char *sftp_busy(const SftpSession *s, size_t *queued) {
    size_t n = 0;
    for (Cmd *c = s->queue; c; c = c->next) if (c->what) n++;
    if (queued) *queued = n;
    if (s->cmd && s->cmd->what) return s->cmd->what;
    for (Cmd *c = s->queue; c; c = c->next) if (c->what) { if (queued) *queued = n - 1; return c->what; }
    return NULL;
}
const char *sftp_status(const SftpSession *s, bool *failed) { if (failed) *failed = s->status_failed; return s->status; }
const wchar_t *sftp_last_download(const SftpSession *s) { return s->last_download; }

// MARK: - ssh's prompts

bool sftp_askpass_main(int *exit_code) {
    wchar_t flag[8];
    if (!GetEnvironmentVariableW(L"BRIAREUS_ASKPASS", flag, 8) || wcscmp(flag, L"1") != 0) return false;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char *prompt = argv && argc > 1 ? wide_to_utf8(argv[1]) : xstrdup("Password:");
    if (argv) LocalFree(argv);
    wchar_t kind[16] = L"";
    GetEnvironmentVariableW(L"SSH_ASKPASS_PROMPT", kind, 16);
    char *trimmed = str_trim(prompt);
    *exit_code = 1;
    // Asked from the background (ssh started this process), so the boxes come forward and stay on top.
    wchar_t *text = utf8_to_wide(trimmed);
    if (!_wcsicmp(kind, L"none")) {
        // A notice, such as "touch your security key": shown, nothing read back.
        MessageBoxW(NULL, text, L"SSH", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        *exit_code = 0;
    } else if (!_wcsicmp(kind, L"confirm") || strstr(trimmed, "(yes/no")) {
        // A host key never seen before (or a key that asks to be confirmed): ssh wants "yes" to go on.
        bool host_key = strstr(trimmed, "(yes/no") != NULL;
        bool yes = MessageBoxW(NULL, text, host_key ? L"Trust this server?" : L"SSH", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
        if (host_key) { const char *answer = yes ? "yes\n" : "no\n"; DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), answer, (DWORD)strlen(answer), &w, NULL); }
        *exit_code = yes || host_key ? 0 : 1;
    } else {
        char *secret = dialog_password(NULL, "SSH", trimmed);
        if (secret) {
            char *line = xstrfmt("%s\n", secret);
            DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)strlen(line), &w, NULL);
            SecureZeroMemory(line, strlen(line)); SecureZeroMemory(secret, strlen(secret));
            free(line); free(secret);
            *exit_code = 0;
        }
    }
    free(text); free(trimmed); free(prompt);
    return true;
}
