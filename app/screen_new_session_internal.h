#ifndef BRIAREUS_SCREEN_NEW_SESSION_INTERNAL_H
#define BRIAREUS_SCREEN_NEW_SESSION_INTERNAL_H
#include "attach_list.h"
#include "credentials.h"
#include "screens.h"
#include "str.h"
#include "voice.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The dashboard's opening view: "Welcome back", and the composer that starts a session, with its row of chips for the
// project, branch, provider, model, effort and the loops.

enum { ID_COMPOSER = 401 };

enum { TIMER_VOICE = 3 };

enum { CHIP_WORKSPACE, CHIP_PROJECT, CHIP_BRANCH, CHIP_PROVIDER, CHIP_MODEL, CHIP_EFFORT, CHIP_LOOP, CHIP_COUNT };

typedef enum { WORKSPACE_WORKTREE = 1, WORKSPACE_LOCAL, WORKSPACE_ORCHESTRATOR } WorkspaceMode;

typedef struct {
    Screen base;
    Project *projects; size_t count; size_t chosen;
    char *asked;   // the repo of the project the screen was opened for; NULL: none in particular
    bool has_catalog; RuntimeCatalog catalog;
    bool has_runtime; RuntimeChoice runtime;   // the pick; none starts on the project default
    bool hand_picked;   // the pick came from the chips rather than from the last pick or the first available provider
    char **branches; size_t branch_count; char *default_branch; char *branch;   // NULL: a new branch off the default
    WorkspaceMode workspace;
    bool review_loop;
    Request *req_runtimes, *req_branches, *req_start, *req_projects;
    bool busy, uncertain;
    char *error;
    HWND composer; int composer_lines; RECT composer_rc; bool focused;
    VoiceNote *voice;
    AttachList *files;
    // footer hit rects, from the last paint
    RECT chip_rc[CHIP_COUNT]; bool chip_on[CHIP_COUNT];
    RECT mic_rc, send_rc, box_rc, attach_rc;
    int chips_h;
} NewSessionScreen;

const Project *screen_new_session_project(NewSessionScreen *s);
char *screen_new_session_no_branch_label(NewSessionScreen *s);
char *screen_new_session_composer_text(NewSessionScreen *s);
void screen_new_session_set_composer_text(NewSessionScreen *s, const char *text);
bool screen_new_session_composer_empty(NewSessionScreen *s);
bool screen_new_session_can_start(NewSessionScreen *s);
bool screen_new_session_effective_choice(NewSessionScreen *s, RuntimeChoice *out);
void screen_new_session_adopt(NewSessionScreen *s, RuntimeCatalog c);
void screen_new_session_load_choices(NewSessionScreen *s);
void screen_new_session_start(NewSessionScreen *s);
LRESULT CALLBACK screen_new_session_composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
void screen_new_session_voice_changed(void *ctx);
int screen_new_session_new_session_footer_height(Screen *base, int width);
void screen_new_session_new_session_footer_layout(Screen *base, const RECT *rc);
void screen_new_session_new_session_footer_paint(Screen *base, Canvas *cv, const RECT *rc);
void screen_new_session_pick_chip(NewSessionScreen *s, int chip);
void screen_new_session_new_session_footer_click(Screen *base, POINT pt);
void screen_new_session_new_session_command(Screen *base, int id, int code, HWND control);
Screen *new_session_screen_new(const Project *project_, const Project *projects, size_t count);

#endif
