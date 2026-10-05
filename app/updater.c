#include "updater.h"
#include "credentials.h"
#include "resource.h"
#include "screens.h"
#include "str.h"
#include "theme.h"
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FIRST_CHECK_MS (15 * 1000)
#define CHECK_EVERY_MS (6 * 60 * 60 * 1000)

enum { ITEM_RESTART = 1, ITEM_INSTALL, ITEM_CHECK, ITEM_AUTO, ITEM_NOTES };

static struct {
    wchar_t exe[MAX_PATH];      // as launched, which names the new build once an update moved the running file aside
    bool busy, installing;
    UpdateRelease latest; bool has_latest, newer;
    bool ready;                 // the latest release is in place and runs from the next start
    char *error;                // what the last check or install ran into
    char *failed_version;       // a release that could not be installed is not downloaded again on its own
    time_t checked;
    UINT_PTR timer;
    bool restart;
    char label[48];
} U;

typedef struct {
    bool install, manual;
    wchar_t exe[MAX_PATH];
    UpdateRelease release; bool ok, newer, installed;
    char *error;
} Job;

static void schedule(UINT ms);
static void start_job(bool install, bool manual);

static void repaint(void) { Pane *sidebar = app_sidebar_pane(); if (sidebar) pane_footer_changed(sidebar); }

static void restart(void) {
    U.restart = true;
    PostMessageW(app_window(), WM_CLOSE, 0, 0);
}
void updater_restart_cancelled(void) { U.restart = false; }

