#include "screen_conversation_internal.h"
#include "saved_prompt.h"


char *screen_conversation_composer_text(ConversationScreen *s) {
    int n = GetWindowTextLengthW(s->composer);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w); GetWindowTextW(s->composer, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    // Edit controls carry CRLF; the server gets LF.
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}

void screen_conversation_set_composer_text(ConversationScreen *s, const char *text) {
    char *lf = str_replace(text ? text : "", "\r\n", "\n");
    char *crlf = str_replace(lf, "\n", "\r\n"); free(lf);
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(s->composer, w);
    SendMessageW(s->composer, EM_SETSEL, (WPARAM)wcslen(w), (LPARAM)wcslen(w));
    free(w); free(crlf);
    pane_footer_changed(s->base.pane);
}

bool screen_conversation_composer_empty(ConversationScreen *s) { char *t = screen_conversation_composer_text(s); char *trimmed = str_trim(t); bool e = !*trimmed; free(t); free(trimmed); return e; }

void screen_conversation_send_message(ConversationScreen *s) {
    if (s->busy || s->uncertain || screen_conversation_composer_empty(s) || !screen_conversation_can_message(s) || attach_list_uploading(s->files)) return;
    char *text = screen_conversation_composer_text(s);
    Json *extra = json_object(); json_set_str(extra, "text", text);
    Json *ids = attach_list_ids(s->files);
    if (ids) json_object_set(extra, "attachments", ids);
    free(text);
    screen_conversation_mutate(s, "message", extra);
}

LRESULT CALLBACK screen_conversation_composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ConversationScreen *s = (ConversationScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) { screen_conversation_send_message(s); return 0; }
    if (msg == WM_CHAR && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    // Ctrl+V and Shift+Insert both reach the control as WM_PASTE; a file or image becomes an attachment, text pastes as ever.
    if (msg == WM_PASTE && attach_list_paste(s->files)) return 0;
    if (msg == WM_DROPFILES) { attach_list_drop(s->files, (HDROP)wp); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) pane_footer_changed(s->base.pane);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, screen_conversation_composer_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// MARK: - Voice

void screen_conversation_voice_changed(void *ctx) {
    ConversationScreen *s = ctx;
    char *text = voice_take_text(s->voice);
    if (text) {
        if (*text) {
            char *current = screen_conversation_composer_text(s);
            size_t n = strlen(current);
            const char *gap = (!n || current[n - 1] == ' ' || current[n - 1] == '\n' || current[n - 1] == '\t') ? "" : " ";
            char *joined = xstrfmt("%s%s%s", current, gap, text);
            screen_conversation_set_composer_text(s, joined);
            free(joined); free(current);
        }
        free(text);
    }
    char *error = voice_take_error(s->voice);
    if (error) { app_alert("Voice note", error); free(error); }
    if (voice_state(s->voice) == VOICE_RECORDING) SetTimer(pane_hwnd(s->base.pane), TIMER_VOICE, 1000, NULL); else KillTimer(pane_hwnd(s->base.pane), TIMER_VOICE);
    pane_footer_changed(s->base.pane);
}

void screen_conversation_conversation_command(Screen *base, int id, int code, HWND control) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (id == ID_COMPOSER && code == EN_CHANGE) {
        int lines = (int)SendMessageW(control, EM_GETLINECOUNT, 0, 0);
        if (lines != s->composer_lines) { s->composer_lines = lines; pane_footer_changed(base->pane); }
        else { RECT rc; GetClientRect(pane_hwnd(base->pane), &rc); rc.top = rc.bottom - screen_conversation_conversation_footer_height(base, rc.right); InvalidateRect(pane_hwnd(base->pane), &rc, FALSE); }
    }
}

// The picker is bound to this conversation, project and connection. It only changes
// the draft; existing selection is left alone until the user chooses insertion.
bool screen_conversation_can_compose(ConversationScreen *s) {
    if (str_eq(session_status(screen_conversation_session(s)), "closed")) return false;
    return screen_conversation_can_message(s) || prompt_picker_offered(s->prompts);
}
static bool prompts_current(void *ctx, const char *repo, const char *identity) {
    ConversationScreen *s = ctx; const Session *ss = screen_conversation_session(s);
    return IsWindow(s->composer) && str_eq(session_id(ss), identity) && str_eq(session_repo(ss), repo);
}
static char *prompts_draft(void *ctx) { return screen_conversation_composer_text(ctx); }
static void prompts_insert(void *ctx, const char *body) {
    ConversationScreen *s = ctx;
    char *current = screen_conversation_composer_text(s), *joined = saved_prompt_insert(current, body);
    screen_conversation_set_composer_text(s, joined); free(joined); free(current); SetFocus(s->composer);
}
void screen_conversation_prompts_init(ConversationScreen *s) {
    const Session *ss = screen_conversation_session(s);
    s->prompts = prompt_picker_new(&s->base, session_repo(ss), session_id(ss), prompts_current, prompts_draft, prompts_insert, s);
}
