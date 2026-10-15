#ifndef BRIAREUS_PROJECT_SFTP_INTERNAL_H
#define BRIAREUS_PROJECT_SFTP_INTERNAL_H
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

// A project's SFTP sessions tab, beside its SSH sessions: the SSH servers registered for the project in Settings down
// the left, and its open SFTP sessions as tabs over the server's file tree on the right. A click on a server opens an SFTP
// session to it (or returns to the open one); the tree opens at the folder the server starts in, its folders open with a
// click, and files go up (⇧ Upload, or dropped from Explorer onto a folder) and down (⇩ Download, or a double click).
// Sessions run this PC's OpenSSH sftp client (sftp_session.c), so its keys, agent and ~/.ssh/config apply.

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

/// A row of the tree: its twisty, its icon, its name, and a file's size and date.
typedef struct { char *name, *size, *when; int depth; bool folder, open, link, selected, loading, home; } RowData;

bool project_sftp_offered(void);
char *project_sftp_server_key(const Json *row);
char *project_sftp_server_name(const Json *row);
int project_sftp_server_port(const Json *row);
SftpSession *project_sftp_shown_session(ProjectSftp *p);
size_t project_sftp_session_count(const char *repo);
HWND project_sftp_owner(ProjectSftp *p);
void project_sftp_relayout(ProjectSftp *p);
void project_sftp_load(ProjectSftp *p);
void project_sftp_refresh(ProjectSftp *p);
void servers_sftp_changed(void);
void project_sftp_connect_row(ProjectSftp *p, const Json *row);
void project_sftp_close_session(SftpSession *s);
char *project_sftp_target_folder(SftpSession *s);
void project_sftp_upload_files(ProjectSftp *p, SftpSession *s, const char *folder);
wchar_t *project_sftp_pick_folder(HWND parent, const char *title);
void project_sftp_upload_folder(ProjectSftp *p, SftpSession *s, const char *folder);
void project_sftp_set_dropping(ProjectSftp *p, bool on);
void project_sftp_paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void project_sftp_row_free(void *v);
int project_sftp_twisty_left(const RECT *rc, int depth);
void project_sftp_paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void project_sftp_paint_columns(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void project_sftp_layout_list(ProjectSftp *p, Doc *doc, int x, int w);
void project_sftp_layout_tabs(ProjectSftp *p, Doc *doc, int x0, int w);
void project_sftp_forget_rows(ProjectSftp *p);
void project_sftp_layout(ProjectSftp *p, Doc *doc, int w);
void project_sftp_header(ProjectSftp *p, HeaderInfo *info);
void project_sftp_place(ProjectSftp *p, const RECT *content, int scroll_y, bool shown);
bool project_sftp_action(ProjectSftp *p, int action, intptr_t arg, POINT pt);
bool project_sftp_context(ProjectSftp *p, int action, intptr_t arg, POINT pt);
ProjectSftp *project_sftp_new(const char *repo, Screen *host, int action_base);
void project_sftp_free(ProjectSftp *p);

#endif
