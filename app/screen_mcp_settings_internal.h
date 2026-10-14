#ifndef BRIAREUS_SCREEN_MCP_SETTINGS_INTERNAL_H
#define BRIAREUS_SCREEN_MCP_SETTINGS_INTERNAL_H
#include "screens.h"
#include "mcp.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_NAME, F_LABEL, F_URL, F_COMMAND, F_ARGS, F_HEADERS, F_ENV, F_CLIENT_ID, F_CLIENT_SECRET, F_SCOPE, F_CLIENT_NAME, F_CALLBACK, F_COUNT };

enum { ACT_SAVE = 1200, ACT_DELETE, ACT_TAB, ACT_TRANSPORT, ACT_ENABLED, ACT_ALL, ACT_REPO, ACT_SECRET, ACT_REDIRECT, ACT_CONNECT, ACT_SIGN_IN, ACT_BROWSER, ACT_FINISH, ACT_FOCUS, ACT_RELOAD };

enum { ID_FIELD = 2100, TIMER_STATUS = 16 };

typedef struct {
    Screen base;
    ApiClient *account;
    Json *row, *repos;
    double id;
    HWND edits[F_COUNT]; RECT rects[F_COUNT]; bool laid[F_COUNT], clipped[F_COUNT];
    bool shown, filling, dirty, stdio, enabled, all, loopback, uncertain, conflict, create_retry, read_error;
    int tab;
    McpSecretMode secrets[3];
    McpSignIn sign_in;
    Request *read, *write;
    Poller poll;
    char *error, *create_name;
} McpForm;

bool mcp_settings_supported(const char *operation);
bool screen_mcp_settings_account_current(McpForm *s);
bool screen_mcp_settings_busy(McpForm *s);
bool screen_mcp_settings_can_save(McpForm *s);
char *screen_mcp_settings_edit_text(HWND edit);
void screen_mcp_settings_set_text(HWND edit, const char *text);
void screen_mcp_settings_fill(McpForm *s);
void screen_mcp_settings_changed(McpForm *s);
void screen_mcp_settings_repaint(McpForm *s);
void screen_mcp_settings_error_text(McpForm *s, const char *error);
Json *screen_mcp_settings_body_now(McpForm *s, char **error);
bool screen_mcp_settings_selected(McpForm *s, const char *repo);
void screen_mcp_settings_form_layout(Screen *base, Doc *doc);
void screen_mcp_settings_form_header(Screen *base, HeaderInfo *info);
void screen_mcp_settings_form_place(Screen *base, const RECT *content, int scroll_y);
void screen_mcp_settings_ensure_controls(McpForm *s);
void screen_mcp_settings_form_save(McpForm *s);
Screen *mcp_settings_screen_new(const Json *row, const Json *defaults);

#endif
