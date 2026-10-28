#ifndef BRIAREUS_SAVED_PROMPTS_H
#define BRIAREUS_SAVED_PROMPTS_H
#include "screens.h"
typedef struct PromptPicker PromptPicker;
typedef bool (*PromptOwnerCurrent)(void *ctx, const char *repo, const char *identity);
typedef char *(*PromptDraft)(void *ctx); // owned UTF-8 current draft
// Insert callback changes only the draft; it must never send a message.
typedef void (*PromptInsert)(void *ctx, const char *body);
PromptPicker *prompt_picker_new(Screen *owner, const char *repo, const char *identity,
    PromptOwnerCurrent current, PromptDraft draft, PromptInsert insert, void *ctx);
void prompt_picker_free(PromptPicker *p);
void prompt_picker_visible(PromptPicker *p, bool shown);
bool prompt_picker_offered(PromptPicker *p);
void prompt_picker_open(PromptPicker *p, POINT screen_point);
#endif
