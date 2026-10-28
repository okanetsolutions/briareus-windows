#ifndef BRIAREUS_DIALOGS_INPUT_INTERNAL_H
#define BRIAREUS_DIALOGS_INPUT_INTERNAL_H
#include "saved_prompts.h"
#include "voice.h"
typedef struct { HWND dialog; VoiceNote *note; int text_id; } DialogVoice;
typedef struct {
    const BoardAction *action; int number; char *input; HWND dialog;
    DialogVoice voice; PromptPicker *prompts;
    Screen *source; Pane *pane; HWND source_window;
    char *repo; bool closed;
} InputState;
// Shared transcript delivery preserves text authored while transcription was pending.
void dialog_voice_append(DialogVoice *v, const char *text);
#endif
