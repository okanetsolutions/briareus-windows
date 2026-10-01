// A project's SFTP sessions tab, beside its SSH sessions: the SSH servers registered for the project in Settings down
// the left, and its open SFTP sessions as tabs over the server's file tree on the right. A click on a server opens an SFTP
// session to it (or returns to the open one); the tree opens at the folder the server starts in, its folders open with a
// click, and files go up (⇧ Upload, or dropped from Explorer onto a folder) and down (⇩ Download, or a double click).
// Sessions run this PC's OpenSSH sftp client (sftp_session.c), so its keys, agent and ~/.ssh/config apply.
#include "dialogs.h"
#include "screens.h"
#include "sftp.h"
#include "sftp_session.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum {
    A_SERVER, A_TAB, A_TAB_CLOSE, A_RECONNECT, A_CLOSE, A_ROW, A_TWISTY, A_UPLOAD, A_DOWNLOAD, A_MKDIR, A_SHOW_DOWNLOAD,
    A_HOME,
};
enum { LIST_W = 260, TAB_H = 30, TAB_CLOSE_W = 22, ROW_H = 28, INDENT = 18, MAX_INSTANCES = 16 };
enum { MENU_DOWNLOAD = 1, MENU_UPLOAD, MENU_UPLOAD_FOLDER, MENU_MKDIR, MENU_RENAME, MENU_DELETE, MENU_REFRESH, MENU_COPY_PATH };

struct ProjectSftp {
    char *repo;
    Screen *host;
    int base;
    Json *rows;               // the project's SshServer rows
    bool loaded, loading;
    char *error;
    Request *req;
    char **paths; int *tops, *bottoms; size_t path_count;   // the tree's rows as last laid out, in content coordinates
    bool dropping;            // the pane takes files dropped from Explorer
    char *last_click; DWORD last_click_at;
};

static ProjectSftp *g_instances[MAX_INSTANCES];

bool project_sftp_offered(void) { return store_supports("settings_ssh_servers"); }

static char *server_key(const Json *row) {
    double id;
    if (json_num(json_get(row, "id"), &id) && isfinite(id)) return xstrfmt("sftp:%.0f", id);
    const char *host = json_str(json_get(row, "host"));
    return xstrfmt("sftp:%s", host ? host : "");
}
static char *server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}
static int server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }

/// The project's session on show: the active one when it is the project's, else its first.
static SftpSession *shown_session(ProjectSftp *p) {
    SftpSession *active = sftp_active();
    if (active && str_eq(sftp_group(active), p->repo)) return active;
    for (size_t i = 0; i < sftp_count(); i++) if (str_eq(sftp_group(sftp_at(i)), p->repo)) return sftp_at(i);
    return NULL;
}
size_t project_sftp_session_count(const char *repo) {
    size_t n = 0;
    for (size_t i = 0; i < sftp_count(); i++) if (str_eq(sftp_group(sftp_at(i)), repo)) n++;
    return n;
}
static HWND owner(ProjectSftp *p) { return p->host->pane ? pane_hwnd(p->host->pane) : NULL; }

static void relayout(ProjectSftp *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static void sessions_changed(void *ctx) { relayout(ctx); }

// MARK: - Loading

static void rows_done(void *ctx, Request *req) {
    ProjectSftp *p = ctx;
    p->loaded = true; p->loading = false;
    if (!req->ok) request_error_into(&p->error, req);
    else {
        set_string(&p->error, NULL);
        json_free(p->rows); p->rows = json_array();
        const Json *all = json_get(req->result, "servers");
        for (size_t i = 0; i < json_count(all); i++)
            if (str_eq(json_str(json_get(json_at(all, i), "repo")), p->repo)) json_array_push(p->rows, json_clone(json_at(all, i)));
    }
    relayout(p);
}
void project_sftp_load(ProjectSftp *p) {
    if (p->loaded || p->req) return;
    if (!project_sftp_offered()) { p->loaded = true; return; }
    p->loading = true;
    store_call("settings_ssh_servers", json_object(), 0, p, rows_done, 0, &p->req);
}
void project_sftp_refresh(ProjectSftp *p) {
    request_cancel(&p->req);
    p->loaded = false;
    project_sftp_load(p);
    // ⟳ also reads the folder on show again.
    SftpSession *s = shown_session(p);
    if (s && sftp_state(s) == SFTP_READY) {
        const char *sel = sftp_selected(s);
        SftpNode *n = sftp_node(s, sel);
        char *dir = n && (n->dir || n->link) ? xstrdup(n->path) : sel ? sftp_parent(sel) : NULL;
        if (dir) sftp_list(s, dir);
        free(dir);
    }
}
void servers_sftp_changed(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] && (g_instances[i]->loaded || g_instances[i]->req)) project_sftp_refresh(g_instances[i]);
}

// MARK: - Connecting

