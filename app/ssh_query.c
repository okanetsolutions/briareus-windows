// SQL over SSH (see ssh_query.h): one ssh.exe per query, without a console, its input the password line and the
// statements, a worker thread reading what it prints until it ends and posting the whole answer to a message window.
#include "ssh_query.h"
#include "sftp_session.h"
#include "str.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WM_SQL_DONE = WM_APP + 61 };
static const wchar_t EVENTS_CLASS[] = L"BriareusSshQueryEvents";

/// A query on its way: who asked, and the job its ssh runs in, so a cancel can end it.
typedef struct { int id; void *ctx; SshQueryDone done; intptr_t tag; HANDLE job; } Pending;
static Pending *g_pending; static size_t g_count, g_cap;
static int g_next_id = 1;
static HWND g_events;

typedef struct { int id; HANDLE process, out, err; } Worker;
typedef struct { int id; bool ok; char *out; } Answer;

static Pending *pending_by_id(int id) {
    for (size_t i = 0; i < g_count; i++) if (g_pending[i].id == id) return &g_pending[i];
    return NULL;
}
static void pending_remove(Pending *p) {
    if (p->job) CloseHandle(p->job);
    *p = g_pending[--g_count];
}

static LRESULT CALLBACK events_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_SQL_DONE) return DefWindowProcW(hwnd, msg, wp, lp);
    Answer *a = (Answer *)lp;
    Pending *p = pending_by_id(a->id);
    if (p) {
        void *ctx = p->ctx; SshQueryDone done = p->done; intptr_t tag = p->tag;
        pending_remove(p);
        if (done) done(ctx, tag, a->ok, a->out ? a->out : "");
    }
    free(a->out); free(a);
    return 0;
}
static bool ensure_events(void) {
    if (g_events) return true;
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = events_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = EVENTS_CLASS;
    RegisterClassExW(&wc);
    g_events = CreateWindowExW(0, EVENTS_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    return g_events != NULL;
}

static char *read_all(HANDLE h) {
    Str s; str_init(&s);
    char buf[16384]; DWORD got;
    while (ReadFile(h, buf, sizeof buf, &got, NULL) && got) str_append(&s, buf, got);
    char *out = str_detach(&s);
    return out ? out : xstrdup("");
}
static DWORD WINAPI worker_main(void *param) {
    Worker *w = param;
    // stdout to its end, then stderr: errors are short and fit the pipe while the answer is read.
    char *out = read_all(w->out), *err = read_all(w->err);
    WaitForSingleObject(w->process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(w->process, &code);
    CloseHandle(w->process); CloseHandle(w->out); CloseHandle(w->err);
    Answer *a = xcalloc(1, sizeof *a);
    a->id = w->id; a->ok = code == 0;
    if (a->ok) { a->out = out; free(err); }
    else {
        char *why = str_trim(*err ? err : out);
        a->out = *why ? why : xstrfmt("ssh ended with code %lu.", (unsigned long)code);
        if (!*why) free(why);
        free(out); free(err);
    }
    if (!PostMessageW(g_events, WM_SQL_DONE, 0, (LPARAM)a)) { free(a->out); free(a); }
    free(w);
    return 0;
}

// MARK: - Quoting

char *sh_quote(const char *s) {
    Str q; str_init(&q);
    str_appendc(&q, '\'');
    for (const char *p = s ? s : ""; *p; p++) {
        // '"'"' rather than '\'': Git's ssh.exe (msys) reads backslashes on its command line its own way.
        if (*p == '\'') str_appendz(&q, "'\"'\"'");
        else str_appendc(&q, *p);
    }
    str_appendc(&q, '\'');
    return str_detach(&q);
}
char *sql_ident(const char *name) {
    Str q; str_init(&q);
    str_appendc(&q, '`');
    for (const char *p = name ? name : ""; *p; p++) { if (*p == '`') str_appendc(&q, '`'); str_appendc(&q, *p); }
    str_appendc(&q, '`');
    return str_detach(&q);
}
/// One argument as the C runtime splits a command line: in double quotes, backslashes before a quote doubled.
static void append_win_arg(Str *line, const char *arg) {
    str_appendc(line, '"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (!*p) { for (size_t i = 0; i < slashes * 2; i++) str_appendc(line, '\\'); break; }
        if (*p == '"') { for (size_t i = 0; i < slashes * 2 + 1; i++) str_appendc(line, '\\'); }
        else for (size_t i = 0; i < slashes; i++) str_appendc(line, '\\');
        str_appendc(line, *p);
    }
    str_appendc(line, '"');
}
char *ssh_query_remote_command(const SqlLogin *login) {
    // The first line in is the password; mysql reads it from MYSQL_PWD and the statements from the rest.
    char *host = sh_quote(login->host && *login->host ? login->host : "127.0.0.1"), *user = sh_quote(login->user);
    char *cmd = xstrfmt("IFS= read -r MYSQL_PWD; export MYSQL_PWD; exec mysql --batch --default-character-set=utf8mb4 -h %s -P %d -u %s",
                        host, login->port > 0 ? login->port : 3306, user);
    free(host); free(user);
    return cmd;
}

// MARK: - Running

bool ssh_query_run(const TermTarget *target, const SqlLogin *login, const char *sql, SshQueryDone done, void *ctx, intptr_t tag, char **error) {
    *error = NULL;
    if (!ensure_events()) { *error = xstrdup("Could not create the query's message window."); return false; }
    wchar_t client[MAX_PATH];
    if (!term_find_program(L"ssh.exe", client, MAX_PATH)) {
        *error = xstrdup("ssh.exe was not found. Install Git for Windows, or the OpenSSH Client under Settings \xE2\x86\x92 System \xE2\x86\x92 Optional features.");
        return false;
    }
    char *problem = term_target_problem(target);
    if (problem) { *error = problem; return false; }
    if ((login->host && strchr(login->host, '\\')) || (login->user && strchr(login->user, '\\'))) {
        *error = xstrdup("The database host or username has a backslash in it, which cannot be passed to the server's mysql over SSH.");
        return false;
    }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL, err_read = NULL, err_write = NULL;
    if (!CreatePipe(&in_read, &in_write, &sa, 65536) || !CreatePipe(&out_read, &out_write, &sa, 65536) || !CreatePipe(&err_read, &err_write, &sa, 65536)) {
        *error = xstrdup("Could not create the pipes for ssh.");
        HANDLE all[] = { in_read, in_write, out_read, out_write, err_read, err_write };
        for (size_t i = 0; i < sizeof all / sizeof *all; i++) if (all[i]) CloseHandle(all[i]);
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE inherit[3] = { in_read, out_write, err_write };
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    STARTUPINFOEXW si; ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read; si.StartupInfo.hStdOutput = out_write; si.StartupInfo.hStdError = err_write;
    si.lpAttributeList = xmalloc(attr_size);
    bool ok = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)
           && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    char *exe = wide_to_utf8(client), *remote = ssh_query_remote_command(login);
    Str line; str_init(&line);
    str_appendf(&line, "\"%s\" -T -p %d -o User=%s -o ServerAliveInterval=30 -o LogLevel=ERROR -- %s ", exe, target->port, target->user, target->host);
    append_win_arg(&line, remote);
    wchar_t *cmd = utf8_to_wide(line.data);
    free(exe); free(remote); str_free(&line);
    wchar_t *env = sftp_child_environment();
    wchar_t *dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &dir))) dir = NULL;
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
    CloseHandle(in_read); CloseHandle(out_write); CloseHandle(err_write);
    if (!ok) {
        CloseHandle(in_write); CloseHandle(out_read); CloseHandle(err_read);
        *error = xstrfmt("Could not start ssh.exe (error %lu).", (unsigned long)create_error);
        return false;
    }
    // Everything ssh starts (its prompts) ends with the job, which a cancel closes.
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

    // The input is a few lines, well inside the pipe's buffer, so it is written whole and closed at once.
    Str in; str_init(&in);
    str_appendz(&in, login->password ? login->password : "");
    str_appendc(&in, '\n');
    str_appendz(&in, sql);
    str_appendc(&in, '\n');
    DWORD wrote;
    WriteFile(in_write, in.data, (DWORD)in.len, &wrote, NULL);
    str_free(&in);
    CloseHandle(in_write);

    if (g_count == g_cap) { g_cap = g_cap ? g_cap * 2 : 8; g_pending = xrealloc(g_pending, g_cap * sizeof *g_pending); }
    Pending *p = &g_pending[g_count++];
    p->id = g_next_id++; p->ctx = ctx; p->done = done; p->tag = tag; p->job = job;
    Worker *w = xmalloc(sizeof *w);
    w->id = p->id; w->process = pi.hProcess; w->out = out_read; w->err = err_read;
    HANDLE th = CreateThread(NULL, 0, worker_main, w, 0, NULL);
    if (th) CloseHandle(th);
    else {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(out_read); CloseHandle(err_read); free(w);
        pending_remove(p);
        *error = xstrdup("Could not start the query's reader.");
        return false;
    }
    return true;
}

