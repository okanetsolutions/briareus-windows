#ifndef BRIAREUS_SCREEN_SETTINGS_GITHUB_INTERNAL_H
#define BRIAREUS_SCREEN_SETTINGS_GITHUB_INTERNAL_H
#include "screen_settings_internal.h"
#include "github_account.h"

enum { GH_LABEL, GH_TOKEN, GH_COUNT };
enum { ACT_GITHUB_SAVE = 1700, ACT_GITHUB_DELETE, ACT_GITHUB_REPO, ACT_GITHUB_FOCUS };
enum { ID_GITHUB_FIELD = 2600 };

typedef struct {
    Screen base;
    Json *row, *repos; // Owned server metadata and draft project assignment; never the token.
    double id;
    HWND edits[GH_COUNT]; // The write-only token lives only in its password edit and outgoing request.
    RECT rects[GH_COUNT];
    bool laid[GH_COUNT], clipped[GH_COUNT];
    Request *req_save, *req_delete;
    ApiClient *client; char *device; // Retained connection and token identity owning this draft.
    bool modal, retired, dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
} GithubForm;

bool screen_settings_github_current(GithubForm *s);
bool screen_settings_github_busy(const GithubForm *s);
void screen_settings_github_controls(GithubForm *s);
char *screen_settings_github_form_id(double id);
char *screen_settings_github_field_text(const Json *row, int f);
void screen_settings_github_fill(GithubForm *s);
Json *screen_settings_github_body(GithubForm *s, char **why);
bool screen_settings_github_form_changed(GithubForm *s);
void screen_settings_github_changed(GithubForm *s);
void screen_settings_github_save(GithubForm *s);
void screen_settings_github_layout(Screen *base, Doc *doc);
void screen_settings_github_header(Screen *base, HeaderInfo *info);
void screen_settings_github_place(Screen *base, const RECT *content, int scroll_y);
void screen_settings_github_ensure_controls(GithubForm *s);
void screen_settings_github_toggle_repo(GithubForm *s, size_t index);
#endif