static void connect_row(ProjectSftp *p, const Json *row) {
    if (!json_is_object(row)) return;
    char *key = server_key(row), *name = server_name(row);
    SftpSession *open = sftp_find(key);
    if (open) sftp_set_active(open);
    else {
        TermTarget target = { key, p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), server_port(row) };
        char *error = NULL;
        if (!sftp_open(&target, &error)) {
            char *title = xstrfmt("Could not connect to %s", name);
            app_alert(title, error ? error : "The session could not start.");
            free(title);
        }
        free(error);
    }
    free(key); free(name);
    relayout(p);
}
static void close_session(SftpSession *s) {
    if (!s) return;
    size_t queued = 0;
    const char *busy = sftp_busy(s, &queued);
    if (sftp_state(s) != SFTP_CLOSED && busy) {
        char *title = xstrfmt("Disconnect from %s?", sftp_label(s));
        bool ok = app_confirm(title, "A transfer is still running; disconnecting stops it, and the transfers waiting behind it are dropped.", "Disconnect", true);
        free(title);
        if (!ok) return;
    }
    sftp_close(s);
}

// MARK: - Files

/// The folder new files go to: the selected folder, the selected file's folder, else the folder the server started in.
static char *target_folder(SftpSession *s) {
    const char *sel = sftp_selected(s);
    SftpNode *n = sftp_node(s, sel);
    if (n && (n->dir || (n->link && n->kid_count))) return xstrdup(n->path);
    if (sel) { char *parent = sftp_parent(sel); if (parent) return parent; }
    return xstrdup(sftp_home(s) ? sftp_home(s) : "/");
}
static bool exists_in(SftpSession *s, const char *folder, const char *name) {
    SftpNode *n = sftp_node(s, folder);
    if (!n) return false;
    for (size_t i = 0; i < n->kid_count; i++) if (str_eq(n->kids[i]->name, name)) return true;
    return false;
}
static const wchar_t *wide_base(const wchar_t *path) {
    const wchar_t *b = path;
    for (const wchar_t *q = path; *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
    return b;
}

/// Queues local files and folders for upload into `folder`, after asking before replacing what is there.
static void upload_paths(SftpSession *s, const char *folder, wchar_t **paths, size_t n) {
    if (!n) return;
    size_t clashes = 0; char *first = NULL;
    for (size_t i = 0; i < n; i++) {
        char *name = wide_to_utf8(wide_base(paths[i]));
        if (exists_in(s, folder, name)) { if (!clashes++) first = xstrdup(name); }
        free(name);
    }
    if (clashes) {
        char *title = clashes == 1 ? xstrfmt("Replace %s?", first) : xstrfmt("Replace %zu items?", clashes);
        char *message = xstrfmt("%s already %s in %s. Uploading replaces %s.", clashes == 1 ? first : "Some of them", clashes == 1 ? "exists" : "exist", folder, clashes == 1 ? "it" : "them");
        bool ok = app_confirm(title, message, "Replace", true);
        free(title); free(message);
        if (!ok) { free(first); return; }
    }
    free(first);
    for (size_t i = 0; i < n; i++) {
        DWORD attrs = GetFileAttributesW(paths[i]);
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        sftp_upload(s, paths[i], folder, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
}

static void upload_files(ProjectSftp *p, SftpSession *s, const char *folder) {
    enum { CHARS = 32768 };
    wchar_t *buf = xcalloc(CHARS, sizeof *buf);
    char *title = xstrfmt("Upload to %s", folder);
    wchar_t *wtitle = utf8_to_wide(title);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = owner(p);
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = CHARS;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    bool ok = GetOpenFileNameW(&ofn);
    free(title); free(wtitle);
    if (!ok) {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) app_alert("Upload", "Too many files at once. Pick fewer.");
        free(buf); return;
    }
    // Several files come back as the folder, then each name, NUL-separated, ending in two NULs; one as its full path.
    const wchar_t *dir = buf, *name = buf + wcslen(buf) + 1;
    wchar_t **paths = NULL; size_t n = 0;
    if (!*name) { paths = xmalloc(sizeof *paths); paths[n++] = _wcsdup(dir); }
    else for (; *name; name += wcslen(name) + 1) {
        size_t len = wcslen(dir) + 1 + wcslen(name) + 1;
        wchar_t *path = xmalloc(len * sizeof *path);
        _snwprintf(path, len, L"%ls\\%ls", dir, name); path[len - 1] = 0;
        paths = xrealloc(paths, (n + 1) * sizeof *paths);
        paths[n++] = path;
    }
    upload_paths(s, folder, paths, n);
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths); free(buf);
}

/// A local folder from the folder picker; the caller frees it.
static wchar_t *pick_folder(HWND parent, const char *title) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->lpVtbl->GetOptions(dialog, &options);
    dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    wchar_t *wtitle = utf8_to_wide(title);
    dialog->lpVtbl->SetTitle(dialog, wtitle);
    free(wtitle);
    wchar_t *out = NULL;
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, parent))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item))) {
            wchar_t *path = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) { out = _wcsdup(path); CoTaskMemFree(path); }
            item->lpVtbl->Release(item);
        }
    }
    dialog->lpVtbl->Release(dialog);
    return out;
}

