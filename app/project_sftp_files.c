#include "project_sftp_internal.h"

static void download(ProjectSftp *p, SftpSession *s, const char *path);
static void make_folder(ProjectSftp *p, SftpSession *s, const char *folder);
static void rename_node(ProjectSftp *p, SftpSession *s, const char *path);
static void delete_node(SftpSession *s, const char *path);
static void copy_text(HWND hwnd, const char *text);
static SftpSession *session_by_index(intptr_t arg);
static const char *row_path(ProjectSftp *p, intptr_t arg);
static void click_row(ProjectSftp *p, SftpSession *s, const char *path);

static void download(ProjectSftp *p, SftpSession *s, const char *path) {
    SftpNode *n = sftp_node(s, path);
    if (!n || str_eq(path, "/")) return;
    wchar_t *wname = utf8_to_wide(n->name);
    if (n->dir || (n->link && n->kid_count)) {
        char *title = xstrfmt("Download %s into", n->name);
        wchar_t *into = project_sftp_pick_folder(project_sftp_owner(p), title);
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
        ofn.lStructSize = sizeof ofn; ofn.hwndOwner = project_sftp_owner(p);
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
    char *name = dialog_text(project_sftp_owner(p), "New folder", label, "Create", "");
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
    char *name = dialog_text(project_sftp_owner(p), "Rename", "New name", "Rename", sftp_basename(path));
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
    SftpSession *s = project_sftp_shown_session(p);
    switch (action - p->base) {
    case A_SERVER: project_sftp_connect_row(p, json_at(p->rows, (size_t)arg)); break;
    case A_TAB: sftp_set_active(session_by_index(arg)); break;
    case A_TAB_CLOSE: project_sftp_close_session(session_by_index(arg)); break;
    case A_CLOSE: project_sftp_close_session(s); break;
    case A_RECONNECT: {
        char *error = NULL;
        if (s && !sftp_reconnect(s, &error)) app_alert("Could not reconnect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    case A_ROW: { const char *path = row_path(p, arg); if (s && path) click_row(p, s, path); break; }
    case A_TWISTY: { const char *path = row_path(p, arg); if (s && path) { char *keep = xstrdup(path); sftp_toggle(s, keep); free(keep); } break; }
    case A_UPLOAD: if (s && sftp_state(s) == SFTP_READY) { char *folder = project_sftp_target_folder(s); project_sftp_upload_files(p, s, folder); free(folder); } break;
    case A_DOWNLOAD: if (s && sftp_state(s) == SFTP_READY && sftp_selected(s)) { char *path = xstrdup(sftp_selected(s)); download(p, s, path); free(path); } break;
    case A_MKDIR: if (s && sftp_state(s) == SFTP_READY) { char *folder = project_sftp_target_folder(s); make_folder(p, s, folder); free(folder); } break;
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
    project_sftp_relayout(p);
    return true;
}

bool project_sftp_context(ProjectSftp *p, int action, intptr_t arg, POINT pt) {
    if (action != p->base + A_ROW) return action >= p->base && action < p->base + PROJECT_SFTP_ACTIONS;
    SftpSession *s = project_sftp_shown_session(p);
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
    project_sftp_relayout(p);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, project_sftp_owner(p), NULL);
    DestroyMenu(menu);
    // The menu ran a message loop: the session may have closed meanwhile.
    s = project_sftp_shown_session(p);
    if (!s || !sftp_node(s, path)) chosen = chosen == MENU_COPY_PATH ? chosen : 0;
    switch (chosen) {
    case MENU_DOWNLOAD: download(p, s, path); break;
    case MENU_UPLOAD: project_sftp_upload_files(p, s, path); break;
    case MENU_UPLOAD_FOLDER: project_sftp_upload_folder(p, s, path); break;
    case MENU_MKDIR: make_folder(p, s, path); break;
    case MENU_REFRESH: sftp_list(s, path); break;
    case MENU_RENAME: rename_node(p, s, path); break;
    case MENU_DELETE: delete_node(s, path); break;
    case MENU_COPY_PATH: copy_text(project_sftp_owner(p), path); break;
    }
    free(path);
    project_sftp_relayout(p);
    return true;
}
