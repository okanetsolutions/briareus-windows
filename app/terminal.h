// The SSH sessions tab's sessions: this PC's OpenSSH client (ssh.exe) run in a Windows
// pseudoconsole and drawn by a terminal window of the app's own. Sessions outlive the screens that show them, as
// SecureCRT's tabs do, until they are closed or the app quits.
#ifndef BRIAREUS_TERMINAL_H
#define BRIAREUS_TERMINAL_H
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Term Term;

/// What a session connects to. `key` names the registered server, so a second click finds its open session; `group` is
/// the project it belongs to, whose tab lists it.
typedef struct { const char *key, *group, *label, *user, *host; int port; } TermTarget;

/// Why a target cannot be handed to ssh as it is (an empty or option-like host or user, a port out of range); NULL when
/// it can. The caller frees it.
char *term_target_problem(const TermTarget *target);
/// The command line a session runs, for `client` (the full path of ssh.exe). The caller frees it.
wchar_t *term_command_line(const wchar_t *client, const TermTarget *target);
/// The full path of one of OpenSSH's programs (ssh.exe, sftp.exe): Git for Windows' first, then Windows' OpenSSH Client,
/// then PATH; false when none is installed.
bool term_find_program(const wchar_t *name, wchar_t *out, DWORD n);
/// Starts a session in a terminal window, a hidden child of `parent`, and makes it the active one; NULL with `*error`
/// when the client could not start.
Term *term_open(HWND parent, const TermTarget *target, char **error);
/// Ends the session's program and frees it.
void term_close(Term *t);
/// Starts the program again in the same window, after it ended.
bool term_reconnect(Term *t, char **error);

size_t term_count(void);
Term *term_at(size_t index);
/// The first session open to a server, or NULL.
Term *term_find(const char *key);
/// How many sessions are open to a server.
size_t term_count_for(const char *key);
Term *term_active(void);
void term_set_active(Term *t);

const char *term_group(const Term *t);
const char *term_key(const Term *t);
const char *term_label(const Term *t);
/// user@host:port
const char *term_target(const Term *t);
/// What the session connects to; the strings are the session's own.
void term_get_target(const Term *t, TermTarget *out);
/// The title the remote shell set, or NULL.
const char *term_title(const Term *t);
/// True while the program runs; false once it ended.
bool term_running(const Term *t);
HWND term_hwnd(const Term *t);
/// Shows the window over `rc` (the parent's client coordinates) or hides it.
void term_place(Term *t, const RECT *rc, bool shown);
void term_focus(Term *t);
/// Whether a window is one of the terminals, so app-wide shortcuts leave its keys alone.
bool term_is_window(HWND hwnd);

/// Called on the UI thread whenever a session opens, closes, ends, changes its title or the active one changes.
void term_add_listener(void (*fn)(void *ctx), void *ctx);
void term_remove_listener(void (*fn)(void *ctx), void *ctx);
/// Ends every session, as the app quits.
void term_shutdown(void);

#endif