static void upload_folder(ProjectSftp *p, SftpSession *s, const char *folder) {
    char *title = xstrfmt("Upload a folder to %s", folder);
    wchar_t *local = pick_folder(owner(p), title);
    free(title);
    if (!local) return;
    upload_paths(s, folder, &local, 1);
    free(local);
}

static void download(ProjectSftp *p, SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n || str_eq(path, "/")) return;
    wchar_t *wname = utf8_to_wide(n->name);
    if (n->dir || (n->link && n->kid_count)) {
        char *title = xstrfmt("Download %s into", n->name);
        wchar_t *into = pick_folder(owner(p), title);
        free(title);
        if (into) {
            size_t len = wcslen(into) + 1 + wcslen(wname) + 1;
            wchar_t *local = xmalloc(len * sizeof *local);
            _snwprintf(local, len, L"%ls\\%ls", into, wname); local[len - 1] = 0;
            if (GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES
                || app_confirm("Merge folders?", "A folder with this name is already there; the download adds to it and replaces files with the same names.", "Download", true))
                sftp_download(s, path, local, true);
            free(local); free(into);
        }
    } else {
        wchar_t file[MAX_PATH * 2];
        // Windows does not allow every name a server does: the default name drops what it refuses.
        _snwprintf(file, MAX_PATH * 2, L"%ls", wname); file[MAX_PATH * 2 - 1] = 0;
        for (wchar_t *q = file; *q; q++) if (wcschr(L"<>:\"/\\|?*", *q) || *q < 32) *q = L'_';
        wchar_t *downloads = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &downloads))) downloads = NULL;
        OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn; ofn.hwndOwner = owner(p);
        ofn.lpstrFilter = L"All files\0*.*\0";
        ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrInitialDir = downloads;
        ofn.lpstrTitle = L"Download";
        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&ofn)) sftp_download(s, path, file, false);
        if (downloads) CoTaskMemFree(downloads);
    }
    free(wname);
}

static void make_folder(ProjectSftp *p, SftpSession *s, const char *folder) {
    char *label = xstrfmt("Name of the new folder in %s", folder);
    char *name = dialog_text(owner(p), "New folder", label, "Create", "");
    free(label);
    char *trimmed = name ? str_trim(name) : NULL;
    if (trimmed && *trimmed) {
        if (strchr(trimmed, '/')) app_alert("New folder", "A folder name cannot hold a slash.");
        else { char *path = sftp_join(folder, trimmed); sftp_mkdir(s, path); free(path); }
    }
    free(trimmed); free(name);
}
static void rename_node(ProjectSftp *p, SftpSession *s, const char *path) {
    if (str_eq(path, "/")) return;
    char *name = dialog_text(owner(p), "Rename", "New name", "Rename", sftp_basename(path));
    char *trimmed = name ? str_trim(name) : NULL;
    if (trimmed && *trimmed && !str_eq(trimmed, sftp_basename(path))) {
        if (strchr(trimmed, '/')) app_alert("Rename", "A name cannot hold a slash.");
        else { char *parent = sftp_parent(path), *to = sftp_join(parent, trimmed); sftp_rename(s, path, to); free(parent); free(to); }
    }
    free(trimmed); free(name);
}
static void delete_node(SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n || str_eq(path, "/")) return;
    char *title = xstrfmt("Delete %s?", n->name);
    bool ok = app_confirm(title, n->dir ? "The folder is deleted on the server. Only an empty folder can be deleted." : "The file is deleted on the server; this cannot be undone.", "Delete", true);
    free(title);
    if (ok) sftp_remove(s, path, n->dir);
}
static void copy_text(HWND hwnd, const char *text) {
    wchar_t *w = utf8_to_wide(text);
    size_t bytes = (wcslen(w) + 1) * sizeof *w;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem && OpenClipboard(hwnd)) {
        memcpy(GlobalLock(mem), w, bytes); GlobalUnlock(mem);
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, mem)) mem = NULL;
        CloseClipboard();
    }
    if (mem) GlobalFree(mem);
    free(w);
}

// MARK: - Dropping files from Explorer

