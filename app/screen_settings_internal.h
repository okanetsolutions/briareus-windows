#ifndef BRIAREUS_SCREEN_SETTINGS_INTERNAL_H
#define BRIAREUS_SCREEN_SETTINGS_INTERNAL_H
#include "project_settings.h"
#include "meeting.h"
#include "mcp.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - The sidebar

enum { ACT_BACK = 1000, ACT_NEW_PROJECT, ACT_OPEN_PROJECT, ACT_NEW_PROVIDER, ACT_OPEN_PROVIDER, ACT_NEW_SERVER, ACT_OPEN_SERVER, ACT_NEW_SSH,
       ACT_OPEN_SSH, ACT_NEW_FORGE, ACT_OPEN_FORGE, ACT_OPEN_MEETING, ACT_NEW_MCP, ACT_OPEN_MCP, ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_NEW_MAIL, ACT_OPEN_MAIL, ACT_OPEN_HOME };

enum { MENU_UP = 1, MENU_DOWN };

typedef struct {
    Screen base;
    Json *projects;     // the server's Project rows, in their order
    Json *defaults;     // what a new one starts from
    bool loaded;
    char *error;
    Request *req, *req_order;
    Json *providers;    // the server's Provider rows (`list`) and what a new one starts from (`defaults`)
    bool providers_loaded, open_first_provider;   // the second: open the first row once the list is read (after a delete)
    char *providers_error;
    Request *req_providers;
    Json *servers;      // the database pool: the server's DbServer rows (`list`) and what a new one starts from (`defaults`)
    bool servers_loaded, open_first_server;   // the second: open the first row once the list is read (after a delete)
    char *servers_error;
    Request *req_servers;
    Json *ssh;          // the server's SshServer rows as `list`, and `defaults`
    bool ssh_loaded;
    char *ssh_error;
    Request *req_ssh;
    Json *forge;        // the server's ForgeAccount rows as `list`, and `defaults`
    bool forge_loaded;
    char *forge_error;
    Request *req_forge;
    Json *mcp; Request *req_mcp; ApiClient *mcp_account; char *mcp_error; bool mcp_loaded;
    Json *mail; Request *req_mail; ApiClient *mail_client; char *mail_error; bool mail_loaded;
    RECT signout_rc;
} SettingsScreen;

extern SettingsScreen *screen_settings_g_settings;
typedef struct { char *label, *repo; bool enabled, db, selected, movable; } ProjectRowData;

typedef struct { char *label, *binary; bool active, login, endpoint, selected, unsaved; } ProviderRowData;

// MARK: - The project form

typedef enum { K_TEXT, K_LIST, K_AREA, K_NUMBER, K_BOOL, K_BOARD } FieldKind;

// K_BOARD: the Projects board, edited as its address
/// One of the form's fields: the Project key it edits, how, and the words around it. `rows` sizes a multi-line box.
typedef struct { const char *key; FieldKind kind; const char *label, *cue, *hint; int rows; bool mono; } FieldDef;

enum {
    F_REPO, F_LABEL, F_ENABLED, F_MAIL_TOOLS, F_LOCAL_DIR, F_BOARD,
    F_SETUP, F_PHP,
    F_DB_NAME, F_DB_EXT, F_DB_POOL, F_DB_RESTORE,
    F_REVIEW_AUTHOR, F_PUBLISH, F_AUTO_LOOP, F_BABYSIT, F_TEST_SHEET, F_TEST_RUN, F_QA_NOTES, F_SHEET_STEPS, F_FEEDBACK_STEPS,
    F_BUDGET, F_IS_SELF,
    F_ENV,
    F_RUN, F_PROFILES,
    F_COUNT
};

extern const FieldDef screen_settings_FIELDS[F_COUNT];
/// The provider, model and effort pickers: the code review's, each errand step's, and the orchestrator's workers'.
/// A flat row edits three keys of the project; a step edits its entry in `stepRuntimes`.
enum { R_REVIEW, R_TEST_SHEET, R_TEST_RUN, R_WORKER, R_COUNT };

typedef struct { const char *provider_key, *model_key, *effort_key, *step, *none, *provider_label; } RuntimeDef;

extern const RuntimeDef screen_settings_RUNTIMES[R_COUNT];
typedef struct { int provider_id; char *model, *effort; } RuntimePick;

// provider 0: the row's `none`

/// The dashboard's sections, as tabs along the top of the form, the way the pull request page lays out its own.
enum { T_PROJECT, T_DATABASE, T_REVIEW, T_ORCHESTRATOR, T_ENV, T_RUN, T_COUNT };

extern int screen_settings_g_tab;
enum { ACT_SAVE = 1100, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_TOGGLE, ACT_PICK, ACT_FOCUS };

