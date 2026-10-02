// Terminal sessions: ssh.exe in a pseudoconsole (ConPTY), a reader thread per session that hands what it
// prints to the UI thread through a message-only window, and a child window per session that paints the emulator's grid
// (core/vt.c) in a monospace font and turns keys, the mouse and the clipboard into what the program reads.
#include "terminal.h"
#include "str.h"
#include "theme.h"
#include "vt.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

enum { SCROLLBACK = 5000, READ_CHUNK = 65536, PAD = 6, MAX_LISTENERS = 16, RUN_MAX = 512 };
enum { WM_TERM_DATA = WM_APP + 41, WM_TERM_EXIT, WM_TERM_EOF };
enum { TIMER_NUDGE = 1, NUDGE_MS = 250 };
enum { MENU_COPY = 1, MENU_PASTE, MENU_SELECT_ALL, MENU_CLEAR };
static const wchar_t TERM_CLASS[] = L"BriareusTerminal", EVENTS_CLASS[] = L"BriareusTerminalEvents";

typedef HRESULT (WINAPI *CreatePcFn)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT (WINAPI *ResizePcFn)(HPCON, COORD);
typedef void (WINAPI *ClosePcFn)(HPCON);
static CreatePcFn p_create_pc;
static ResizePcFn p_resize_pc;
static ClosePcFn p_close_pc;

/// A selection end: a line counted from the first that ever scrolled away, so it stays on its text as output scrolls.
typedef struct { long long line; int col; } Mark;

struct Term {
    int id, spawn;                 // the session, and the run of its program the pseudoconsole's events belong to
    char *key, *group, *label, *user, *host, *target;
    int port;
    HWND hwnd;
    Vt *vt;
    int cols, rows;
    HPCON pc;
    HANDLE in_write, process, wait;
    bool running;                  // the program runs, or its last output is still being read
    bool exited;                   // the program ended; its pseudoconsole is closing
    DWORD exit_code;
    int offset;                    // lines scrolled back from the bottom
    bool selecting, has_sel;
    Mark sel_a, sel_b;
    wchar_t high_surrogate;
};

typedef struct { int term; size_t len; char data[]; } Chunk;
typedef struct { int term, spawn; HANDLE out_read; } ReaderArgs;

static Term **g_terms; static size_t g_count, g_cap;
static Term *g_active;
static int g_next_id = 1, g_next_spawn = 1;
static HWND g_events;
static struct { void (*fn)(void *); void *ctx; } g_listeners[MAX_LISTENERS];

static void notify(void) {
    for (int i = 0; i < MAX_LISTENERS; i++) if (g_listeners[i].fn) g_listeners[i].fn(g_listeners[i].ctx);
}
void term_add_listener(void (*fn)(void *ctx), void *ctx) {
    for (int i = 0; i < MAX_LISTENERS; i++) if (!g_listeners[i].fn) { g_listeners[i].fn = fn; g_listeners[i].ctx = ctx; return; }
}
void term_remove_listener(void (*fn)(void *ctx), void *ctx) {
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (g_listeners[i].fn == fn && g_listeners[i].ctx == ctx) { g_listeners[i].fn = NULL; g_listeners[i].ctx = NULL; }
}

static Term *by_id(int id) {
    for (size_t i = 0; i < g_count; i++) if (g_terms[i]->id == id) return g_terms[i];
    return NULL;
}

// MARK: - Fonts and colours

static HFONT g_fonts[4];           // regular, bold, italic, bold italic
static int g_font_dpi, g_cw, g_ch;

static HFONT make_font(const wchar_t *face, int style) {
    return CreateFontW(-px(14), 0, 0, 0, style & 1 ? FW_BOLD : FW_NORMAL, (style & 2) != 0, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
}
/// The cell size follows the app's DPI; true when it changed.
static bool fonts_update(void) {
    if (g_fonts[0] && g_font_dpi == theme_dpi()) return false;
    for (int i = 0; i < 4; i++) if (g_fonts[i]) { DeleteObject(g_fonts[i]); g_fonts[i] = NULL; }
    g_font_dpi = theme_dpi();
    // Cascadia Mono ships with Windows 11 and Windows Terminal; Consolas is on every Windows.
    const wchar_t *face = L"Cascadia Mono";
    HDC dc = GetDC(NULL);
    HFONT probe = make_font(face, 0);
    HGDIOBJ old = SelectObject(dc, probe);
    wchar_t got[LF_FACESIZE] = L"";
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    DeleteObject(probe);
    if (_wcsicmp(got, face) != 0) face = L"Consolas";
    for (int i = 0; i < 4; i++) g_fonts[i] = make_font(face, i);
    old = SelectObject(dc, g_fonts[0]);
    SIZE sz; GetTextExtentPoint32W(dc, L"MMMMMMMMMM", 10, &sz);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    g_cw = sz.cx > 0 ? (sz.cx + 5) / 10 : 8;
    g_ch = tm.tmHeight > 0 ? tm.tmHeight + px(1) : 16;
    return true;
}

/// One Half, dark or light as the app is: the first 16 colours programs pick by number.
static COLORREF palette16(int i) {
    static const unsigned dark[16] = { 0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4,
                                       0x5A6374, 0xEF8790, 0xB3DB94, 0xF0D49E, 0x84C3F5, 0xD99BE8, 0x7DCBD4, 0xFFFFFF };
    static const unsigned light[16] = { 0x383A42, 0xE45649, 0x50A14F, 0xC18401, 0x0184BC, 0xA626A4, 0x0997B3, 0xA0A1A7,
                                        0x4F525D, 0xDF6C75, 0x3E953A, 0xA87000, 0x2F6FD0, 0xC577DD, 0x0B8A9E, 0x202227 };
    unsigned c = (theme.dark ? dark : light)[i & 15];
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}
static COLORREF term_background(void) { return theme.dark ? RGB(0x16, 0x18, 0x1D) : RGB(0xFB, 0xFA, 0xF6); }
static COLORREF term_foreground(void) { return theme.dark ? RGB(0xDC, 0xDF, 0xE4) : RGB(0x2B, 0x2D, 0x33); }
static COLORREF resolve(uint32_t c, bool fg, bool bold) {
    switch (VT_COLOR_KIND(c)) {
    case VT_COLOR_INDEX: {
        int i = (int)(c & 0xFF);
        // Bold brightens the eight basic colours, as most terminals still do.
        if (i < 16) return palette16(bold && i < 8 ? i + 8 : i);
        uint32_t rgb = vt_index_rgb(i);
        return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }
    case VT_COLOR_RGB: return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    return fg ? term_foreground() : term_background();
}

// MARK: - Targets and the client

static bool safe_word(const char *z, const char *extra) {
    if (str_empty(z) || z[0] == '-') return false;
    for (const char *p = z; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 || strchr(extra, c))) return false;
    }
    return true;
}
char *term_target_problem(const TermTarget *target) {
    if (!target || str_empty(target->host)) return xstrdup("This server has no host.");
    // The host and user go to ssh as its arguments: nothing that reads as an option or splits into more than one.
    if (!safe_word(target->host, ".-_:[]%")) return xstrfmt("The host \xE2\x80\x9C%s\xE2\x80\x9D is not a hostname or an IP address ssh can be given.", target->host);
    if (str_empty(target->user)) return xstrdup("This server has no username.");
    if (!safe_word(target->user, ".-_$\\")) return xstrfmt("The username \xE2\x80\x9C%s\xE2\x80\x9D cannot be given to ssh.", target->user);
    if (target->port < 1 || target->port > 65535) return xstrfmt("The port %d is not from 1 to 65535.", target->port);
    return NULL;
}

