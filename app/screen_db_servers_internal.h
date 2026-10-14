#ifndef BRIAREUS_SCREEN_DB_SERVERS_INTERNAL_H
#define BRIAREUS_SCREEN_DB_SERVERS_INTERNAL_H
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - The fields

enum { F_LABEL, F_HOST, F_PORT, F_USERNAME, F_PASSWORD, F_COUNT };

/// One of the form's boxes: the DbServer key it edits and the words around it.
typedef struct { const char *key, *label, *cue; bool number, secret, mono; const char *hint; } FieldDef;

extern const FieldDef screen_db_servers_FIELDS[F_COUNT];
extern const char *const screen_db_servers_ENABLED_KEY;
/// Every field is on Server; Pool only reads.
enum { T_SERVER, T_POOL, T_COUNT };

extern int screen_db_servers_g_tab;
enum { ACT_SAVE = 1200, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_TOGGLE, ACT_FOCUS, ACT_TEST };

enum { ID_FIELD = 2100 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the server is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];   // each edit's place in content coordinates, from the last layout
    bool laid[F_COUNT], clipped[F_COUNT];
    bool enabled;          // In the pool
    Request *req_save, *req_delete, *req_test;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
    char *test_text; bool test_ok;   // what the last Test connection said, green when the server answered
} DbForm;

bool screen_db_servers_row_has(DbForm *s, const char *key);
void screen_db_servers_form_fill(DbForm *s);
Json *screen_db_servers_form_body(DbForm *s, char **why);
bool screen_db_servers_tab_changed(DbForm *s, int t);
void screen_db_servers_form_changed(DbForm *s);
void screen_db_servers_form_layout(Screen *base, Doc *doc);
void screen_db_servers_form_header(Screen *base, HeaderInfo *info);
void screen_db_servers_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_db_servers_form_ensure_controls(DbForm *s);
void screen_db_servers_form_save(DbForm *s);
Screen *db_server_settings_screen_new(const Json *row, const Json *defaults);

#endif