enum { ID_FIELD = 2000 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the project is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];   // each edit's place in content coordinates, from the last layout
    bool laid[F_COUNT];    // laid out in the last layout: its tab is open
    bool clipped[F_COUNT];
    int lines[F_COUNT];    // a long box's lines, which it grows to show, as the dashboard's textareas do
    bool bools[F_COUNT];
    ProjectBabysit babysit;
    RuntimePick picks[R_COUNT];
    bool has_catalog; RuntimeCatalog catalog;
    Request *req_save, *req_delete, *req_runtimes;
    bool dirty, filling, shown;
    bool tab_dot;          // the open tab showed its unsaved-changes dot at the last layout
    int focused, focus_first;   // the field with the focus, and one to focus once the controls exist; -1 for none
    char *error;
} FormScreen;

/// A long box grows to this many lines, and scrolls inside past it.
enum { MAX_ROWS = 40 };

typedef struct { char *text; bool enabled; } SelectData;

// MARK: - The SSH server form

/// An SSH server's boxes, as the web's form has them, each on one of the tabs below. The database login under them is
/// write-only on the server: the row says only whether one is stored (`hasDbCredentials`), so the username is read back
/// through `GET …/db-credentials` and the password box stays empty, sent only when something is typed in it.
enum { S_LABEL, S_HOST, S_PORT, S_USER, S_KEY, S_DBHOST, S_DBPORT, S_DBUSER, S_DBPASS, S_COUNT };

extern const FieldDef screen_settings_SSH_FIELDS[S_COUNT];
/// The two pickers: the project whose sessions may use the server, and whether its commands wait for approval.
enum { SP_REPO, SP_MODE, SP_COUNT };

/// The form's cells, two to a row: what the server is, how it is reached, and what it may run unasked.
enum { C_LABEL, C_REPO, C_HOST, C_PORT, C_USER, C_KEY, C_MODE, C_ENABLED, C_DBHOST, C_DBPORT, C_DBUSER, C_DBPASS };

/// The form's tabs: the server itself, and the database login on it. The open one stays open from one server to the next.
enum { ST_SERVER, ST_DATABASE, ST_COUNT };

extern int screen_settings_g_ssh_tab;
enum { ACT_SSH_SAVE = 1200, ACT_SSH_CLONE, ACT_SSH_DELETE, ACT_SSH_TOGGLE, ACT_SSH_PICK, ACT_SSH_FOCUS, ACT_SSH_TAB };

enum { ID_SSH_FIELD = 2100 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    double id;             // 0 until the server is saved
    HWND edits[S_COUNT];
    RECT rects[S_COUNT];
    bool laid[S_COUNT], clipped[S_COUNT];
    bool enabled;          // Available to sessions on this project
    char *picks[SP_COUNT];
    Request *req_save, *req_delete, *req_login;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
    int error_tab;         // the tab whose field the last refused save was about
    char *db_user;         // the stored database username, as read back; NULL until known
    char *login_error;     // why it could not be read back
} SshForm;

// MARK: - The Forge account form

/// A Forge account's boxes. The token is write-only: the server never sends it back, so its box starts empty and a save
/// with it empty keeps the stored one.
enum { G_LABEL, G_ORG, G_TOKEN, G_COUNT };

enum { ACT_FORGE_SAVE = 1300, ACT_FORGE_DELETE, ACT_FORGE_REPO, ACT_FORGE_FOCUS };

enum { ID_FORGE_FIELD = 2200 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row or the defaults
    double id;             // 0 until the account is saved
    HWND edits[G_COUNT];
    RECT rects[G_COUNT];
    bool laid[G_COUNT], clipped[G_COUNT];
    Json *repos;           // the projects ticked, as `owner/name`
    Request *req_save, *req_delete;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
} ForgeForm;