static LRESULT CALLBACK drop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ProjectSftp *p = (ProjectSftp *)ref;
    if (msg == WM_DROPFILES) {
        HDROP drop = (HDROP)wp;
        SftpSession *s = shown_session(p);
        if (!s || sftp_state(s) != SFTP_READY) { DragFinish(drop); return 0; }
        // Onto a folder's row: into it; onto a file's: beside it; elsewhere: the selected folder.
        POINT pt; DragQueryPoint(drop, &pt);
        RECT content = pane_content_rect(p->host->pane);
        int y = pt.y - content.top + pane_scroll_y(p->host->pane);
        char *folder = NULL;
        for (size_t i = 0; i < p->path_count && !folder; i++) {
            if (y < p->tops[i] || y >= p->bottoms[i]) continue;
            SftpNode *n = sftp_node(s, p->paths[i]);
            if (n && (n->dir || (n->link && n->kid_count))) folder = xstrdup(n->path);
            else folder = sftp_parent(p->paths[i]);
        }
        if (!folder) folder = target_folder(s);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        wchar_t **paths = xcalloc(count ? count : 1, sizeof *paths);
        size_t n = 0;
        for (UINT i = 0; i < count; i++) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            wchar_t *path = xmalloc((len + 1) * sizeof *path);
            DragQueryFileW(drop, i, path, len + 1);
            paths[n++] = path;
        }
        DragFinish(drop);
        SetForegroundWindow(GetAncestor(hwnd, GA_ROOT));
        upload_paths(s, folder, paths, n);
        for (size_t i = 0; i < n; i++) free(paths[i]);
        free(paths); free(folder);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, drop_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void set_dropping(ProjectSftp *p, bool on) {
    if (on == p->dropping || !p->host->pane) return;
    HWND hwnd = pane_hwnd(p->host->pane);
    p->dropping = on;
    if (on) { SetWindowSubclass(hwnd, drop_proc, (UINT_PTR)p, (DWORD_PTR)p); DragAcceptFiles(hwnd, TRUE); }
    else { DragAcceptFiles(hwnd, FALSE); RemoveWindowSubclass(hwnd, drop_proc, (UINT_PTR)p); }
}

// MARK: - Layout

