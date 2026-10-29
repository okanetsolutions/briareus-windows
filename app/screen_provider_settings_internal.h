#ifndef BRIAREUS_SCREEN_PROVIDER_SETTINGS_INTERNAL_H
#define BRIAREUS_SCREEN_PROVIDER_SETTINGS_INTERNAL_H
#include "screens.h"
#include "str.h"
#include "provider_status.h"
#include "provider_settings.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { BINARY_LOGIN, BINARY_CHOICE, BINARY_KEY } ProviderBinaryAuth;
typedef struct { const char *id, *title; ProviderBinaryAuth auth; const char *note; } screen_provider_settings_BINARIES_type;
extern const screen_provider_settings_BINARIES_type screen_provider_settings_BINARIES[6];
enum { BINARY_COUNT = sizeof screen_provider_settings_BINARIES / sizeof *screen_provider_settings_BINARIES };

typedef enum { K_TEXT, K_LIST } FieldKind;

/// One of the form's boxes: the Provider key it edits (NULL for the login code, which is not part of the row).
typedef struct { const char *key; FieldKind kind; const char *label, *cue; int rows; bool mono, secret; } FieldDef;

enum { F_LABEL, F_BASE_URL, F_API_KEY, F_MODELS, F_EFFORTS, F_DEFAULT_MODEL, F_DEFAULT_EFFORT, F_LOGIN_CODE, F_COUNT };

extern const FieldDef screen_provider_settings_FIELDS[F_COUNT];
/// The dashboard's sections as tabs, as the project form has them. Status is a saved provider's alone.
enum { T_PROVIDER, T_MODELS, T_STATUS, T_COUNT };

extern int screen_provider_settings_g_tab;
enum { ACT_SAVE = 1200, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_ACTIVE, ACT_BINARY, ACT_MODE, ACT_FOCUS, ACT_LOGIN, ACT_FINISH, ACT_TEST, ACT_CHECK, ACT_COPY_CODE };

enum { ID_FIELD = 2100 };

enum { TIMER_LOGIN = 7 };

extern const int screen_provider_settings_LOGIN_WAITS_MS[4];
typedef struct {
    Screen base;
    Json *binary_ids;      // owned OpenAPI enum; NULL uses the known registry
    Request *req_binaries;
    bool binaries_loaded;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the provider is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];
    bool laid[F_COUNT], clipped[F_COUNT];
    int lines[F_COUNT];
    bool active, token;    // Active, and the Mode: an API token rather than the CLI's own login
    char *binary;
    Json *status;          // GET /settings/providers/{id}/status's `status`
    char *status_error;
    bool status_fresh;     // the read under way was asked with `fresh`
    char *test_result; bool test_failed;
    Json *login_from;      // owned auth snapshot at the start of the current device login
    char *device_code;     // codex's code to type on its confirm page
    bool code_wanted;      // a claude login waits for the code its authorization page shows
    int login_wait;        // the next of LOGIN_WAITS_MS
    Request *req_save, *req_delete, *req_status, *req_login, *req_test;
    bool modal, retired;   // Destruction during a leave confirmation defers release until it returns.
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
} FormScreen;

enum { MAX_ROWS = 40 };

int screen_provider_settings_line_count(const char *text);
bool screen_provider_settings_is_multiline(int f);
int screen_provider_settings_shown_rows(FormScreen *s, int f);
bool screen_provider_settings_mode_offered(const char *binary);
bool screen_provider_settings_uses_token(FormScreen *s);
bool screen_provider_settings_can_log_in(FormScreen *s);
bool screen_provider_settings_field_shown(FormScreen *s, int f);
const char *screen_provider_settings_binary_title(const char *binary);
const char *screen_provider_settings_base_url_hint(const char *binary);
const char *screen_provider_settings_api_key_hint(const char *binary);
char *screen_provider_settings_edit_text(HWND edit);
void screen_provider_settings_set_edit_text(HWND edit, const char *text);
Json *screen_provider_settings_list_from_text(const char *text);
const char *screen_provider_settings_row_binary(const Json *row);
void screen_provider_settings_form_fill(FormScreen *s);
char *screen_provider_settings_field_now(FormScreen *s, int f);
Json *screen_provider_settings_form_body(FormScreen *s, char **why, int *tab);
void screen_provider_settings_form_changed(FormScreen *s);
bool screen_provider_settings_tab_changed(FormScreen *s, int t);
int screen_provider_settings_open_tab(FormScreen *s);
void screen_provider_settings_layout_tabs(FormScreen *s, Doc *doc, int x, int w);
void screen_provider_settings_note(Doc *doc, int x, int w, const char *text);
void screen_provider_settings_headline(FormScreen *s, Doc *doc, int x, int w);
void screen_provider_settings_layout_status(FormScreen *s, Doc *doc, int x, int w);
void screen_provider_settings_form_layout(Screen *base, Doc *doc);
void screen_provider_settings_form_header(Screen *base, HeaderInfo *info);
void screen_provider_settings_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_provider_settings_form_ensure_controls(FormScreen *s);
void screen_provider_settings_load_binaries(FormScreen *s);
size_t screen_provider_settings_binary_count(FormScreen *s);
const char *screen_provider_settings_binary_id(FormScreen *s, size_t index);
void screen_provider_settings_select_binary(FormScreen *s, const char *binary);
void screen_provider_settings_select_mode(FormScreen *s, bool token);
HMENU screen_provider_settings_binary_menu(FormScreen *s); // Caller destroys the menu.
const char *screen_provider_settings_connection_note(const char *binary);
void screen_provider_settings_pick_binary(FormScreen *s, POINT pt);
void screen_provider_settings_pick_mode(FormScreen *s, POINT pt);
void screen_provider_settings_load_status(FormScreen *s, bool fresh);
void screen_provider_settings_login_start(FormScreen *s);
void screen_provider_settings_cancel_login(FormScreen *s);
void screen_provider_settings_login_finish(FormScreen *s);
void screen_provider_settings_test_endpoint(FormScreen *s);
void screen_provider_settings_form_save(FormScreen *s);
Screen *provider_settings_screen_new(const Json *row, const Json *defaults);

#endif