wchar_t *term_command_line(const wchar_t *client, const TermTarget *target) {
    char *exe = wide_to_utf8(client);
    char *line = xstrfmt("\"%s\" -p %d -l %s -- %s", exe, target->port, target->user, target->host);
    wchar_t *w = utf8_to_wide(line);
    free(exe); free(line);
    return w;
}

static bool file_exists(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
/// Git for Windows' own OpenSSH, under its install folder as the registry (or the default path) has it.
static bool find_git_client(const wchar_t *name, wchar_t *out, DWORD n) {
    static const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; i++) {
        wchar_t dir[MAX_PATH]; DWORD size = sizeof dir;
        if (RegGetValueW(roots[i], L"SOFTWARE\\GitForWindows", L"InstallPath", RRF_RT_REG_SZ, NULL, dir, &size) != ERROR_SUCCESS) continue;
        _snwprintf(out, n, L"%ls\\usr\\bin\\%ls", dir, name);
        out[n - 1] = 0;
        if (file_exists(out)) return true;
    }
    wchar_t program_files[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    _snwprintf(out, n, L"%ls\\Git\\usr\\bin\\%ls", program_files, name);
    out[n - 1] = 0;
    return file_exists(out);
}
/// The full path of one of OpenSSH's programs. Git for Windows' OpenSSH comes first: Windows' own (9.5) stalls in a
/// pseudoconsole on recent Windows builds, its output and its exit waiting for a key, while Git's streams. Both read
/// ~/.ssh. Then Windows' OpenSSH Client, then whatever PATH finds.
bool term_find_program(const wchar_t *name, wchar_t *out, DWORD n) {
    if (find_git_client(name, out, n)) return true;
    wchar_t dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len && len < MAX_PATH - 20) {
        _snwprintf(out, n, L"%ls\\OpenSSH\\%ls", dir, name);
        out[n - 1] = 0;
        if (file_exists(out)) return true;
    }
    return SearchPathW(NULL, name, NULL, n, out, NULL) > 0;
}

static bool conpty_available(void) {
    if (p_create_pc) return true;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    p_create_pc = (CreatePcFn)(void *)GetProcAddress(k, "CreatePseudoConsole");
    p_resize_pc = (ResizePcFn)(void *)GetProcAddress(k, "ResizePseudoConsole");
    p_close_pc = (ClosePcFn)(void *)GetProcAddress(k, "ClosePseudoConsole");
    if (!p_create_pc || !p_resize_pc || !p_close_pc) { p_create_pc = NULL; return false; }
    return true;
}

// MARK: - The program's output

static DWORD WINAPI reader_main(void *param) {
    ReaderArgs *a = param;
    char *buf = xmalloc(READ_CHUNK);
    DWORD got;
    while (ReadFile(a->out_read, buf, READ_CHUNK, &got, NULL) && got) {
        Chunk *c = malloc(sizeof *c + got);
        if (!c) break;
        c->term = a->term; c->len = got;
        memcpy(c->data, buf, got);
        if (!PostMessageW(g_events, WM_TERM_DATA, 0, (LPARAM)c)) free(c);
    }
    // The pseudoconsole closed: everything it printed has been posted before this.
    PostMessageW(g_events, WM_TERM_EOF, (WPARAM)a->term, (LPARAM)a->spawn);
    CloseHandle(a->out_read);
    free(buf); free(a);
    return 0;
}
static VOID CALLBACK exited(PVOID param, BOOLEAN timed_out) {
    (void)timed_out;
    PostMessageW(g_events, WM_TERM_EXIT, 0, (LPARAM)param);
}
/// Closing a pseudoconsole waits for its host to finish writing, so it happens off the UI thread.
static DWORD WINAPI close_pc_main(void *param) { p_close_pc((HPCON)param); return 0; }
static void close_pc_async(HPCON pc) {
    if (!pc) return;
    HANDLE th = CreateThread(NULL, 0, close_pc_main, pc, 0, NULL);
    if (th) CloseHandle(th); else p_close_pc(pc);
}