void updater_relaunch_if_asked(void) {
    if (!U.restart || !U.exe[0]) return;
    wchar_t command[MAX_PATH + 3];
    swprintf(command, MAX_PATH + 3, L"\"%ls\"", U.exe);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (CreateProcessW(U.exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

// MARK: - Checking

static void job_work(void *p) {
    Job *j = p;
    if (!update_check(&j->release, &j->error)) return;
    j->ok = true;
    j->newer = update_newer(j->release.version, APP_VERSION_STRING);
    if (!j->newer || !j->install) return;
    char *bytes; size_t len;
    if (!update_download(&j->release, &bytes, &len, &j->error)) return;
    j->installed = update_install(j->exe, bytes, len, &j->error);
    free(bytes);
}

/// What a check asked for from the menu ended in, said in a dialog.
static void report(const Job *j) {
    if (j->installed) {
        char *title = xstrfmt("Briareus %s is installed", U.latest.version);
        bool now = app_confirm(title, "Restart Briareus now to use it? Otherwise it starts the next time you open Briareus.", "Restart now", false);
        free(title);
        if (now) restart();
    } else if (U.error) {
        app_alert(j->install && j->newer ? "Briareus could not be updated" : "Could not check for updates", U.error);
    } else if (!j->newer) {
        char *message = xstrfmt("Version %s is the latest release.", APP_VERSION_STRING);
        app_alert("Briareus is up to date", message);
        free(message);
    } else {
        char *title = xstrfmt("Briareus %s is available", U.latest.version);
        bool install = app_confirm(title, "Download and install it now? It runs once Briareus restarts.", "Install", false);
        free(title);
        if (install) start_job(true, true);
    }
}

static void job_done(void *p) {
    Job *j = p;
    U.busy = U.installing = false;
    U.checked = time(NULL);
    free(U.error); U.error = j->error; j->error = NULL;
    if (j->ok) {
        update_release_free(&U.latest);
        U.latest = j->release; memset(&j->release, 0, sizeof j->release);
        U.has_latest = true; U.newer = j->newer;
    }
    if (j->installed) U.ready = true;
    else if (j->install && j->newer && U.error) { free(U.failed_version); U.failed_version = xstrdup(U.latest.version); }
    repaint();
    // Once an update is in place the running file is the backup, which another one could not replace: no more checks.
    if (!U.ready) schedule(CHECK_EVERY_MS);
    if (j->manual) report(j);
    update_release_free(&j->release); free(j->error); free(j);
}

/// Checks, and installs a newer release when asked to or when automatic updates are on.
static void start_job(bool install, bool manual) {
    if (U.busy || U.ready) return;
    Job *j = xcalloc(1, sizeof *j);
    // An automatic install that failed waits for the next release, or for Install in the menu.
    bool automatic = settings_read_auto_update() && !(U.failed_version && U.has_latest && str_eq(U.failed_version, U.latest.version));
    j->install = install || automatic;
    j->manual = manual;
    wcscpy(j->exe, U.exe);
    U.busy = true; U.installing = install;
    repaint();
    async_run(job_work, job_done, j);
}

static void CALLBACK timer_fired(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    KillTimer(NULL, id);
    if (id == U.timer) U.timer = 0;
    start_job(false, false);
}
static void schedule(UINT ms) {
    if (U.timer) KillTimer(NULL, U.timer);
    U.timer = SetTimer(NULL, 0, ms, timer_fired);
}

/// The backup the last update left: the process that ran from it may still be closing, so it is tried for a while.
static void cleanup_work(void *p) {
    const wchar_t *exe = p;
    for (int i = 0; i < 20 && !update_cleanup(exe); i++) Sleep(500);
}

void updater_start(void) {
    DWORD n = GetModuleFileNameW(NULL, U.exe, MAX_PATH);
    if (!n || n >= MAX_PATH) { U.exe[0] = 0; return; }
    async_run(cleanup_work, NULL, U.exe);
    schedule(FIRST_CHECK_MS);
}

// MARK: - Sidebar

const char *updater_label(bool *highlight) {
    *highlight = U.has_latest && (U.ready || U.newer);
    if (*highlight) snprintf(U.label, sizeof U.label, "\xE2\x86\x91 v%s", U.latest.version);
    else snprintf(U.label, sizeof U.label, "v%s", APP_VERSION_STRING);
    return U.label;
}

static void append_item(HMENU menu, UINT flags, UINT_PTR id, const char *text) { wchar_t *w = utf8_to_wide(text); AppendMenuW(menu, flags, id, w); free(w); }

static char *status_line(void) {
    if (U.busy) return xstrdup(U.installing ? "Updating Briareus\xE2\x80\xA6" : "Checking for updates\xE2\x80\xA6");
    if (U.ready) return xstrfmt("Briareus %s is installed: restart to use it", U.latest.version);
    if (U.error) {
        // A long reason is cut on a character boundary, the menu being one line.
        size_t cut = strlen(U.error);
        if (cut <= 90) return xstrdup(U.error);
        cut = 87;
        while (cut && ((unsigned char)U.error[cut] & 0xC0) == 0x80) cut--;
        return xstrfmt("%.*s\xE2\x80\xA6", (int)cut, U.error);
    }
    if (U.newer) return xstrfmt("Briareus %s is available", U.latest.version);
    if (U.checked) return xstrfmt("Briareus %s is up to date", APP_VERSION_STRING);
    return xstrfmt("Briareus %s", APP_VERSION_STRING);
}

void updater_menu(Pane *pane, POINT pt) {
    HMENU menu = CreatePopupMenu();
    char *status = status_line();
    append_item(menu, MF_STRING | MF_GRAYED, 0, status);
    free(status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (U.ready) append_item(menu, MF_STRING, ITEM_RESTART, "Restart to update");
    else if (U.newer) {
        char *install = xstrfmt("Install %s", U.latest.version);
        append_item(menu, MF_STRING | (U.busy ? MF_GRAYED : 0), ITEM_INSTALL, install);
        free(install);
    }
    append_item(menu, MF_STRING | (U.busy || U.ready || !U.exe[0] ? MF_GRAYED : 0), ITEM_CHECK, "Check for updates now");
    bool automatic = settings_read_auto_update();
    append_item(menu, MF_STRING | (automatic ? MF_CHECKED : 0), ITEM_AUTO, "Install updates automatically");
    if (U.has_latest && U.latest.page) append_item(menu, MF_STRING, ITEM_NOTES, "Release notes\xE2\x80\xA6");
    HWND owner = pane_hwnd(pane);
    ClientToScreen(owner, &pt);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, NULL);
    DestroyMenu(menu);
    switch (chosen) {
    case ITEM_RESTART: restart(); break;
    case ITEM_INSTALL: start_job(true, true); break;
    case ITEM_CHECK: start_job(false, true); break;
    case ITEM_AUTO:
        settings_write_auto_update(!automatic);
        // Turned on with a release already waiting: it is fetched now rather than at the next check.
        if (!automatic && U.newer) start_job(true, false);
        break;
    case ITEM_NOTES: open_web_url(U.latest.page); break;
    }
}
