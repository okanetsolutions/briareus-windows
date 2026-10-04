// Modal dialogs: new conversation, rename, connection, and an errand's input.
#ifndef BRIAREUS_DIALOGS_H
#define BRIAREUS_DIALOGS_H
#include "board.h"
#include "models.h"
#include <windows.h>
#include <stdbool.h>

/// Starts a conversation; true with the new session when one began.
bool dialog_new_conversation(HWND owner, const Project *project, Session *started);
/// A new title, or NULL when cancelled.
char *dialog_rename(HWND owner, const char *current);
/// One line of text under a caption and a label, with `ok_label` on its button; NULL when cancelled.
char *dialog_text(HWND owner, const char *caption, const char *label, const char *ok_label, const char *current);
/// A password or passphrase, typed hidden under `label`; NULL when cancelled.
char *dialog_password(HWND owner, const char *caption, const char *label);
/// What an errand needs to be told; true with the text (possibly empty when optional) when started.
bool dialog_action_input(HWND owner, const BoardAction *action, int number, char **input);
/// The meeting assistant's system prompt and first message, written before joining a meeting about `project`, from
/// `*prompt` and `*first_message`; Reset puts the defaults back. True with the texts on Join.
bool dialog_meeting_prompt(HWND owner, const char *project, const char *default_prompt, const char *default_first,
                           char **prompt, char **first_message);
/// Applies the theme to a dialog and its controls.
void dialog_theme(HWND dialog);
/// Paints dialog backgrounds and static text in the theme; call from WM_CTLCOLOR* handlers.
LRESULT dialog_ctl_color(HWND dialog, UINT msg, WPARAM wp, LPARAM lp);

#endif