static void term_write(Term *t, const char *data, size_t len) {
    if (!t->in_write || !len) return;
    DWORD put;
    WriteFile(t->in_write, data, (DWORD)len, &put, NULL);
}
static void selection_clear(Term *t) { if (t->has_sel || t->selecting) { t->has_sel = t->selecting = false; InvalidateRect(t->hwnd, NULL, FALSE); } }
static void scrollbar_update(Term *t);

/// Focus in or out, for a program that asked to be told (mode 1004); ConPTY asks, to pass it on as a console event.
static void report_focus(Term *t) {
    if (t->running && vt_focus_events(t->vt)) term_write(t, GetFocus() == t->hwnd ? "\x1b[I" : "\x1b[O", 3);
}

static void feed(Term *t, const char *data, size_t len) {
    unsigned long long before = vt_lines_pushed(t->vt);
    bool focus_events = vt_focus_events(t->vt);
    vt_write(t->vt, data, len);
    if (!focus_events && vt_focus_events(t->vt)) report_focus(t);
    size_t rlen;
    char *reply = vt_take_response(t->vt, &rlen);
    if (reply) { term_write(t, reply, rlen); free(reply); }
    // A view scrolled back stays on the text it shows while new lines arrive below.
    int pushed = (int)(vt_lines_pushed(t->vt) - before);
    if (t->offset && pushed) { t->offset += pushed; if (t->offset > vt_scrollback(t->vt)) t->offset = vt_scrollback(t->vt); }
    if (vt_take_title_changed(t->vt)) notify();
    vt_take_bell(t->vt);
    scrollbar_update(t);
    InvalidateRect(t->hwnd, NULL, FALSE);
}

static void process_ended(Term *t) {
    if (t->wait) { UnregisterWait(t->wait); t->wait = NULL; }
    if (t->process) { GetExitCodeProcess(t->process, &t->exit_code); CloseHandle(t->process); t->process = NULL; }
    if (t->in_write) { CloseHandle(t->in_write); t->in_write = NULL; }
    t->exited = true;
    // The pseudoconsole flushes the program's last screen and closes; its end of file says the session is over.
    close_pc_async(t->pc);
    t->pc = NULL;
}

static LRESULT CALLBACK events_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TERM_DATA: {
        Chunk *c = (Chunk *)lp;
        Term *t = by_id(c->term);
        if (t) feed(t, c->data, c->len);
        free(c);
        return 0;
    }
    case WM_TERM_EXIT:
        for (size_t i = 0; i < g_count; i++) if (g_terms[i]->spawn == (int)lp && !g_terms[i]->exited) process_ended(g_terms[i]);
        return 0;
    case WM_TERM_EOF: {
        Term *t = by_id((int)wp);
        if (!t || t->spawn != (int)lp) return 0;
        // The pseudoconsole can also end first (its host failed): the program goes with it.
        if (!t->exited) { if (t->process) TerminateProcess(t->process, 1); process_ended(t); }
        t->running = false;
        KillTimer(t->hwnd, TIMER_NUDGE);
        char *note = t->exit_code == 0
            ? xstrdup("\r\n\x1b[0;2m[Session closed. Press Enter to reconnect.]\x1b[0m\r\n")
            : xstrfmt("\r\n\x1b[0;2m[Session closed (exit code %lu). Press Enter to reconnect.]\x1b[0m\r\n", (unsigned long)t->exit_code);
        feed(t, note, strlen(note));
        free(note);
        notify();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// MARK: - Starting the program

static void size_cells(Term *t, int *cols, int *rows) {
    RECT rc; GetClientRect(t->hwnd, &rc);
    int w = rc.right - rc.left - 2 * px(PAD) - GetSystemMetrics(SM_CXVSCROLL), h = rc.bottom - rc.top - 2 * px(PAD);
    *cols = g_cw ? w / g_cw : 80; *rows = g_ch ? h / g_ch : 24;
    if (*cols < 20) *cols = 20;
    if (*rows < 5) *rows = 5;
}

static bool spawn(Term *t, char **error) {
    if (!conpty_available()) { *error = xstrdup("This version of Windows has no pseudoconsole (ConPTY); Windows 10 1809 or later is needed."); return false; }
    wchar_t client[MAX_PATH];
    if (!term_find_program(L"ssh.exe", client, MAX_PATH)) {
        *error = xstrfmt("%s was not found. Install Git for Windows, or the OpenSSH Client under Settings \xE2\x86\x92 System \xE2\x86\x92 Optional features.", "ssh.exe");
        return false;
    }
    TermTarget target = { t->key, t->group, t->label, t->user, t->host, t->port };
    char *problem = term_target_problem(&target);
    if (problem) { *error = problem; return false; }

    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
        *error = xstrdup("Could not create the pipes for the terminal.");
        if (in_read) { CloseHandle(in_read); CloseHandle(in_write); }
        return false;
    }
    COORD size = { (SHORT)t->cols, (SHORT)t->rows };
    HPCON pc = NULL;
    HRESULT hr = p_create_pc(size, in_read, out_write, 0, &pc);
    CloseHandle(in_read); CloseHandle(out_write);   // the pseudoconsole holds its own copies
    if (FAILED(hr)) {
        CloseHandle(in_write); CloseHandle(out_read);
        *error = xstrfmt("Could not create a pseudoconsole (0x%08lX).", (unsigned long)hr);
        return false;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    STARTUPINFOEXW si; ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = xmalloc(attr_size);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    bool ok = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)
           && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc, NULL, NULL);
    // ssh starts in the profile, where ~/.ssh is.
    wchar_t *dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &dir))) dir = NULL;
    wchar_t *cmd = term_command_line(client, &target);
    DWORD create_error = 0;
    if (ok) {
        ok = CreateProcessW(client, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, dir, &si.StartupInfo, &pi);
        if (!ok) create_error = GetLastError();
    }
    free(cmd);
    if (dir) CoTaskMemFree(dir);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok) {
        p_close_pc(pc);
        CloseHandle(in_write); CloseHandle(out_read);
        *error = xstrfmt("Could not start %s (error %lu).", "ssh.exe", (unsigned long)create_error);
        return false;
    }
    CloseHandle(pi.hThread);

    t->spawn = g_next_spawn++;
    t->pc = pc; t->in_write = in_write; t->process = pi.hProcess;
    t->running = true; t->exited = false; t->exit_code = 0;
    ReaderArgs *a = xmalloc(sizeof *a);
    a->term = t->id; a->spawn = t->spawn; a->out_read = out_read;
    HANDLE th = CreateThread(NULL, 0, reader_main, a, 0, NULL);
    if (th) CloseHandle(th);
    else { CloseHandle(out_read); free(a); }
    if (!RegisterWaitForSingleObject(&t->wait, pi.hProcess, exited, (PVOID)(intptr_t)t->spawn, INFINITE, WT_EXECUTEONLYONCE)) t->wait = NULL;
    SetTimer(t->hwnd, TIMER_NUDGE, NUDGE_MS, NULL);
    return true;
}

