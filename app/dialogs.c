#include "dialogs.h"
#include "dialogs_input_internal.h"
#include "saved_prompt.h"
#include "mail_reply.h"
#include "canvas.h"
#include "resource.h"
#include "screens.h"
#include "str.h"
#include "theme.h"
#include "voice.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <string.h>

static HBRUSH dialog_brush;

static char *control_text(HWND dialog, int id) {
    HWND h = GetDlgItem(dialog, id);
    int n = GetWindowTextLengthW(h);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(h, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}
static void set_control_text(HWND dialog, int id, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetDlgItemTextW(dialog, id, w);
    free(w); free(crlf);
}
static void append_control_text(HWND dialog, int id, const char *text) {
    char *current = control_text(dialog, id);
    size_t n = strlen(current);
    const char *gap = (!n || current[n - 1] == ' ' || current[n - 1] == '\n' || current[n - 1] == '\t') ? "" : " ";
    char *joined = xstrfmt("%s%s%s", current, gap, text);
    set_control_text(dialog, id, joined);
    HWND h = GetDlgItem(dialog, id);
    int len = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, len, len);
    free(joined); free(current);
}

static BOOL CALLBACK theme_child(HWND child, LPARAM lp) {
    (void)lp;
    wchar_t cls[32]; GetClassNameW(child, cls, 32);
    if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"ListBox") == 0 || _wcsicmp(cls, L"Edit") == 0) theme_apply_control(child);
    else if (_wcsicmp(cls, L"ComboBox") == 0) SetWindowTheme(child, theme.dark ? L"DarkMode_CFD" : L"Explorer", NULL);
    return TRUE;
}
void dialog_theme(HWND dialog) {
    theme_apply_window(dialog);
    EnumChildWindows(dialog, theme_child, 0);
    if (dialog_brush) DeleteObject(dialog_brush);
    dialog_brush = CreateSolidBrush(theme.background);
}
LRESULT dialog_ctl_color(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    (void)dialog; (void)lp;
    HDC hdc = (HDC)wp;
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        SetTextColor(hdc, theme.text); SetBkColor(hdc, theme.background); SetBkMode(hdc, TRANSPARENT);
        if (!dialog_brush) dialog_brush = CreateSolidBrush(theme.background);
        return (LRESULT)dialog_brush;
    }
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        static HBRUSH edit_brush;
        SetTextColor(hdc, theme.text); SetBkColor(hdc, theme.elevated);
        if (edit_brush) DeleteObject(edit_brush);
        edit_brush = CreateSolidBrush(theme.elevated);
        return (LRESULT)edit_brush;
    }
    return 0;
}

/// A dialog box over one of our windows keeps the others waiting too (app_modal_begin); one asked from elsewhere does not.
static INT_PTR modal_box(int id, HWND owner, DLGPROC proc, LPARAM param) {
    if (owner) app_modal_begin(owner);
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(id), owner, proc, param);
    if (owner) app_modal_end();
    return result;
}

// MARK: - Drawing the dialogs' own controls