typedef struct { char *name, *target; int sessions; bool live, enabled; } ServerRowData;
static void server_row_free(void *v) { ServerRowData *d = v; free(d->name); free(d->target); free(d); }
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(6), top = rc->top + px(5);
    RECT icon = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, 0xE8B7, &icon, FONT_ICON_SMALL, d->live ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    if (d->sessions) {
        char n[16]; snprintf(n, sizeof n, "%d", d->sessions);
        int nw = text_width(cv, n, FONT_CAPTION);
        RECT cr = { right - nw, rc->top, right, rc->bottom };
        draw_text(cv, n, &cr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        draw_status_dot(cv, cr.left - px(8), (rc->top + rc->bottom) / 2, d->live ? "idle" : "closed");
        right = cr.left - px(16);
    }
    int tx = icon.right + px(6);
    RECT t = { tx, top, right, top + px(20) };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE, d->enabled ? theme.ink : theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sub = { tx, top + px(20), right, top + px(36) };
    draw_text(cv, d->target, &sub, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_rect(cv, rc, theme.line); }

typedef struct { char *label; bool active, live; } TabData;
static void tab_free(void *v) { TabData *d = v; free(d->label); free(d); }
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF soft = blend(theme.raise, theme.canvas, 0.5);
    if (d->active || hovered) fill_round_rect(cv, rc, px(6), d->active ? theme.raise : soft, d->active ? theme.line : soft);
    if (d->active) { RECT bar = { rc->left + px(8), rc->bottom - px(2), rc->right - px(8), rc->bottom }; fill_rect(cv, &bar, theme.accent); }
    int x = rc->left + px(10), cy = (rc->top + rc->bottom) / 2;
    draw_status_dot(cv, x + px(3), cy, d->live ? "idle" : "closed");
    RECT t = { x + px(12), rc->top, rc->right - px(TAB_CLOSE_W), rc->bottom };
    draw_text(cv, d->label, &t, d->active ? FONT_CAPTION_SEMIBOLD : FONT_CAPTION, d->active ? theme.ink : theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_tab_close(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(4), theme.line, theme.line);
    draw_glyph(cv, 0xE711, rc, FONT_ICON_SMALL, hovered ? theme.ink : theme.muted);
}

/// A row of the tree: its twisty, its icon, its name, and a file's size and date.
typedef struct { char *name, *size, *when; int depth; bool folder, open, link, selected, loading, home; } RowData;
static void row_free(void *v) { RowData *d = v; free(d->name); free(d->size); free(d->when); free(d); }
static int twisty_left(const RECT *rc, int depth) { return rc->left + px(6) + depth * px(INDENT); }
static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->selected) fill_round_rect(cv, rc, px(5), theme.raise, theme.line);
    else if (hovered) fill_round_rect(cv, rc, px(5), blend(theme.raise, theme.canvas, 0.5), blend(theme.raise, theme.canvas, 0.5));
    int x = twisty_left(rc, d->depth);
    RECT tw = { x, rc->top, x + px(16), rc->bottom };
    if (d->folder) draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &tw, FONT_ICON_SMALL, theme.muted);
    RECT icon = { tw.right + px(2), rc->top, tw.right + px(22), rc->bottom };
    wchar_t glyph = d->folder ? (d->home ? 0xE80F : d->open ? 0xE838 : 0xE8B7) : d->link ? 0xE71B : 0xE8A5;
    draw_glyph(cv, glyph, &icon, FONT_ICON_SMALL, d->folder ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    int when_w = px(120), size_w = px(80);
    if (d->when) {
        RECT wr = { right - when_w, rc->top, right, rc->bottom };
        draw_text(cv, d->when, &wr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    right -= when_w + px(12);
    if (d->size) {
        RECT sr = { right - size_w, rc->top, right, rc->bottom };
        draw_text(cv, d->size, &sr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    right -= size_w + px(12);
    RECT t = { icon.right + px(6), rc->top, right, rc->bottom };
    draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_columns(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    int right = rc->right - px(8);
    RECT wr = { right - px(120), rc->top, right, rc->bottom };
    draw_text(cv, "Modified", &wr, FONT_CAPTION2, theme.tertiary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    right -= px(120 + 12);
    RECT sr = { right - px(80), rc->top, right, rc->bottom };
    draw_text(cv, "Size", &sr, FONT_CAPTION2, theme.tertiary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT nr = { rc->left + px(6), rc->top, sr.left, rc->bottom };
    draw_text(cv, "Name", &nr, FONT_CAPTION2, theme.tertiary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/// The servers down the left, each with its name, user@host:port and its open sessions.
static void layout_list(ProjectSftp *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading servers\xE2\x80\xA6"); return; }
    for (size_t i = 0; i < json_count(p->rows); i++) {
        const Json *row = json_at(p->rows, i);
        ServerRowData *d = xcalloc(1, sizeof *d);
        d->name = server_name(row);
        const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
        d->target = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", server_port(row));
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        char *key = server_key(row);
        d->sessions = (int)sftp_count_for(key);
        for (size_t t = 0; t < sftp_count(); t++) if (str_eq(sftp_key(sftp_at(t)), key) && sftp_state(sftp_at(t)) != SFTP_CLOSED) d->live = true;
        free(key);
        doc_custom(doc, x, w, px(46), paint_server, d, server_row_free, p->base + A_SERVER, (intptr_t)i);
    }
    if (!json_count(p->rows) && !p->error)
        doc_text(doc, x + px(6), w - px(12), "No SSH servers for this project. Register one under \xE2\x9A\x99 Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

/// The project's sessions as tabs, wrapping, from `x` across `w`.
static void layout_tabs(ProjectSftp *p, Doc *doc, int x0, int w) {
    int x = x0, y = doc->y, h = px(TAB_H), gap = px(4);
    SftpSession *shown = shown_session(p);
    for (size_t i = 0; i < sftp_count(); i++) {
        SftpSession *s = sftp_at(i);
        if (!str_eq(sftp_group(s), p->repo)) continue;
        TabData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(sftp_label(s)); d->active = s == shown; d->live = sftp_state(s) != SFTP_CLOSED;
        int tw = text_width(doc->cv, d->label, FONT_CAPTION_SEMIBOLD) + px(10 + 12 + TAB_CLOSE_W + 6);
        if (tw > px(240)) tw = px(240);
        if (tw < px(110)) tw = px(110);
        if (x > x0 && x + tw > x0 + w) { x = x0; y += h + gap; }
        RECT tr = { x, y, x + tw, y + h };
        Item *it = doc_item(doc, doc_add(doc, &tr, paint_tab));
        it->data = d; it->free_data = tab_free; it->action = p->base + A_TAB; it->arg = (intptr_t)i; it->hand = true;
        RECT cr = { tr.right - px(TAB_CLOSE_W) - px(2), y + (h - px(18)) / 2, tr.right - px(4), y + (h - px(18)) / 2 + px(18) };
        Item *c = doc_item(doc, doc_add(doc, &cr, paint_tab_close));
        c->action = p->base + A_TAB_CLOSE; c->arg = (intptr_t)i; c->hand = true;
        x += tw + gap;
    }
    doc->y = y + h;
}

static void forget_rows(ProjectSftp *p) {
    for (size_t i = 0; i < p->path_count; i++) free(p->paths[i]);
    free(p->paths); free(p->tops); free(p->bottoms);
    p->paths = NULL; p->tops = p->bottoms = NULL; p->path_count = 0;
}
static void remember_row(ProjectSftp *p, const char *path, int top, int bottom) {
    p->paths = xrealloc(p->paths, (p->path_count + 1) * sizeof *p->paths);
    p->tops = xrealloc(p->tops, (p->path_count + 1) * sizeof *p->tops);
    p->bottoms = xrealloc(p->bottoms, (p->path_count + 1) * sizeof *p->bottoms);
    p->paths[p->path_count] = xstrdup(path); p->tops[p->path_count] = top; p->bottoms[p->path_count] = bottom;
    p->path_count++;
}

/// A folder's open entries under it, depth first.
static void layout_node(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w, SftpNode *n, int depth) {
    bool folder = n->dir || (n->link && (!n->loaded || n->kid_count));
    RowData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(n->name); d->depth = depth; d->folder = folder; d->open = folder && n->expanded; d->link = n->link;
    d->selected = str_eq(sftp_selected(s), n->path);
    d->home = str_eq(n->path, sftp_home(s));
    if (!folder) d->size = sftp_format_size(n->size);
    d->when = n->when ? xstrdup(n->when) : NULL;
    int top = doc->y;
    size_t index = p->path_count;
    remember_row(p, n->path, top, top + px(ROW_H));
    doc_custom(doc, x, w, px(ROW_H), paint_row, d, row_free, p->base + A_ROW, (intptr_t)index);
    if (folder) {
        // The twisty opens and closes without selecting.
        RECT tw = { twisty_left(&(RECT){ x, top, x + w, top }, depth) - px(4), top, twisty_left(&(RECT){ x, top, x + w, top }, depth) + px(18), top + px(ROW_H) };
        Item *t = doc_item(doc, doc_add(doc, &tw, NULL));
        t->action = p->base + A_TWISTY; t->arg = (intptr_t)index; t->hand = true;
    }
    if (!folder || !n->expanded) return;
    int inner = x + (depth + 1) * px(INDENT) + px(28);
    if (n->loading && !n->kid_count) {
        doc_text(doc, inner, w - (inner - x), "Loading\xE2\x80\xA6", FONT_CAPTION, theme.muted, DT_SINGLELINE);
        doc_space(doc, px(4));
    }
    if (n->error) {
        doc_text(doc, inner, w - (inner - x), n->error, FONT_CAPTION, theme.danger, DT_WORDBREAK);
        doc_space(doc, px(4));
    } else if (n->loaded && !n->loading && !n->kid_count) {
        doc_text(doc, inner, w - (inner - x), "Empty folder", FONT_CAPTION, theme.tertiary, DT_SINGLELINE);
        doc_space(doc, px(4));
    }
    for (size_t i = 0; i < n->kid_count; i++) layout_node(p, s, doc, x, w, n->kids[i], depth + 1);
}

/// One line over the tree, always there so the rows do not move: what runs now, how the last transfer or change went
/// (with "Show in folder" after a download), else how to move files.
typedef struct { char *text; wchar_t glyph; COLORREF color; bool busy, show; } StatusData;
static void status_free(void *v) { StatusData *d = v; free(d->text); free(d); }
static void paint_status(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StatusData *d = it->data;
    int right = rc->right - px(4);
    if (d->show) {
        const char *link = "Show in folder";
        int lw = text_width(cv, link, FONT_CAPTION_SEMIBOLD);
        RECT lr = { right - lw, rc->top, right, rc->bottom };
        draw_text(cv, link, &lr, FONT_CAPTION_SEMIBOLD, doc_item_hovered(doc, it) ? theme.ink : theme.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        right = lr.left - px(16);
    }
    RECT g = { rc->left, rc->top, rc->left + px(18), rc->bottom };
    if (d->glyph) draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->color);
    RECT t = { d->glyph ? g.right + px(6) : rc->left + px(6), rc->top, right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_CAPTION, d->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void layout_status(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w) {
    StatusData *d = xcalloc(1, sizeof *d);
    size_t queued = 0;
    const char *busy = sftp_busy(s, &queued);
    bool failed = false;
    const char *status = sftp_status(s, &failed);
    if (busy) {
        d->text = queued ? xstrfmt("%s \xC2\xB7 %zu more waiting", busy, queued) : xstrdup(busy);
        d->glyph = 0xE895; d->color = theme.accent; d->busy = true;
    } else if (status) {
        // Several complaints share the line.
        d->text = str_replace(status, "\n", " \xC2\xB7 ");
        d->glyph = failed ? 0xE783 : 0xE73E; d->color = failed ? theme.danger : theme.secondary;
        d->show = !failed && sftp_last_download(s) && str_has_prefix(status, "Downloaded ");
    } else {
        d->text = xstrdup(sftp_state(s) == SFTP_READY ? "Drop files or folders from Explorer onto a folder to upload them there; double-click a file to download it; right-click for more."
                                                      : "Not connected.");
        d->color = theme.tertiary;
    }
    doc_custom(doc, x, w, px(24), paint_status, d, status_free, d->show ? p->base + A_SHOW_DOWNLOAD : 0, 0);
    doc_space(doc, px(4));
}

static void layout_session(ProjectSftp *p, SftpSession *s, Doc *doc, int x, int w) {
    SftpState state = sftp_state(s);
    if (state != SFTP_CONNECTING) layout_status(p, s, doc, x, w);
    if (state == SFTP_CONNECTING) {
        doc_space(doc, px(30));
        char *line = xstrfmt("Connecting to %s\xE2\x80\xA6", sftp_target(s));
        doc_loading(doc, x, w, line);
        free(line);
        doc_space(doc, px(8));
        doc_text(doc, x, w, "A password or an unknown host key is asked for in a window of its own.", FONT_CAPTION, theme.muted, DT_CENTER | DT_WORDBREAK);
        return;
    }
    SftpNode *root = sftp_root(s);
    if (!root) {
        const char *log = sftp_connect_log(s);
        char *message = xstrfmt("Could not connect to %s.%s%s", sftp_target(s), log ? "\n" : "", log ? log : "");
        doc_notice(doc, x, w, message);
        free(message);
        doc_space(doc, px(8));
        doc_button(doc, x, 0, "Connect again", BUTTON_BORDERED, p->base + A_RECONNECT, 0, true);
        return;
    }
    if (state == SFTP_CLOSED) {
        doc_text(doc, x, w, "The session is closed; Reconnect opens it again.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(6));
    }
    doc_custom(doc, x, w, px(22), paint_columns, NULL, NULL, 0, 0);
    doc_rule(doc, x, w);
    doc_space(doc, px(2));
    layout_node(p, s, doc, x, w, root, 0);
    doc_space(doc, px(10));
}

void project_sftp_layout(ProjectSftp *p, Doc *doc, int w) {
    forget_rows(p);
    if (!project_sftp_offered()) {
        doc_text(doc, 0, w, "The SSH servers are read from Settings, which needs an Admin token. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_sftp_load(p);
    RECT view = pane_content_rect(p->host->pane);
    int top = doc->y, bottom = top + (view.bottom - view.top) - top - px(14);
    if (bottom < top + px(240)) bottom = top + px(240);
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    layout_list(p, doc, 0, lw);
    int list_bottom = doc->y;
    int rx = lw + px(18), rw = w - rx;
    doc->y = top;
    SftpSession *s = shown_session(p);
    if (!s) {
        doc_space(doc, px(40));
        doc_empty_state(doc, rx, rw, 0xE8B7, "No open sessions",
                        "Click a server on the left to browse its files here, and upload and download them. It connects from this PC straight to the "
                        "server with your own OpenSSH client, so your keys, ssh-agent and ~/.ssh/config apply; passwords are asked for in a window.");
    } else {
        layout_tabs(p, doc, rx, rw);
        doc_space(doc, px(10));
        layout_session(p, s, doc, rx, rw);
    }
    int right_bottom = doc->y;
    if (right_bottom > bottom) bottom = right_bottom;
    // A rule between the list and the sessions.
    RECT rule = { lw + px(8), top, lw + px(9), bottom };
    doc_add(doc, &rule, paint_rule);
    doc->y = bottom > list_bottom ? bottom : list_bottom;
}

void project_sftp_header(ProjectSftp *p, HeaderInfo *info) {
    SftpSession *s = shown_session(p);
    if (!s) return;
    SftpState state = sftp_state(s);
    const char *where = sftp_selected(s);
    snprintf(info->subtitle, sizeof info->subtitle, "SFTP %s \xC2\xB7 %s%s%s", sftp_target(s),
             state == SFTP_READY ? "connected" : state == SFTP_CONNECTING ? "connecting" : "closed", where ? " \xC2\xB7 " : "", where ? where : "");
    snprintf(info->status, sizeof info->status, "%s", state == SFTP_CLOSED ? "closed" : "idle");
    bool ready = state == SFTP_READY;
    SftpNode *sel = sftp_node(s, sftp_selected(s));
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = 0xE898; b->action = p->base + A_UPLOAD; b->enabled = ready; b->prominent = true; b->tip = "Upload files into the selected folder";
    snprintf(b->label, sizeof b->label, "Upload");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE896; b->action = p->base + A_DOWNLOAD; b->enabled = ready && sel && !str_eq(sel->path, "/"); b->tip = "Download the selected file or folder";
    snprintf(b->label, sizeof b->label, "Download");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE8F4; b->action = p->base + A_MKDIR; b->enabled = ready; b->tip = "New folder in the selected folder";
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE80F; b->action = p->base + A_HOME; b->enabled = ready && sftp_home(s); b->tip = "Go to the folder the server starts in";
    if (state == SFTP_CLOSED) {
        b = &info->buttons[info->button_count++];
        b->glyph = 0xE72C; b->action = p->base + A_RECONNECT; b->enabled = true; b->tip = "Connect this session again";
        snprintf(b->label, sizeof b->label, "Reconnect");
    }
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE711; b->action = p->base + A_CLOSE; b->enabled = true; b->tip = "Close this session";
}

void project_sftp_place(ProjectSftp *p, const RECT *content, int scroll_y, bool shown) {
    (void)content; (void)scroll_y;
    set_dropping(p, shown && project_sftp_offered());
}

// MARK: - Actions

static SftpSession *session_by_index(intptr_t arg) { return arg >= 0 ? sftp_at((size_t)arg) : NULL; }
static const char *row_path(ProjectSftp *p, intptr_t arg) { return arg >= 0 && (size_t)arg < p->path_count ? p->paths[arg] : NULL; }

static void click_row(ProjectSftp *p, SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n) return;
    bool folder = n->dir || (n->link && (!n->loaded || n->kid_count));
    // A second click on the same row soon after is a double click: it opens a file by downloading it.
    DWORD now = GetTickCount();
    bool twice = str_eq(p->last_click, path) && now - p->last_click_at <= GetDoubleClickTime();
    set_string(&p->last_click, twice ? NULL : path);
    p->last_click_at = now;
    char *keep = xstrdup(path);
    if (twice) { if (!folder && sftp_state(s) == SFTP_READY) download(p, s, keep); free(keep); return; }
    sftp_select(s, keep);
    if (folder) sftp_toggle(s, keep);
    free(keep);
}

bool project_sftp_action(ProjectSftp *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_SFTP_ACTIONS) return false;
    SftpSession *s = shown_session(p);
    switch (action - p->base) {
    case A_SERVER: connect_row(p, json_at(p->rows, (size_t)arg)); break;
    case A_TAB: sftp_set_active(session_by_index(arg)); break;
    case A_TAB_CLOSE: close_session(session_by_index(arg)); break;
    case A_CLOSE: close_session(s); break;
    case A_RECONNECT: {
        char *error = NULL;
        if (s && !sftp_reconnect(s, &error)) app_alert("Could not reconnect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    case A_ROW: { const char *path = row_path(p, arg); if (s && path) click_row(p, s, path); break; }
    case A_TWISTY: { const char *path = row_path(p, arg); if (s && path) { char *keep = xstrdup(path); sftp_toggle(s, keep); free(keep); } break; }
    case A_UPLOAD: if (s && sftp_state(s) == SFTP_READY) { char *folder = target_folder(s); upload_files(p, s, folder); free(folder); } break;
    case A_DOWNLOAD: if (s && sftp_state(s) == SFTP_READY && sftp_selected(s)) { char *path = xstrdup(sftp_selected(s)); download(p, s, path); free(path); } break;
    case A_MKDIR: if (s && sftp_state(s) == SFTP_READY) { char *folder = target_folder(s); make_folder(p, s, folder); free(folder); } break;
    case A_HOME: if (s && sftp_home(s)) { char *home = xstrdup(sftp_home(s)); sftp_reveal(s, home); free(home); } break;
    case A_SHOW_DOWNLOAD: {
        const wchar_t *local = s ? sftp_last_download(s) : NULL;
        if (!local) break;
        size_t len = wcslen(local) + 16;
        wchar_t *args = xmalloc(len * sizeof *args);
        _snwprintf(args, len, L"/select,\"%ls\"", local); args[len - 1] = 0;
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        free(args);
        break;
    }
    }
    relayout(p);
    return true;
}

bool project_sftp_context(ProjectSftp *p, int action, intptr_t arg, POINT pt) {
    if (action != p->base + A_ROW) return action >= p->base && action < p->base + PROJECT_SFTP_ACTIONS;
    SftpSession *s = shown_session(p);
    const char *row = row_path(p, arg);
    if (!s || !row) return true;
    char *path = xstrdup(row);
    sftp_select(s, path);
    SftpNode *n = sftp_node(s, path);
    bool ready = sftp_state(s) == SFTP_READY, folder = n && (n->dir || (n->link && n->kid_count)), root = str_eq(path, "/");
    UINT on = ready ? 0 : MF_GRAYED;
    HMENU menu = CreatePopupMenu();
    if (!root) AppendMenuW(menu, MF_STRING | on, MENU_DOWNLOAD, folder ? L"Download folder\x2026" : L"Download\x2026");
    if (folder) {
        AppendMenuW(menu, MF_STRING | on, MENU_UPLOAD, L"Upload files here\x2026");
        AppendMenuW(menu, MF_STRING | on, MENU_UPLOAD_FOLDER, L"Upload a folder here\x2026");
        AppendMenuW(menu, MF_STRING | on, MENU_MKDIR, L"New folder\x2026");
        AppendMenuW(menu, MF_STRING | on, MENU_REFRESH, L"Refresh");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_COPY_PATH, L"Copy path");
    if (!root) {
        AppendMenuW(menu, MF_STRING | on, MENU_RENAME, L"Rename\x2026");
        AppendMenuW(menu, MF_STRING | on, MENU_DELETE, L"Delete");
    }
    relayout(p);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, owner(p), NULL);
    DestroyMenu(menu);
    // The menu ran a message loop: the session may have closed meanwhile.
    s = shown_session(p);
    if (!s || !sftp_node(s, path)) chosen = chosen == MENU_COPY_PATH ? chosen : 0;
    switch (chosen) {
    case MENU_DOWNLOAD: download(p, s, path); break;
    case MENU_UPLOAD: upload_files(p, s, path); break;
    case MENU_UPLOAD_FOLDER: upload_folder(p, s, path); break;
    case MENU_MKDIR: make_folder(p, s, path); break;
    case MENU_REFRESH: sftp_list(s, path); break;
    case MENU_RENAME: rename_node(p, s, path); break;
    case MENU_DELETE: delete_node(s, path); break;
    case MENU_COPY_PATH: copy_text(owner(p), path); break;
    }
    free(path);
    relayout(p);
    return true;
}

// MARK: - Lifetime

ProjectSftp *project_sftp_new(const char *repo, Screen *host, int action_base) {
    ProjectSftp *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo ? repo : "");
    p->host = host; p->base = action_base;
    p->rows = json_array();
    for (int i = 0; i < MAX_INSTANCES; i++) if (!g_instances[i]) { g_instances[i] = p; break; }
    sftp_add_listener(sessions_changed, p);
    return p;
}
void project_sftp_free(ProjectSftp *p) {
    if (!p) return;
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] == p) g_instances[i] = NULL;
    sftp_remove_listener(sessions_changed, p);
    set_dropping(p, false);
    // The sessions go on, for the tab to come back to.
    request_cancel(&p->req);
    json_free(p->rows);
    forget_rows(p);
    free(p->repo); free(p->error); free(p->last_click);
    free(p);
}