// MARK: - Selection and the clipboard

static long long line_base(Term *t) { return (long long)vt_lines_pushed(t->vt); }
/// The mark under a point of the window.
static Mark mark_at(Term *t, int x, int y) {
    int col = (x - px(PAD) + g_cw / 2) / (g_cw ? g_cw : 1), row = (y - px(PAD)) / (g_ch ? g_ch : 1);
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    if (col < 0) col = 0;
    if (col > t->cols) col = t->cols;
    Mark m = { line_base(t) + row - t->offset, col };
    return m;
}
static int mark_cmp(Mark a, Mark b) { return a.line != b.line ? (a.line < b.line ? -1 : 1) : a.col - b.col; }
static void selection_bounds(Term *t, Mark *from, Mark *to) {
    if (mark_cmp(t->sel_a, t->sel_b) <= 0) { *from = t->sel_a; *to = t->sel_b; }
    else { *from = t->sel_b; *to = t->sel_a; }
}
static bool selected(Term *t, long long line, int col) {
    if (!t->has_sel) return false;
    Mark from, to; selection_bounds(t, &from, &to);
    Mark m = { line, col };
    return mark_cmp(m, from) >= 0 && mark_cmp(m, to) < 0;
}
static char *selection_text(Term *t) {
    if (!t->has_sel) return NULL;
    Mark from, to; selection_bounds(t, &from, &to);
    if (!mark_cmp(from, to)) return NULL;
    long long base = line_base(t);
    int y0 = (int)(from.line - base), y1 = (int)(to.line - base), lo = -vt_scrollback(t->vt);
    if (y1 < lo) return NULL;
    if (y0 < lo) { y0 = lo; from.col = 0; }
    return vt_text(t->vt, y0, from.col, y1, to.col);
}
static void copy_selection(Term *t) {
    char *text = selection_text(t);
    if (text && *text) copy_to_clipboard(t->hwnd, text);
    free(text);
}
static void paste(Term *t) {
    if (!t->running || !OpenClipboard(t->hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    char *text = w ? wide_to_utf8(w) : NULL;
    if (w) GlobalUnlock(h);
    CloseClipboard();
    if (!text) return;
    // Lines end in a carriage return, as the Enter key sends them.
    Str s; str_init(&s);
    if (vt_bracketed_paste(t->vt)) str_appendz(&s, "\x1b[200~");
    for (const char *p = text; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        if (*p == '\x1b') continue;   // a pasted escape could end bracketed paste early
        str_appendc(&s, *p == '\n' ? '\r' : *p);
    }
    if (vt_bracketed_paste(t->vt)) str_appendz(&s, "\x1b[201~");
    t->offset = 0;
    term_write(t, s.data, s.len);
    str_free(&s); free(text);
}
static void select_all(Term *t) {
    t->has_sel = true;
    t->sel_a.line = line_base(t) - vt_scrollback(t->vt); t->sel_a.col = 0;
    t->sel_b.line = line_base(t) + t->rows - 1; t->sel_b.col = t->cols;
    InvalidateRect(t->hwnd, NULL, FALSE);
}
/// Double-click: the run of non-blank characters under the mouse.
static void select_word(Term *t, Mark m) {
    int width = 0;
    const VtCell *row = vt_line(t->vt, (int)(m.line - line_base(t)), &width);
    if (!row || m.col >= width) return;
    int col = m.col < width ? m.col : width - 1;
    if (row[col].ch == ' ') return;
    int a = col, b = col;
    while (a > 0 && row[a - 1].ch != ' ') a--;
    while (b < width && row[b].ch != ' ') b++;
    t->has_sel = true;
    t->sel_a.line = t->sel_b.line = m.line; t->sel_a.col = a; t->sel_b.col = b;
    InvalidateRect(t->hwnd, NULL, FALSE);
}

// MARK: - Painting

typedef struct { HDC dc; int x, y; COLORREF fg, bg; uint16_t attr; wchar_t text[RUN_MAX]; INT dx[RUN_MAX]; int n, cells; } Run;

static void run_flush(Run *r) {
    if (!r->cells) return;
    RECT box = { r->x, r->y, r->x + r->cells * g_cw, r->y + g_ch };
    SetBkColor(r->dc, r->bg);
    SetTextColor(r->dc, r->fg);
    SelectObject(r->dc, g_fonts[(r->attr & VT_BOLD ? 1 : 0) | (r->attr & VT_ITALIC ? 2 : 0)]);
    ExtTextOutW(r->dc, r->x, r->y, ETO_OPAQUE | ETO_CLIPPED, &box, r->text, (UINT)r->n, r->dx);
    if (r->attr & (VT_UNDERLINE | VT_STRIKE)) {
        HPEN pen = CreatePen(PS_SOLID, px(1) > 0 ? px(1) : 1, r->fg);
        HGDIOBJ old = SelectObject(r->dc, pen);
        if (r->attr & VT_UNDERLINE) { MoveToEx(r->dc, box.left, box.bottom - px(2), NULL); LineTo(r->dc, box.right, box.bottom - px(2)); }
        if (r->attr & VT_STRIKE) { MoveToEx(r->dc, box.left, (box.top + box.bottom) / 2, NULL); LineTo(r->dc, box.right, (box.top + box.bottom) / 2); }
        SelectObject(r->dc, old);
        DeleteObject(pen);
    }
    r->x = box.right; r->n = 0; r->cells = 0;
}

static void paint(Term *t, HDC target) {
    RECT rc; GetClientRect(t->hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HGDIOBJ old_font = SelectObject(dc, g_fonts[0]);
    COLORREF background = term_background();
    HBRUSH bg = CreateSolidBrush(background);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, OPAQUE);

    bool focused = GetFocus() == t->hwnd;
    int cx, cy; bool cursor_visible;
    vt_cursor(t->vt, &cx, &cy, &cursor_visible);
    long long base = line_base(t);
    COLORREF sel_bg = blend(theme.accent, background, 0.35), sel_fg = term_foreground();
    for (int r = 0; r < t->rows; r++) {
        int y = r - t->offset, width = 0;
        const VtCell *line = vt_line(t->vt, y, &width);
        Run run = { .dc = dc, .x = px(PAD), .y = px(PAD) + r * g_ch };
        for (int x = 0; x < t->cols; x++) {
            VtCell c = line && x < width ? line[x] : (VtCell){ ' ', VT_COLOR_DEFAULT, VT_COLOR_DEFAULT, 0 };
            if (c.attr & VT_WIDE_TAIL) continue;
            int span = c.attr & VT_WIDE && x + 1 < t->cols ? 2 : 1;
            COLORREF fg = resolve(c.fg, true, c.attr & VT_BOLD), bgc = resolve(c.bg, false, false);
            if (c.attr & VT_INVERSE) { COLORREF s = fg; fg = bgc; bgc = s; }
            if (c.attr & VT_DIM) fg = blend(fg, bgc, 0.6);
            if (c.attr & VT_HIDDEN) fg = bgc;
            if (selected(t, base + y, x)) { bgc = sel_bg; fg = sel_fg; }
            bool at_cursor = cursor_visible && t->offset == 0 && y == cy && x == cx && t->running;
            if (at_cursor && focused) { bgc = theme.accent; fg = background; }
            uint16_t attr = c.attr & (VT_BOLD | VT_ITALIC | VT_UNDERLINE | VT_STRIKE);
            if (run.cells && (fg != run.fg || bgc != run.bg || attr != run.attr || run.n + 2 >= RUN_MAX)) run_flush(&run);
            run.fg = fg; run.bg = bgc; run.attr = attr;
            uint32_t ch = c.ch ? c.ch : ' ';
            if (ch >= 0x10000) {
                run.text[run.n] = (wchar_t)(0xD800 + ((ch - 0x10000) >> 10)); run.dx[run.n++] = span * g_cw;
                run.text[run.n] = (wchar_t)(0xDC00 + ((ch - 0x10000) & 0x3FF)); run.dx[run.n++] = 0;
            } else { run.text[run.n] = (wchar_t)ch; run.dx[run.n++] = span * g_cw; }
            run.cells += span;
        }
        run_flush(&run);
    }
    // Unfocused, the cursor is an outline.
    if (cursor_visible && t->offset == 0 && t->running && !focused && cy < t->rows) {
        RECT c = { px(PAD) + cx * g_cw, px(PAD) + cy * g_ch, px(PAD) + (cx + 1) * g_cw, px(PAD) + (cy + 1) * g_ch };
        HBRUSH b = CreateSolidBrush(theme.accent);
        FrameRect(dc, &c, b);
        DeleteObject(b);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void scrollbar_update(Term *t) {
    SCROLLINFO si = { sizeof si, SIF_ALL | SIF_DISABLENOSCROLL };
    int sb = vt_scrollback(t->vt);
    si.nMin = 0; si.nMax = sb + t->rows - 1; si.nPage = (UINT)t->rows; si.nPos = sb - t->offset;
    SetScrollInfo(t->hwnd, SB_VERT, &si, TRUE);
}
static void scroll_view(Term *t, int offset) {
    int sb = vt_scrollback(t->vt);
    if (offset < 0) offset = 0;
    if (offset > sb) offset = sb;
    if (offset == t->offset) return;
    t->offset = offset;
    scrollbar_update(t);
    InvalidateRect(t->hwnd, NULL, FALSE);
}

static void resize_to_window(Term *t) {
    int cols, rows;
    size_cells(t, &cols, &rows);
    if (cols == t->cols && rows == t->rows) return;
    t->cols = cols; t->rows = rows;
    vt_resize(t->vt, cols, rows);
    if (t->pc && t->running) { COORD size = { (SHORT)cols, (SHORT)rows }; p_resize_pc(t->pc, size); }
    if (t->offset > vt_scrollback(t->vt)) t->offset = vt_scrollback(t->vt);
    scrollbar_update(t);
    InvalidateRect(t->hwnd, NULL, FALSE);
}

// MARK: - Keys

/// xterm's modifier parameter: 1 + Shift + 2·Alt + 4·Ctrl.
static int modifiers(bool shift, bool alt, bool ctrl) { return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0); }
static void send_keys(Term *t, const char *z) { t->offset = 0; selection_clear(t); scrollbar_update(t); term_write(t, z, strlen(z)); }
/// Drops the character message a handled key produced, so it is not sent twice.
static void eat_char(HWND hwnd) {
    MSG m;
    while (PeekMessageW(&m, hwnd, WM_CHAR, WM_DEADCHAR, PM_REMOVE)) {}
    while (PeekMessageW(&m, hwnd, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {}
}

static bool key_down(Term *t, WPARAM vk, bool alt) {
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    // The terminal's own shortcuts.
    if (ctrl && shift && vk == 'C') { copy_selection(t); eat_char(t->hwnd); return true; }
    if (ctrl && shift && vk == 'V') { paste(t); eat_char(t->hwnd); return true; }
    if (ctrl && !shift && vk == 'C' && t->has_sel) { copy_selection(t); selection_clear(t); eat_char(t->hwnd); return true; }
    if (shift && vk == VK_INSERT) { paste(t); return true; }
    if (ctrl && vk == VK_INSERT) { copy_selection(t); return true; }
    if (ctrl && vk == VK_TAB) {
        // The next (or previous) tab of the same project.
        size_t i = 0;
        while (i < g_count && g_terms[i] != t) i++;
        for (size_t step = 1; step < g_count; step++) {
            Term *next = g_terms[(i + (shift ? g_count - step : step)) % g_count];
            if (str_eq(next->group, t->group)) { term_set_active(next); break; }
        }
        eat_char(t->hwnd);
        return true;
    }
    if (shift && !ctrl && (vk == VK_PRIOR || vk == VK_NEXT)) { scroll_view(t, t->offset + (vk == VK_PRIOR ? 1 : -1) * (t->rows - 1)); return true; }
    if (!t->running) {
        if (vk == VK_RETURN) { char *error = NULL; if (!term_reconnect(t, &error)) { char *line = xstrfmt("\r\n%s\r\n", error); feed(t, line, strlen(line)); free(line); } free(error); eat_char(t->hwnd); }
        return vk == VK_RETURN;
    }
    int m = modifiers(shift, alt, ctrl);
    char seq[32] = "";
    const char *arrows = "ABCD";   // up, down, right, left
    int arrow = vk == VK_UP ? 0 : vk == VK_DOWN ? 1 : vk == VK_RIGHT ? 2 : vk == VK_LEFT ? 3 : -1;
    if (arrow >= 0) {
        if (m > 1) snprintf(seq, sizeof seq, "\x1b[1;%d%c", m, arrows[arrow]);
        else snprintf(seq, sizeof seq, vt_app_cursor(t->vt) ? "\x1bO%c" : "\x1b[%c", arrows[arrow]);
    } else if (vk == VK_HOME || vk == VK_END) {
        char f = vk == VK_HOME ? 'H' : 'F';
        if (m > 1) snprintf(seq, sizeof seq, "\x1b[1;%d%c", m, f);
        else snprintf(seq, sizeof seq, vt_app_cursor(t->vt) ? "\x1bO%c" : "\x1b[%c", f);
    } else if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_PRIOR || vk == VK_NEXT) {
        int n = vk == VK_INSERT ? 2 : vk == VK_DELETE ? 3 : vk == VK_PRIOR ? 5 : 6;
        if (m > 1) snprintf(seq, sizeof seq, "\x1b[%d;%d~", n, m); else snprintf(seq, sizeof seq, "\x1b[%d~", n);
    } else if (vk >= VK_F1 && vk <= VK_F12) {
        static const int codes[12] = { 0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24 };
        int i = (int)(vk - VK_F1);
        if (i < 4) { if (m > 1) snprintf(seq, sizeof seq, "\x1b[1;%d%c", m, "PQRS"[i]); else snprintf(seq, sizeof seq, "\x1bO%c", "PQRS"[i]); }
        else if (m > 1) snprintf(seq, sizeof seq, "\x1b[%d;%d~", codes[i], m);
        else snprintf(seq, sizeof seq, "\x1b[%d~", codes[i]);
    } else if (vk == VK_BACK) {
        snprintf(seq, sizeof seq, "%s%c", alt ? "\x1b" : "", ctrl ? 0x08 : 0x7F);
        send_keys(t, seq); eat_char(t->hwnd);
        return true;
    } else if (vk == VK_TAB && shift) {
        send_keys(t, "\x1b[Z"); eat_char(t->hwnd);
        return true;
    } else if (ctrl && (vk == VK_SPACE || vk == '2')) {
        t->offset = 0; term_write(t, "\0", 1); eat_char(t->hwnd);
        return true;
    }
    if (!seq[0]) return false;
    send_keys(t, seq);
    eat_char(t->hwnd);
    return true;
}

static void char_typed(Term *t, wchar_t c, bool alt) {
    if (!t->running) return;
    if (c >= 0xD800 && c <= 0xDBFF) { t->high_surrogate = c; return; }
    wchar_t w[3] = { 0 };
    if (c >= 0xDC00 && c <= 0xDFFF) { if (!t->high_surrogate) return; w[0] = t->high_surrogate; w[1] = c; }
    else w[0] = c;
    t->high_surrogate = 0;
    char *u = wide_to_utf8(w);
    char *out = alt ? xstrfmt("\x1b%s", u) : xstrdup(u);
    send_keys(t, out);
    free(u); free(out);
}

// MARK: - The window

static void context_menu(Term *t, POINT pt) {
    HMENU menu = CreatePopupMenu();
    char *sel = selection_text(t);
    AppendMenuW(menu, MF_STRING | (sel && *sel ? 0 : MF_GRAYED), MENU_COPY, L"Copy\tCtrl+Shift+C");
    AppendMenuW(menu, MF_STRING | (t->running && IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), MENU_PASTE, L"Paste\tCtrl+Shift+V");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_SELECT_ALL, L"Select all");
    AppendMenuW(menu, MF_STRING | (vt_scrollback(t->vt) ? 0 : MF_GRAYED), MENU_CLEAR, L"Clear scrollback");
    free(sel);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, t->hwnd, NULL);
    DestroyMenu(menu);
    switch (chosen) {
    case MENU_COPY: copy_selection(t); break;
    case MENU_PASTE: paste(t); break;
    case MENU_SELECT_ALL: select_all(t); break;
    case MENU_CLEAR: vt_clear_scrollback(t->vt); t->offset = 0; selection_clear(t); scrollbar_update(t); InvalidateRect(t->hwnd, NULL, FALSE); break;
    }
}

static LRESULT CALLBACK term_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Term *t = (Term *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    // The window is sized before the session's emulator exists.
    if (!t || !t->vt) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        if (fonts_update()) resize_to_window(t);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(t, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: resize_to_window(t); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: report_focus(t); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_TIMER:
        // Windows' ssh.exe only gets on with its output and its exit once its console input wakes it, and nothing does
        // while the keyboard is idle: a key-up of no key, as a Win32 input record, wakes it without reaching the server.
        if (wp == TIMER_NUDGE && t->running && t->in_write && vt_win32_input(t->vt)) { static const char none[] = "\x1b[0;0;0;0;0;1_"; term_write(t, none, strlen(none)); }
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN: if (key_down(t, wp, false)) return 0; break;
    case WM_SYSKEYDOWN:
        // Alt+F4 still closes the app; Alt with a key is the key after ESC, as terminals send Meta.
        if (wp != VK_F4 && key_down(t, wp, true)) return 0;
        break;
    case WM_CHAR:
        if (wp == 0x08) return 0;   // Backspace was sent as DEL when the key went down
        char_typed(t, (wchar_t)wp, false);
        return 0;
    case WM_SYSCHAR:
        if (wp == VK_SPACE) break;   // Alt+Space opens the window menu
        char_typed(t, (wchar_t)wp, true);
        return 0;
    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (!notches) return 0;
        if (vt_alt_screen(t->vt) && t->running) {
            // Full-screen programs (less, vim, htop) have no scrollback: the wheel moves them with the arrow keys.
            const char *key = notches > 0 ? (vt_app_cursor(t->vt) ? "\x1bOA" : "\x1b[A") : (vt_app_cursor(t->vt) ? "\x1bOB" : "\x1b[B");
            for (int i = 0; i < 3 * abs(notches); i++) term_write(t, key, strlen(key));
            return 0;
        }
        scroll_view(t, t->offset + notches * 3);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof si, SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int sb = vt_scrollback(t->vt), pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos--; break;
        case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= t->rows; break;
        case SB_PAGEDOWN: pos += t->rows; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = sb; break;
        }
        scroll_view(t, sb - pos);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        SetCapture(hwnd);
        Mark m = mark_at(t, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        t->selecting = true; t->has_sel = false; t->sel_a = t->sel_b = m;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        select_word(t, mark_at(t, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_MOUSEMOVE:
        if (t->selecting) {
            int y = GET_Y_LPARAM(lp);
            // Dragging past the top or bottom scrolls the view along.
            RECT rc; GetClientRect(hwnd, &rc);
            if (y < 0) scroll_view(t, t->offset + 1);
            else if (y > rc.bottom) scroll_view(t, t->offset - 1);
            t->sel_b = mark_at(t, GET_X_LPARAM(lp), y);
            t->has_sel = mark_cmp(t->sel_a, t->sel_b) != 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (t->selecting) { t->selecting = false; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED: t->selecting = false; return 0;
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        context_menu(t, pt);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_classes(void) {
    static bool done;
    if (done) return;
    done = true;
    WNDCLASSW wc = { 0 };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = term_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = TERM_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    RegisterClassW(&wc);
    WNDCLASSW ec = { 0 };
    ec.lpfnWndProc = events_proc;
    ec.hInstance = GetModuleHandleW(NULL);
    ec.lpszClassName = EVENTS_CLASS;
    RegisterClassW(&ec);
    g_events = CreateWindowExW(0, EVENTS_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    // The remote shell is told what this terminal understands.
    SetEnvironmentVariableW(L"TERM", L"xterm-256color");
}

bool term_is_window(HWND hwnd) {
    wchar_t name[64];
    return hwnd && GetClassNameW(hwnd, name, 64) && wcscmp(name, TERM_CLASS) == 0;
}

// MARK: - Sessions

Term *term_open(HWND parent, const TermTarget *target, char **error) {
    char *problem = term_target_problem(target);
    if (problem) { *error = problem; return NULL; }
    register_classes();
    fonts_update();
    Term *t = xcalloc(1, sizeof *t);
    t->id = g_next_id++;
    t->key = xstrdup(target->key ? target->key : "");
    t->group = xstrdup(target->group ? target->group : "");
    t->label = xstrdup(!str_empty(target->label) ? target->label : target->host);
    t->user = xstrdup(target->user); t->host = xstrdup(target->host); t->port = target->port;
    t->target = xstrfmt("%s@%s:%d", t->user, t->host, t->port);
    t->hwnd = CreateWindowExW(0, TERM_CLASS, L"", WS_CHILD | WS_VSCROLL | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), t);
    theme_apply_control(t->hwnd);
    // The first size is the parent's, near enough; the screen places the window exactly once it lays out.
    RECT pr; GetClientRect(parent, &pr);
    SetWindowPos(t->hwnd, NULL, 0, 0, pr.right - pr.left, pr.bottom - pr.top - px(100), SWP_NOZORDER | SWP_NOACTIVATE);
    size_cells(t, &t->cols, &t->rows);
    t->vt = vt_new(t->cols, t->rows, SCROLLBACK);
    if (!spawn(t, error)) {
        DestroyWindow(t->hwnd);
        vt_free(t->vt);
        free(t->key); free(t->group); free(t->label); free(t->user); free(t->host); free(t->target);
        free(t);
        return NULL;
    }
    if (g_count == g_cap) { g_cap = g_cap ? g_cap * 2 : 8; g_terms = xrealloc(g_terms, g_cap * sizeof *g_terms); }
    g_terms[g_count++] = t;
    g_active = t;
    scrollbar_update(t);
    notify();
    return t;
}

bool term_reconnect(Term *t, char **error) {
    if (!t || t->running) return true;
    if (!spawn(t, error)) return false;
    char *note = xstrfmt("\x1b[0;2m[Connecting to %s\xE2\x80\xA6]\x1b[0m\r\n", t->target);
    feed(t, note, strlen(note));
    free(note);
    notify();
    return true;
}

static void stop(Term *t) {
    if (t->wait) { UnregisterWaitEx(t->wait, INVALID_HANDLE_VALUE); t->wait = NULL; }
    if (t->process) { TerminateProcess(t->process, 1); CloseHandle(t->process); t->process = NULL; }
    if (t->in_write) { CloseHandle(t->in_write); t->in_write = NULL; }
    close_pc_async(t->pc);
    t->pc = NULL;
    t->running = false;
}

void term_close(Term *t) {
    if (!t) return;
    size_t i = 0;
    while (i < g_count && g_terms[i] != t) i++;
    if (i == g_count) return;
    memmove(g_terms + i, g_terms + i + 1, (g_count - i - 1) * sizeof *g_terms);
    g_count--;
    if (g_active == t) g_active = g_count ? g_terms[i < g_count ? i : g_count - 1] : NULL;
    stop(t);
    DestroyWindow(t->hwnd);
    vt_free(t->vt);
    free(t->key); free(t->group); free(t->label); free(t->user); free(t->host); free(t->target);
    free(t);
    notify();
}

void term_shutdown(void) {
    while (g_count) {
        Term *t = g_terms[--g_count];
        stop(t);
        vt_free(t->vt);
        free(t->key); free(t->group); free(t->label); free(t->user); free(t->host); free(t->target);
        free(t);
    }
    g_active = NULL;
}

size_t term_count(void) { return g_count; }
Term *term_at(size_t index) { return index < g_count ? g_terms[index] : NULL; }
Term *term_find(const char *key) {
    for (size_t i = 0; i < g_count; i++) if (str_eq(g_terms[i]->key, key)) return g_terms[i];
    return NULL;
}
size_t term_count_for(const char *key) {
    size_t n = 0;
    for (size_t i = 0; i < g_count; i++) if (str_eq(g_terms[i]->key, key)) n++;
    return n;
}
Term *term_active(void) { return g_active; }
void term_set_active(Term *t) {
    if (t == g_active) return;
    g_active = t;
    notify();
}
const char *term_group(const Term *t) { return t->group; }
const char *term_key(const Term *t) { return t->key; }
const char *term_label(const Term *t) { return t->label; }
const char *term_target(const Term *t) { return t->target; }
void term_get_target(const Term *t, TermTarget *out) { out->key = t->key; out->group = t->group; out->label = t->label; out->user = t->user; out->host = t->host; out->port = t->port; }
const char *term_title(const Term *t) {
    // Until the remote shell names the window, ConPTY titles it with the client's path.
    const char *title = vt_title(t->vt);
    return title && str_has_suffix(title, "ssh.exe") ? NULL : title;
}
bool term_running(const Term *t) { return t->running; }
HWND term_hwnd(const Term *t) { return t->hwnd; }

void term_place(Term *t, const RECT *rc, bool shown) {
    if (!t) return;
    if (shown && rc) {
        RECT now; GetWindowRect(t->hwnd, &now);
        MapWindowPoints(NULL, GetParent(t->hwnd), (POINT *)&now, 2);
        if (!EqualRect(&now, rc)) SetWindowPos(t->hwnd, HWND_TOP, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOACTIVATE);
    }
    if (shown != (IsWindowVisible(t->hwnd) != 0)) ShowWindow(t->hwnd, shown ? SW_SHOWNA : SW_HIDE);
}
void term_focus(Term *t) { if (t && IsWindowVisible(t->hwnd)) SetFocus(t->hwnd); }