/// Edits lose their system border and get a rounded frame painted by the dialog, focused in the accent colour.
static void dialog_prepare_edit(HWND dialog, int id) {
    HWND e = GetDlgItem(dialog, id);
    RECT r; GetClientRect(e, &r);
    SetWindowRgn(e, CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1, px(8), px(8)), TRUE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(8), px(8)));
    if (GetWindowLongW(e, GWL_STYLE) & ES_MULTILINE) {
        RECT f = { 0 }; SendMessageW(e, EM_GETRECT, 0, (LPARAM)&f);
        f.top += px(6); f.bottom -= px(6);
        SendMessageW(e, EM_SETRECT, 0, (LPARAM)&f);
    }
    (void)dialog;
}
static RECT edit_frame_rect(HWND dialog, HWND e) {
    RECT r; GetWindowRect(e, &r);
    MapWindowPoints(NULL, dialog, (POINT *)&r, 2);
    InflateRect(&r, px(2), px(2));
    return r;
}
static void dialog_paint_frames(HWND dialog, const int *ids, size_t count) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(dialog, &ps);
    for (size_t i = 0; i < count; i++) {
        HWND e = GetDlgItem(dialog, ids[i]);
        if (!e || !IsWindowVisible(e)) continue;
        // Each frame is drawn on its own, since a borrowed canvas paints all of the rectangle it is given.
        RECT r = edit_frame_rect(dialog, e);
        Canvas *cv = canvas_begin_dc(hdc, &r);
        if (!cv) continue;
        RECT local = { 0, 0, r.right - r.left, r.bottom - r.top };
        fill_rect(cv, &local, theme.background);
        fill_round_rect(cv, &local, px(9), theme.elevated, GetFocus() == e ? theme.accent : theme.border);
        canvas_end_dc(cv);
    }
    EndPaint(dialog, &ps);
}
static void dialog_invalidate_frame(HWND dialog, int id) {
    HWND e = GetDlgItem(dialog, id);
    if (!e) return;
    RECT r = edit_frame_rect(dialog, e);
    InvalidateRect(dialog, &r, TRUE);
}
/// Buttons are owner-drawn as the app's: the one that goes ahead filled in the accent colour, the rest bordered.
typedef enum { DIALOG_BUTTON_PROMINENT, DIALOG_BUTTON_BORDERED } DialogButtonStyle;
static void dialog_draw_button(const DRAWITEMSTRUCT *di, DialogButtonStyle style) {
    RECT rc = di->rcItem;
    bool disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0, focused = (di->itemState & ODS_FOCUS) != 0;
    COLORREF fill, border, text;
    if (style == DIALOG_BUTTON_PROMINENT) { fill = disabled ? blend(theme.secondary, theme.background, 0.35) : theme.accent; border = fill; text = theme.white; }
    else { fill = theme.surface; border = focused ? theme.accent : theme.border; text = theme.text; }
    if (pressed) fill = blend(theme.text, fill, 0.12);
    if (disabled) text = blend(text, fill, 0.5);
    Canvas *cv = canvas_begin_dc(di->hDC, &di->rcItem);
    if (!cv) return;
    OffsetRect(&rc, -rc.left, -rc.top);   // the canvas starts at the item's corner
    fill_rect(cv, &rc, theme.background);
    fill_round_rect(cv, &rc, px(6), fill, border);
    wchar_t label[64]; GetWindowTextW(di->hwndItem, label, 64);
    draw_textw(cv, label, &rc, FONT_SUBHEADLINE_SEMIBOLD, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    canvas_end_dc(cv);
}
static bool dialog_draw_item(WPARAM wp, LPARAM lp, int prominent_id) {
    (void)wp;
    const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
    if (di->CtlType != ODT_BUTTON) return false;
    dialog_draw_button(di, (int)di->CtlID == prominent_id ? DIALOG_BUTTON_PROMINENT : DIALOG_BUTTON_BORDERED);
    return true;
}
/// Static text in the secondary colour (labels, notes) or the danger colour (errors), on the dialog's background.
static LRESULT dialog_static_color(HWND dialog, WPARAM wp, LPARAM lp, COLORREF color) {
    LRESULT brush = dialog_ctl_color(dialog, WM_CTLCOLORSTATIC, wp, lp);
    SetTextColor((HDC)wp, color);
    return brush;
}

// MARK: - Voice in dialogs

void dialog_voice_append(DialogVoice *v, const char *text) {
    if (!text || !*text) return;
    append_control_text(v->dialog, v->text_id, text);
    // Multiline edits do not emit EN_CHANGE for WM_SETTEXT; feedback must enable Start.
    if (v->text_id == IDC_INPUT) SendMessageW(v->dialog, WM_COMMAND, MAKEWPARAM(IDC_INPUT, EN_CHANGE), (LPARAM)GetDlgItem(v->dialog, IDC_INPUT));
}
static void dialog_voice_changed(void *ctx) {
    DialogVoice *v = ctx;
    char *text = voice_take_text(v->note);
    if (text) { dialog_voice_append(v, text); free(text); }
    char *error = voice_take_error(v->note);
    if (error) { wchar_t *w = utf8_to_wide(error); MessageBoxW(v->dialog, w, L"Voice note", MB_OK | MB_ICONINFORMATION); free(w); free(error); }
    VoiceState state = voice_state(v->note);
    const wchar_t *label = state == VOICE_RECORDING ? L"Stop and transcribe" : state == VOICE_TRANSCRIBING ? L"Transcribing\x2026" : state == VOICE_STARTING ? L"Starting\x2026" : L"Record a voice note";
    SetDlgItemTextW(v->dialog, IDC_MIC, label);
    EnableWindow(GetDlgItem(v->dialog, IDC_MIC), state == VOICE_IDLE || state == VOICE_RECORDING);
    if (state == VOICE_RECORDING) SetTimer(v->dialog, 7, 1000, NULL); else { KillTimer(v->dialog, 7); SetDlgItemTextW(v->dialog, IDC_RECORDING, L""); }
}
static void dialog_voice_tick(DialogVoice *v) {
    if (!v->note) return;
    voice_tick(v->note);
    if (voice_state(v->note) == VOICE_RECORDING) {
        char *clock = format_clock(voice_elapsed(v->note));
        char *text = xstrfmt("Recording %s \xC2\xB7 Esc discards", clock);
        wchar_t *w = utf8_to_wide(text);
        SetDlgItemTextW(v->dialog, IDC_RECORDING, w);
        free(w); free(text); free(clock);
    }
}
static void dialog_voice_toggle(DialogVoice *v) {
    if (!v->note) return;
    VoiceState state = voice_state(v->note);
    if (state == VOICE_RECORDING) voice_stop(v->note); else if (state == VOICE_IDLE) voice_record(v->note);
}

// MARK: - Rename

typedef struct { const char *caption, *label, *ok_label, *current; char *result; bool secret; } RenameState;
static INT_PTR CALLBACK rename_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    RenameState *r = (RenameState *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        r = (RenameState *)lp; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_TITLE);
        SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
        { wchar_t *caption = utf8_to_wide(r->caption); SetWindowTextW(dialog, caption); free(caption); }
        set_control_text(dialog, IDC_TITLE_LABEL, r->label);
        set_control_text(dialog, IDOK, r->ok_label);
        set_control_text(dialog, IDC_TITLE, r->current);
        if (r->secret) {
            SendMessageW(GetDlgItem(dialog, IDC_TITLE), EM_SETPASSWORDCHAR, 0x25CF, 0);
            // Asked by another program (ssh), so this window has to come forward by itself.
            SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(dialog);
        }
        SendMessageW(GetDlgItem(dialog, IDC_TITLE), EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_TITLE));
        return FALSE;
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_TITLE }; dialog_paint_frames(dialog, ids, 1); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDOK) ? TRUE : FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TITLE && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) { dialog_invalidate_frame(dialog, IDC_TITLE); return TRUE; }
        if (LOWORD(wp) == IDOK) { r->result = control_text(dialog, IDC_TITLE); EndDialog(dialog, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
char *dialog_text(HWND owner, const char *caption, const char *label, const char *ok_label, const char *current) {
    RenameState r = { caption, label, ok_label, current ? current : "", NULL, false };
    if (modal_box(IDD_RENAME, owner, rename_proc, (LPARAM)&r) != IDOK) { free(r.result); return NULL; }
    return r.result;
}
char *dialog_password(HWND owner, const char *caption, const char *label) {
    RenameState r = { caption, label, "OK", "", NULL, true };
    if (modal_box(IDD_RENAME, owner, rename_proc, (LPARAM)&r) != IDOK) {
        if (r.result) SecureZeroMemory(r.result, strlen(r.result));
        free(r.result); return NULL;
    }
    return r.result;
}
char *dialog_rename(HWND owner, const char *current) { return dialog_text(owner, "Rename conversation", "Title", "Save", current); }

// MARK: - Action input

static bool input_current(void *ctx, const char *repo, const char *identity) {
    (void)identity; InputState *s = ctx;
    if (s->closed || !str_eq(repo, s->repo) || !IsWindowVisible(s->dialog)) return false;
    if ((InputState *)GetWindowLongPtrW(s->dialog, GWLP_USERDATA) != s) return false;
    return IsWindowVisible(s->source_window) && pane_top(s->pane) == s->source;
}
static char *input_draft(void *ctx) { return control_text(((InputState *)ctx)->dialog, IDC_INPUT); }
static void input_insert(void *ctx, const char *body) {
    InputState *s = ctx; char *draft = input_draft(s), *joined = saved_prompt_insert(draft, body);
    set_control_text(s->dialog, IDC_INPUT, joined); free(draft); free(joined);
    HWND edit = GetDlgItem(s->dialog, IDC_INPUT); int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len); SetFocus(edit);
    SendMessageW(s->dialog, WM_COMMAND, MAKEWPARAM(IDC_INPUT, EN_CHANGE), (LPARAM)edit);
}
static void input_close(InputState *s) {
    s->closed = true;
    prompt_picker_free(s->prompts); s->prompts = NULL;
    if (s->voice.note) { voice_free(s->voice.note); s->voice.note = NULL; }
}
static void input_update(InputState *s) {
    char *text = control_text(s->dialog, IDC_INPUT); char *trimmed = str_trim(text);
    EnableWindow(GetDlgItem(s->dialog, IDC_START), !(*trimmed == 0 && s->action->input.required));
    free(text); free(trimmed);
}
static INT_PTR CALLBACK input_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    InputState *s = (InputState *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        s = (InputState *)lp; s->dialog = dialog; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_INPUT);
        SendMessageW(dialog, DM_SETDEFID, IDC_START, 0);
        wchar_t *title = utf8_to_wide(s->action->label); SetWindowTextW(dialog, title); free(title);
        const char *hint = !str_empty(s->action->input.placeholder) ? s->action->input.placeholder : s->action->input.label;
        set_control_text(dialog, IDC_HINT, hint ? hint : "");
        char *note = xstrfmt("%s. This runs a paid agent on pull request #%d and may write to GitHub.", s->action->hint, s->number);
        set_control_text(dialog, IDC_NOTE, note); free(note);
        s->voice.dialog = dialog; s->voice.text_id = IDC_INPUT;
        if (!store_can_transcribe()) ShowWindow(GetDlgItem(dialog, IDC_MIC), SW_HIDE);
        else s->voice.note = voice_new(dialog_voice_changed, &s->voice);
        s->prompts = prompt_picker_dialog_new(dialog, s->repo, "feedback", input_current, input_draft, input_insert, s);
        prompt_picker_visible(s->prompts, true);
        bool library = str_eq(s->action->id, "custom-feedback") && store_supports("prompts")
            && saved_prompt_scope_allowed(g_store.device.permission, g_store.device.repos, g_store.device.repo_count, s->repo);
        ShowWindow(GetDlgItem(dialog, IDC_PROMPT_LIBRARY), library ? SW_SHOW : SW_HIDE);
        input_update(s);
        SetFocus(GetDlgItem(dialog, IDC_INPUT));
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, GetDlgCtrlID((HWND)lp) == IDC_HINT ? theme.text : theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_INPUT }; dialog_paint_frames(dialog, ids, 1); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDC_START) ? TRUE : FALSE;
    case WM_TIMER: if (wp == 7 && s) dialog_voice_tick(&s->voice); return TRUE;
    case WM_SHOWWINDOW:
        if (s && s->prompts) {
            prompt_picker_visible(s->prompts, wp != 0);
        }
        return FALSE;
    case WM_DESTROY: if (s) input_close(s); return TRUE;
    case WM_COMMAND:
        if (!s) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_INPUT:
            if (HIWORD(wp) == EN_CHANGE) input_update(s);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, IDC_INPUT);
            return TRUE;
        case IDC_MIC: dialog_voice_toggle(&s->voice); return TRUE;
        case IDC_PROMPT_LIBRARY: {
            RECT r; GetWindowRect(GetDlgItem(dialog, IDC_PROMPT_LIBRARY), &r);
            prompt_picker_open(s->prompts, (POINT){r.left, r.bottom}); return TRUE;
        }
        case IDC_START: {
            if (!prompt_picker_owner_current(s->prompts)) { EndDialog(dialog, IDCANCEL); return TRUE; }
            char *text = control_text(dialog, IDC_INPUT); char *trimmed = str_trim(text); free(text);
            if (!*trimmed && s->action->input.required) { free(trimmed); return TRUE; }
            s->input = trimmed; EndDialog(dialog, IDOK); return TRUE;
        }
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_action_input(HWND owner, Screen *source, const char *repo, const BoardAction *action, int number, char **input) {
    InputState s; memset(&s, 0, sizeof s);
    s.action = action; s.number = number; s.repo = xstrdup(repo);
    s.source = source; s.pane = source->pane; s.source_window = pane_hwnd(s.pane);
    INT_PTR result = modal_box(IDD_ACTION_INPUT, owner, input_proc, (LPARAM)&s);
    input_close(&s); free(s.repo);
    if (result != IDOK) { free(s.input); return false; }
    *input = s.input ? s.input : xstrdup("");
    return true;
}

