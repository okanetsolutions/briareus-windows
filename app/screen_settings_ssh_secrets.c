#include "screen_settings_internal.h"

bool screen_settings_ssh_current(SshForm *s) {
    if (s->account != g_store.client || !store_can_manage()) return false;
    return str_eq(s->device, g_store.device.id);
}

void screen_settings_ssh_secret_controls(SshForm *s) {
    s->filling = true;
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        HWND edit = s->edits[S_PASSWORD + f];
        if (!edit) continue;
        const SshSecret *v = &s->secrets.fields[f];
        screen_settings_ssh_set_secret(edit, v->text ? v->text : "");
    }
    s->filling = false;
    screen_settings_ssh_secret_cues(s);
}

char *screen_settings_ssh_secret_text(HWND edit) {
    int length = GetWindowTextLengthW(edit);
    wchar_t *w = xcalloc((size_t)length + 1, sizeof *w);
    GetWindowTextW(edit, w, length + 1);
    char *text = wide_to_utf8(w);
    ssh_secret_free((char *)w, ((size_t)length + 1) * sizeof *w);
    return text;
}
void screen_settings_ssh_set_secret(HWND edit, const char *text) {
    wchar_t *w = utf8_to_wide(text);
    SetWindowTextW(edit, w);
    ssh_secret_free((char *)w, (wcslen(w) + 1) * sizeof *w);
}

void screen_settings_ssh_secret_cues(SshForm *s) {
    for (int f = 0; f < SSH_SECRET_COUNT; f++) {
        HWND edit = s->edits[S_PASSWORD + f];
        if (!edit) continue;
        const SshSecret *v = &s->secrets.fields[f];
        const char *cue = v->remove ? "Removed on save" : v->stored ? "Stored; type a new one to replace it" : screen_settings_SSH_FIELDS[S_PASSWORD + f].cue;
        wchar_t *w = utf8_to_wide(cue);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
    }
}