void ssh_query_cancel(void *ctx) {
    for (size_t i = 0; i < g_count;) {
        if (g_pending[i].ctx == ctx) pending_remove(&g_pending[i]);   // closing the job ends its ssh
        else i++;
    }
}

// MARK: - Parsing

static char *unescape(const char *s, size_t n) {
    Str o; str_init(&o);
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 == n) { str_appendc(&o, s[i]); continue; }
        char c = s[++i];
        str_appendc(&o, c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? ' ' : c);
    }
    char *out = str_detach(&o);
    return out ? out : xstrdup("");
}
void sql_table_parse(const char *out, SqlTable *t) {
    memset(t, 0, sizeof *t);
    size_t cap = 0;
    for (const char *line = out; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        size_t fields = 1;
        for (size_t i = 0; i < len; i++) if (line[i] == '\t') fields++;
        if (!t->rows) t->cols = fields;
        if (len || t->cols == 1) {
            if ((t->rows + 1) * t->cols > cap) { cap = cap ? cap * 2 : 64 * t->cols; while (cap < (t->rows + 1) * t->cols) cap *= 2; t->cells = xrealloc(t->cells, cap * sizeof *t->cells); }
            const char *f = line;
            for (size_t c = 0; c < t->cols; c++) {
                const char *tab = c + 1 < t->cols ? memchr(f, '\t', (size_t)(line + len - f)) : NULL;
                size_t n = tab ? (size_t)(tab - f) : (size_t)(line + len - f);
                if (f > line + len) n = 0;
                t->cells[t->rows * t->cols + c] = f <= line + len ? unescape(f, n) : xstrdup("");
                f = tab ? tab + 1 : line + len + 1;
            }
            t->rows++;
        }
        if (!end) break;
        line = end + 1;
    }
}
void sql_table_free(SqlTable *t) {
    for (size_t i = 0; i < t->rows * t->cols; i++) free(t->cells[i]);
    free(t->cells);
    memset(t, 0, sizeof *t);
}