// MARK: - Meeting prompt

typedef struct { const char *project, *default_prompt, *default_first; char **prompt, **first; } MeetPromptState;
static INT_PTR CALLBACK meet_prompt_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    MeetPromptState *s = (MeetPromptState *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        s = (MeetPromptState *)lp; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_MEET_PROMPT);
        dialog_prepare_edit(dialog, IDC_MEET_FIRST);
        SendMessageW(dialog, DM_SETDEFID, IDC_START, 0);
        SendDlgItemMessageW(dialog, IDC_MEET_PROMPT, EM_SETLIMITTEXT, 60000, 0);
        char *title = xstrfmt("Meeting assistant \xC2\xB7 %s", s->project);
        { wchar_t *w = utf8_to_wide(title); SetWindowTextW(dialog, w); free(w); }
        free(title);
        set_control_text(dialog, IDC_MEET_PROMPT_LABEL, "System prompt: what the ElevenLabs agent is told. The project's tools and how to use them are part of it.");
        set_control_text(dialog, IDC_MEET_FIRST_LABEL, "First message: what it says as it joins. Empty: it joins silently.");
        set_control_text(dialog, IDC_NOTE, "{name}, {project} and {wake_words} are filled in as it joins. Saved for the next meeting.");
        set_control_text(dialog, IDC_MEET_PROMPT, *s->prompt);
        set_control_text(dialog, IDC_MEET_FIRST, *s->first);
        SetFocus(GetDlgItem(dialog, IDC_MEET_PROMPT));
        SendDlgItemMessageW(dialog, IDC_MEET_PROMPT, EM_SETSEL, 0, 0);
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_MEET_PROMPT, IDC_MEET_FIRST }; dialog_paint_frames(dialog, ids, 2); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDC_START) ? TRUE : FALSE;
    case WM_COMMAND:
        if (!s) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_MEET_PROMPT: case IDC_MEET_FIRST:
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, LOWORD(wp));
            return TRUE;
        case IDC_MEET_RESET:
            set_control_text(dialog, IDC_MEET_PROMPT, s->default_prompt);
            set_control_text(dialog, IDC_MEET_FIRST, s->default_first);
            return TRUE;
        case IDC_START: {
            char *prompt = control_text(dialog, IDC_MEET_PROMPT), *first = control_text(dialog, IDC_MEET_FIRST);
            free(*s->prompt); *s->prompt = str_trim(prompt);
            free(*s->first); *s->first = str_trim(first);
            free(prompt); free(first);
            // An empty prompt is the default.
            if (!**s->prompt) { free(*s->prompt); *s->prompt = xstrdup(s->default_prompt); }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_meeting_prompt(HWND owner, const char *project, const char *default_prompt, const char *default_first,
                           char **prompt, char **first_message) {
    MeetPromptState s = { project, default_prompt, default_first, prompt, first_message };
    return modal_box(IDD_MEET_PROMPT, owner, meet_prompt_proc, (LPARAM)&s) == IDOK;
}

// MARK: - Edit an issue or pull request

typedef struct { const char *caption; char **title, **body; } EditItemState;
static void edit_item_update(HWND dialog) {
    char *text = control_text(dialog, IDC_EDIT_TITLE), *trimmed = str_trim(text);
    EnableWindow(GetDlgItem(dialog, IDOK), *trimmed != 0);
    free(text); free(trimmed);
}
static INT_PTR CALLBACK edit_item_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    EditItemState *s = (EditItemState *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        s = (EditItemState *)lp; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_EDIT_TITLE);
        dialog_prepare_edit(dialog, IDC_EDIT_BODY);
        SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
        // GitHub takes a body of up to 65,536 characters.
        SendDlgItemMessageW(dialog, IDC_EDIT_BODY, EM_SETLIMITTEXT, 65536, 0);
        { wchar_t *w = utf8_to_wide(s->caption); SetWindowTextW(dialog, w); free(w); }
        set_control_text(dialog, IDC_EDIT_TITLE, *s->title);
        // GitHub keeps a body as it was typed, often with CRLF; the box adds its own CR to each line.
        { char *body = str_replace(*s->body ? *s->body : "", "\r\n", "\n"); set_control_text(dialog, IDC_EDIT_BODY, body); free(body); }
        edit_item_update(dialog);
        SetFocus(GetDlgItem(dialog, IDC_EDIT_TITLE));
        SendDlgItemMessageW(dialog, IDC_EDIT_TITLE, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_EDIT_TITLE, IDC_EDIT_BODY }; dialog_paint_frames(dialog, ids, 2); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDOK) ? TRUE : FALSE;
    case WM_COMMAND:
        if (!s) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_EDIT_TITLE: case IDC_EDIT_BODY:
            if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_EDIT_TITLE) edit_item_update(dialog);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, LOWORD(wp));
            return TRUE;
        case IDOK: {
            char *title = control_text(dialog, IDC_EDIT_TITLE), *body = control_text(dialog, IDC_EDIT_BODY);
            char *trimmed = str_trim(title);
            if (!*trimmed) { free(title); free(body); free(trimmed); return TRUE; }
            free(*s->title); *s->title = trimmed;
            free(*s->body); *s->body = body;
            free(title);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_edit_item(HWND owner, const char *caption, char **title, char **body) {
    EditItemState s = { caption, title, body };
    return modal_box(IDD_EDIT_ITEM, owner, edit_item_proc, (LPARAM)&s) == IDOK;
}

// MARK: - Usage calendar days

typedef struct { UsageRange value; } UsageRangeState;

static bool usage_range_read(HWND dialog, UsageRange *range) {
    char *from = control_text(dialog, IDC_USAGE_FROM), *to = control_text(dialog, IDC_USAGE_TO);
    bool valid = usage_range_make(from, to, range);
    free(from); free(to);
    return valid;
}

static void usage_range_update(HWND dialog) {
    UsageRange range;
    bool valid = usage_range_read(dialog, &range);
    EnableWindow(GetDlgItem(dialog, IDOK), valid);
    set_control_text(dialog, IDC_HINT, valid ? "" : "Enter valid days spanning at most 366 inclusive days.");
}

static INT_PTR CALLBACK usage_range_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    UsageRangeState *s = (UsageRangeState *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        s = (UsageRangeState *)lp; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_USAGE_FROM); dialog_prepare_edit(dialog, IDC_USAGE_TO);
        SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
        set_control_text(dialog, IDC_USAGE_FROM, s->value.from); set_control_text(dialog, IDC_USAGE_TO, s->value.to);
        usage_range_update(dialog);
        SetFocus(GetDlgItem(dialog, IDC_USAGE_FROM));
        SendDlgItemMessageW(dialog, IDC_USAGE_FROM, EM_SETSEL, 0, -1);
        return FALSE;
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_USAGE_FROM, IDC_USAGE_TO }; dialog_paint_frames(dialog, ids, 2); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDOK) ? TRUE : FALSE;
    case WM_COMMAND:
        if (!s) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_USAGE_FROM: case IDC_USAGE_TO:
            if (HIWORD(wp) == EN_CHANGE) usage_range_update(dialog);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, LOWORD(wp));
            return TRUE;
        case IDOK:
            if (usage_range_read(dialog, &s->value)) EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}