int screen_settings_row_id(const Json *row);
char *screen_settings_form_id(int id);
char *screen_settings_ssh_form_id(double id);
double screen_settings_ssh_row_id(const Json *row);
char *screen_settings_forge_form_id(double id);
bool screen_settings_is_form_id(const char *id);
char *screen_settings_settings_unavailable(void);
char *screen_settings_ssh_unavailable(void);
void screen_settings_paint_back(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_settings_project_row_free(void *p);
void screen_settings_paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_settings_provider_row_free(void *p);
void screen_settings_paint_provider_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_settings_settings_open_row(SettingsScreen *s, size_t index);
const Json *screen_settings_settings_rows(SettingsScreen *s);
const Json *screen_settings_provider_rows(SettingsScreen *s);
void screen_settings_settings_open_provider(SettingsScreen *s, size_t index);
void screen_settings_mcp_load(SettingsScreen *s);
void settings_mcp_changed(void);
void screen_settings_mail_load(SettingsScreen *s);
void settings_mail_changed(const Json *list);
void screen_settings_ssh_load(SettingsScreen *s);
void screen_settings_forge_load(SettingsScreen *s);
void screen_settings_projects_load(SettingsScreen *s);
void screen_settings_providers_load(SettingsScreen *s);
void settings_providers_changed(bool open_first);
const Json *screen_settings_server_rows(SettingsScreen *s);
void screen_settings_servers_open_row(SettingsScreen *s, size_t index);
void screen_settings_servers_load(SettingsScreen *s);
void settings_db_servers_changed(bool open_first);
const Json *settings_project_rows(void);
const Json *settings_db_server_rows(void);
size_t settings_pool_capacity(void);
void screen_settings_settings_load(SettingsScreen *s);
void settings_projects_changed(int select_id);
void screen_settings_settings_ssh_changed(void);
const Json *screen_settings_ssh_rows(SettingsScreen *s);
void screen_settings_ssh_open_row(SettingsScreen *s, size_t index);
void screen_settings_settings_forge_changed(void);
const Json *screen_settings_forge_rows(SettingsScreen *s);
void screen_settings_forge_open_row(SettingsScreen *s, size_t index);
void screen_settings_settings_move(SettingsScreen *s, size_t index, int delta);
void screen_settings_section_title(Doc *doc, int w, const char *title, const char *screen_settings_note, int new_action);
void screen_settings_settings_layout(Screen *base, Doc *doc);
void screen_settings_layout_move_arrows(SettingsScreen *s, Doc *doc, int row, size_t i, size_t n, bool selected);
void settings_select_id(const char *id);
Screen *settings_screen_new(void);
int screen_settings_field_tab(int f);
int screen_settings_runtime_tab(int r);
int screen_settings_line_count(const char *text);
bool screen_settings_is_edit(int f);
bool screen_settings_is_multiline(int f);
int screen_settings_shown_rows(FormScreen *s, int f);
bool screen_settings_row_has(FormScreen *s, const char *key);
bool screen_settings_field_offered(FormScreen *s, int f);
bool screen_settings_field_enabled(FormScreen *s, int f);
void screen_settings_pick_clear(RuntimePick *p);
char *screen_settings_edit_text(HWND edit);
void screen_settings_set_edit_text(HWND edit, const char *text);
char *screen_settings_field_text(const Json *row, int f);
void screen_settings_row_pick(FormScreen *s, int r, RuntimePick *out);
void screen_settings_form_fill(FormScreen *s);
Json *screen_settings_form_body(FormScreen *s, char **why, int *tab);
void screen_settings_form_changed(FormScreen *s);
bool screen_settings_tab_changed(FormScreen *s, int t);
void screen_settings_field(FormScreen *s, Doc *doc, int x, int w, int f);
void screen_settings_select_free(void *p);
void screen_settings_paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
void screen_settings_note(Doc *doc, int x, int w, const char *text);
void screen_settings_form_layout(Screen *base, Doc *doc);
void screen_settings_form_header(Screen *base, HeaderInfo *info);
int screen_settings_margin_of(Pane *pane);
void screen_settings_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_settings_form_ensure_controls(FormScreen *s);
void screen_settings_load_runtimes(FormScreen *s);
void screen_settings_append_item(HMENU menu, UINT id, const char *text, bool checked, bool enabled);
void screen_settings_pick(FormScreen *s, int r, int part, POINT pt);
void screen_settings_form_save(FormScreen *s);
Screen *project_settings_screen_new(const Json *row, const Json *defaults);
const char *screen_settings_mode_title(const char *mode);
char *screen_settings_ssh_field_text(const Json *row, int f);
bool screen_settings_ssh_has_login(SshForm *s);
bool screen_settings_ssh_db_offered(void);
void screen_settings_ssh_fill(SshForm *s);
Json *screen_settings_ssh_body(SshForm *s, char **why);
bool screen_settings_ssh_tab_changed(SshForm *s, int tab);
void screen_settings_ssh_changed(SshForm *s);
void screen_settings_ssh_layout(Screen *base, Doc *doc);
void screen_settings_ssh_header(Screen *base, HeaderInfo *info);
void screen_settings_ssh_place(Screen *base, const RECT *content, int scroll_y);
void screen_settings_ssh_ensure_controls(SshForm *s);
void screen_settings_ssh_load_login(SshForm *s);
void screen_settings_ssh_pick(SshForm *s, int p, POINT pt);
void screen_settings_ssh_save(SshForm *s);
Screen *screen_settings_ssh_settings_screen_new(const Json *row, const Json *defaults);
char *screen_settings_forge_field_text(const Json *row, int f);
bool screen_settings_repos_same(const Json *a, const Json *b);
void screen_settings_forge_fill(ForgeForm *s);
Json *screen_settings_forge_body(ForgeForm *s, char **why);
bool screen_settings_forge_form_changed(ForgeForm *s);
void screen_settings_forge_changed(ForgeForm *s);
void screen_settings_forge_layout(Screen *base, Doc *doc);
void screen_settings_forge_header(Screen *base, HeaderInfo *info);
void screen_settings_forge_place(Screen *base, const RECT *content, int scroll_y);
void screen_settings_forge_ensure_controls(ForgeForm *s);
void screen_settings_forge_toggle_repo(ForgeForm *s, size_t index);
void screen_settings_forge_save(ForgeForm *s);
Screen *screen_settings_forge_settings_screen_new(const Json *row, const Json *defaults);

bool screen_settings_field_bool(FormScreen *s, int f);
#endif
