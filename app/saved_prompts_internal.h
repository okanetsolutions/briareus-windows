#ifndef BRIAREUS_SAVED_PROMPTS_INTERNAL_H
#define BRIAREUS_SAVED_PROMPTS_INTERNAL_H
#include "saved_prompts.h"
#include "saved_prompt.h"
#include "dialogs.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
enum { PICK_SAVE = 1, PICK_EDIT, PICK_DELETE, PICK_RETRY, PICK_RESUME, PICK_ROW = 100 };
struct PromptPicker {
    Screen *owner; HWND window; ApiClient *client; Json *device;
    char *repo, *identity, *key; void *ctx;
    PromptOwnerCurrent current; PromptDraft draft; PromptInsert insert;
    Request *read, *write;
    Json *rows, *original;
    char *error, *notice, *title, *body;
    int editing; // -1 no retained dialog; 0 create; positive server prompt id
    bool everywhere, shown, modal, retired, loaded;
    unsigned generation, visibility_generation; double epoch;
};
bool prompt_picker_current(PromptPicker *p);
bool prompt_picker_writable(PromptPicker *p, const char *operation);
const Json *prompt_picker_state(PromptPicker *p);
void prompt_picker_read(PromptPicker *p);
void prompt_picker_write(PromptPicker *p, const char *operation, Json *args);
void prompt_picker_release(PromptPicker *p);
void prompt_picker_changed(PromptPicker *p);
void prompt_picker_clear_edit(PromptPicker *p);
// Action dispatch is shared by the menu and linked regressions; rows are cloned before modal loops.
void prompt_picker_insert_row(PromptPicker *p, const Json *row);
void prompt_picker_edit(PromptPicker *p, const Json *row);
void prompt_picker_save(PromptPicker *p);
void prompt_picker_delete(PromptPicker *p, const Json *row);
// Dispatch a popup result against the snapshots captured when it opened.
void prompt_picker_choose(PromptPicker *p, int chosen, const Json *rows, unsigned generation, unsigned visibility, POINT pt);
#endif