bool dialog_usage_range(HWND owner, UsageRange *range) {
    UsageRangeState s = { *range };
    if (modal_box(IDD_USAGE_RANGE, owner, usage_range_proc, (LPARAM)&s) != IDOK) return false;
    *range = s.value;
    return true;
}

// MARK: - Plain-text mail draft

typedef struct { const char *context; char **text; } MailDraftDialog;
static void mail_draft_update(HWND dialog) {
    char *text = control_text(dialog, IDC_INPUT);
    Json *body = mail_reply_body(text);
    set_control_text(dialog, IDC_RECORDING, body ? "Plain text, ready to send from the reader." : "Use nonempty text, at most 100000 UTF-16 characters.");
    json_free(body); free(text);
}
static INT_PTR CALLBACK mail_draft_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    MailDraftDialog *s = (MailDraftDialog *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        s = (MailDraftDialog *)lp; SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog); dialog_prepare_edit(dialog, IDC_INPUT);
        SetWindowTextW(dialog, L"Mail reply draft");
        set_control_text(dialog, IDC_HINT, "Plain-text reply; review recipient and thread before sending.");
        set_control_text(dialog, IDC_NOTE, s->context);
        set_control_text(dialog, IDC_START, "Save draft");
        SendDlgItemMessageW(dialog, IDC_INPUT, EM_SETLIMITTEXT, 200000, 0);
        set_control_text(dialog, IDC_INPUT, *s->text);
        mail_draft_update(dialog);
        SendMessageW(dialog, DM_SETDEFID, IDC_START, 0);
        SetFocus(GetDlgItem(dialog, IDC_INPUT)); return FALSE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_INPUT }; dialog_paint_frames(dialog, ids, 1); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDC_START) ? TRUE : FALSE;
    case WM_COMMAND:
        if (!s) return FALSE;
        if (LOWORD(wp) == IDC_INPUT) {
            if (HIWORD(wp) == EN_CHANGE) mail_draft_update(dialog);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, IDC_INPUT);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_START) {
            char *text = control_text(dialog, IDC_INPUT); free(*s->text); *s->text = text;
            EndDialog(dialog, IDOK); return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_mail_reply(HWND owner, const char *context, char **text) {
    MailDraftDialog s = { context, text };
    return modal_box(IDD_MAIL_REPLY, owner, mail_draft_proc, (LPARAM)&s) == IDOK;
}
