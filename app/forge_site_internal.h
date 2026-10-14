#ifndef BRIAREUS_FORGE_SITE_INTERNAL_H
#define BRIAREUS_FORGE_SITE_INTERNAL_H
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One Forge site inside a project's Forge tab, in place of the server's list of sites: its Overview (everything Forge
// says about it, read again), its Deploy script and its Environment (.env), each editable and saved back to Forge through
// the server's proxy (/forge/accounts/{account}/servers/{server}/sites/{site}[/deployment-script|/env]). The .env is a
// production secret: it is read only when asked for, and replacing it is confirmed first.

enum { TAB_OVERVIEW, TAB_SCRIPT, TAB_ENV };

enum { ED_SCRIPT, ED_ENV, ED_COUNT };

// The actions, from the host's `action_base` up.
enum { A_TAB, A_SAVE, A_REVEAL, A_AUTO_SOURCE, A_OPEN_SITE, A_FOCUS };

enum { ID_EDITOR = 2600 };

struct ForgeSite {
    Screen *host;
    int base;
    double account, server_id, site_id;
    Json *server;           // the ForgeServer the site is on
    Json *site;             // the ForgeSite, as listed and then as read again
    int tab;
    bool shown;
    char *site_error;
    Request *req_site;
    // The deploy script and the .env: what was read, whether it was read, the editors over them.
    char *saved[ED_COUNT];
    bool loaded[ED_COUNT];
    bool auto_source, saved_auto_source;
    char *error[ED_COUNT];
    char *notice[ED_COUNT];   // a save's word that it went through
    Request *req_load[ED_COUNT], *req_save[ED_COUNT];
    bool env_wanted;          // the .env was asked for
    HWND edits[ED_COUNT];
    RECT rects[ED_COUNT];
    bool laid[ED_COUNT], clipped[ED_COUNT];
    bool filling;
    bool changed[ED_COUNT];
};

int forge_site_margin_of(Pane *pane);
void forge_site_refresh_view(ForgeSite *v);
char *forge_site_editor_text(HWND e);
void forge_site_fill_editor(ForgeSite *v, int ed);
bool forge_site_editor_changed(ForgeSite *v, int ed);
void forge_site_note_change(ForgeSite *v, int ed);
int forge_site_open_editor(const ForgeSite *v);
void forge_site_save(ForgeSite *v);
void forge_site_layout_editor(ForgeSite *v, Doc *doc, int x, int w, int ed);
bool forge_site_site_url(const ForgeSite *v, char **out);
void forge_site_layout_overview(ForgeSite *v, Doc *doc, int x, int w);
void forge_site_layout_script(ForgeSite *v, Doc *doc, int x, int w);
void forge_site_layout_env(ForgeSite *v, Doc *doc, int x, int w);
void forge_site_layout(ForgeSite *v, Doc *doc, int x, int w);
void forge_site_header(ForgeSite *v, HeaderInfo *info);
void forge_site_place(ForgeSite *v, const RECT *content, int scroll_y, bool shown);
void forge_site_ensure_editors(ForgeSite *v);
void forge_site_refresh(ForgeSite *v);
bool forge_site_action(ForgeSite *v, int action, intptr_t arg);
bool forge_site_command(ForgeSite *v, int id, int code);
bool forge_site_key(ForgeSite *v, WPARAM vk, bool ctrl);
bool forge_site_can_leave(ForgeSite *v);
ForgeSite *forge_site_new(Screen *host, int action_base, double account, const Json *server, const Json *site);
void forge_site_free(ForgeSite *v);

#endif
