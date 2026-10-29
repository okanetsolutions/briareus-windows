#include "project_sftp_internal.h"

static bool exists_in(SftpSession *s, const char *folder, const char *name);
static const wchar_t *wide_base(const wchar_t *path);
static void upload_paths(SftpSession *s, const char *folder, wchar_t **paths, size_t n);
static LRESULT CALLBACK drop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// MARK: - Files

/// The folder new files go to: the selected folder, the selected file's folder, else the folder the server started in.
char *project_sftp_target_folder(SftpSession *s) {
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

void project_sftp_upload_files(ProjectSftp *p, SftpSession *s, const char *folder) {
    enum { CHARS = 32768 };
    wchar_t *buf = xcalloc(CHARS, sizeof *buf);
    char *title = xstrfmt("Upload to %s", folder);
    wchar_t *wtitle = utf8_to_wide(title);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = project_sftp_owner(p);
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = CHARS;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    app_modal_begin(ofn.hwndOwner);
    bool ok = GetOpenFileNameW(&ofn);
    app_modal_end();
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
wchar_t *project_sftp_pick_folder(HWND parent, const char *title) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->lpVtbl->GetOptions(dialog, &options);
    dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    wchar_t *wtitle = utf8_to_wide(title);
    dialog->lpVtbl->SetTitle(dialog, wtitle);
    free(wtitle);
    wchar_t *out = NULL;
    app_modal_begin(parent);
    HRESULT shown = dialog->lpVtbl->Show(dialog, parent);
    app_modal_end();
    if (SUCCEEDED(shown)) {
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

void project_sftp_upload_folder(ProjectSftp *p, SftpSession *s, const char *folder) {
    char *title = xstrfmt("Upload a folder to %s", folder);
    wchar_t *local = project_sftp_pick_folder(project_sftp_owner(p), title);
    free(title);
    if (!local) return;
    upload_paths(s, folder, &local, 1);
    free(local);
}

// MARK: - Dropping files from Explorer

static LRESULT CALLBACK drop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ProjectSftp *p = (ProjectSftp *)ref;
    if (msg == WM_DROPFILES) {
        HDROP drop = (HDROP)wp;
        SftpSession *s = project_sftp_shown_session(p);
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
        if (!folder) folder = project_sftp_target_folder(s);
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

void project_sftp_set_dropping(ProjectSftp *p, bool on) {
    if (on == p->dropping || !p->host->pane) return;
    HWND hwnd = pane_hwnd(p->host->pane);
    p->dropping = on;
    if (on) { SetWindowSubclass(hwnd, drop_proc, (UINT_PTR)p, (DWORD_PTR)p); DragAcceptFiles(hwnd, TRUE); }
    else { DragAcceptFiles(hwnd, FALSE); RemoveWindowSubclass(hwnd, drop_proc, (UINT_PTR)p); }
}
