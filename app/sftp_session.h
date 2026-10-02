// The SFTP sessions tab's sessions: this PC's OpenSSH sftp client run without a console, fed one command at a time
// through a pipe, its answers read back into a tree of the server's folders. Password and host key prompts come up as
// dialogs of the app's own (ssh asks Briareus.exe, as its SSH_ASKPASS). Sessions outlive the screens that show them, as
// the SSH sessions do, until they are closed or the app quits.
#ifndef BRIAREUS_SFTP_SESSION_H
#define BRIAREUS_SFTP_SESSION_H
#include "terminal.h"
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct SftpSession SftpSession;

/// A file or folder on the server. Folders hold their entries once listed, folders first, then by name.
typedef struct SftpNode {
    char *name, *path;
    bool dir, link;
    long long size;
    char *when;
    bool expanded, loaded, loading;
    char *error;                       // why listing it failed
    struct SftpNode **kids; size_t kid_count;
} SftpNode;

typedef enum { SFTP_CONNECTING, SFTP_READY, SFTP_CLOSED } SftpState;

/// Starts a session and makes it the active one; NULL with `*error` when sftp.exe could not start.
SftpSession *sftp_open(const TermTarget *target, char **error);
/// Ends the session's program and frees it.
void sftp_close(SftpSession *s);
/// Connects a closed session again; its tree is read afresh.
bool sftp_reconnect(SftpSession *s, char **error);

size_t sftp_count(void);
SftpSession *sftp_at(size_t index);
SftpSession *sftp_find(const char *key);
size_t sftp_count_for(const char *key);
/// Sessions connecting or connected, which closing the app ends.
size_t sftp_live_count(void);
SftpSession *sftp_active(void);
void sftp_set_active(SftpSession *s);

const char *sftp_group(const SftpSession *s);
const char *sftp_key(const SftpSession *s);
const char *sftp_label(const SftpSession *s);
/// user@host:port
const char *sftp_target(const SftpSession *s);
SftpState sftp_state(const SftpSession *s);
/// The folder the server started in, once connected.
const char *sftp_home(const SftpSession *s);
/// What the client printed while connecting, when it closed before it was ready (the reason it failed).
const char *sftp_connect_log(const SftpSession *s);

/// "/" and everything listed under it; NULL until connected.
SftpNode *sftp_root(SftpSession *s);
SftpNode *sftp_node(SftpSession *s, const char *path);
const char *sftp_selected(const SftpSession *s);
void sftp_select(SftpSession *s, const char *path);
/// Opens a folder (listing it the first time) or closes it.
void sftp_toggle(SftpSession *s, const char *path);
/// Lists a folder again.
void sftp_list(SftpSession *s, const char *path);
/// Opens every folder from "/" down to `path` and selects it.
void sftp_reveal(SftpSession *s, const char *path);

/// Queues a download of a file or a folder (recursively) to a local path.
void sftp_download(SftpSession *s, const char *remote, const wchar_t *local, bool folder);
/// Queues an upload of a local file or folder (recursively) into a remote folder, under its own name.
void sftp_upload(SftpSession *s, const wchar_t *local, const char *remote_dir, bool folder);
void sftp_mkdir(SftpSession *s, const char *path);
void sftp_rename(SftpSession *s, const char *from, const char *to);
/// Deletes a file, or an empty folder.
void sftp_remove(SftpSession *s, const char *path, bool folder);

/// What runs now (an upload, a download, a change), or NULL; `*queued` is how many wait behind it.
const char *sftp_busy(const SftpSession *s, size_t *queued);
/// The last transfer's or change's outcome, and whether it failed; NULL when there is none.
const char *sftp_status(const SftpSession *s, bool *failed);
/// The local path of the last finished download, for "Show in folder"; NULL when there is none.
const wchar_t *sftp_last_download(const SftpSession *s);

/// Called on the UI thread whenever a session opens, closes, connects, finishes a command or the active one changes.
void sftp_add_listener(void (*fn)(void *ctx), void *ctx);
void sftp_remove_listener(void (*fn)(void *ctx), void *ctx);
/// Ends every session, as the app quits.
void sftp_shutdown(void);

/// Briareus.exe as ssh's SSH_ASKPASS: when the environment says so, asks the prompt on the command line in a dialog,
/// prints the answer for ssh and returns true with the exit code; false when the app should start as usual.
bool sftp_askpass_main(int *exit_code);
/// This process's environment with ssh pointed at Briareus.exe for its prompts, for any ssh the app starts. Freed by the caller.
wchar_t *sftp_child_environment(void);

#endif
