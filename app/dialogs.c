#include "dialogs.h"
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
static void combo_add(HWND combo, const char *text) { wchar_t *w = utf8_to_wide(text); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w); free(w); }

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

// MARK: - Drawing the dialogs' own controls

/// Edits lose their system border and get a rounded frame painted by the dialog, focused in the accent colour.
static void dialog_prepare_edit(HWND dialog, int id) {
    HWND e = GetDlgItem(dialog, id);
    RECT r; GetClientRect(e, &r);
    SetWindowRgn(e, CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1, px(8), px(8)), TRUE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(8), px(8)));
    if (GetWindowLongW(e, GWL_STYLE) & ES_MULTILINE) {
        RECT f; SendMessageW(e, EM_GETRECT, 0, (LPARAM)&f);
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

typedef struct { HWND dialog; VoiceNote *note; int text_id; } DialogVoice;
static void dialog_voice_changed(void *ctx) {
    DialogVoice *v = ctx;
    char *text = voice_take_text(v->note);
    if (text) { if (*text) append_control_text(v->dialog, v->text_id, text); free(text); }
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

// MARK: - New conversation

typedef struct {
    const Project *project;
    Session *started;
    HWND dialog;
    bool busy, uncertain;
    bool has_catalog; RuntimeCatalog catalog;
    bool has_runtime; RuntimeChoice runtime;   // the user's pick; none starts on the project default
    char **branches; size_t branch_count; char *default_branch;
    Request *req_runtimes, *req_branches, *req_start;
    DialogVoice voice;
    // combo rows: index -> (provider, model)
    struct { int provider; char *model; bool is_default; bool unavailable; } *rows; size_t row_count;
} NewConversation;

static bool effective_choice(NewConversation *d, RuntimeChoice *out) {
    if (d->has_runtime) { runtime_choice_copy(out, &d->runtime); return true; }
    if (d->has_catalog && d->catalog.has_default) { runtime_choice_copy(out, &d->catalog.def); return true; }
    return false;
}
static void fill_effort(NewConversation *d) {
    HWND combo = GetDlgItem(d->dialog, IDC_EFFORT);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    RuntimeChoice eff;
    bool has = d->has_catalog && effective_choice(d, &eff);
    size_t n = 0; const char *const *efforts = has ? runtime_catalog_efforts(&d->catalog, &eff, &n) : NULL;
    bool show = n > 0;
    ShowWindow(combo, show ? SW_SHOW : SW_HIDE); ShowWindow(GetDlgItem(d->dialog, IDC_EFFORT_LABEL), show ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < n; i++) {
        char *title = str_capitalized(efforts[i]); combo_add(combo, title); free(title);
        if (str_eq(efforts[i], eff.effort)) SendMessageW(combo, CB_SETCURSEL, (WPARAM)i, 0);
    }
    if (n && SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) SendMessageW(combo, CB_SETCURSEL, 0, 0);
    if (has) runtime_choice_free(&eff);
}
static void fill_models(NewConversation *d) {
    HWND combo = GetDlgItem(d->dialog, IDC_MODEL);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < d->row_count; i++) free(d->rows[i].model);
    free(d->rows); d->rows = NULL; d->row_count = 0;
    bool show = d->has_catalog && d->catalog.provider_count > 0;
    ShowWindow(combo, show ? SW_SHOW : SW_HIDE); ShowWindow(GetDlgItem(d->dialog, IDC_MODEL_LABEL), show ? SW_SHOW : SW_HIDE);
    if (!show) { fill_effort(d); return; }
    RuntimeChoice eff; bool has_eff = effective_choice(d, &eff);
    int selected = -1;
    if (d->catalog.has_default) {
        char *label = runtime_catalog_label(&d->catalog, &d->catalog.def);
        char *text = xstrfmt("Project default (%s)", label);
        combo_add(combo, text); free(text); free(label);
        d->rows = xrealloc(d->rows, (d->row_count + 1) * sizeof *d->rows);
        d->rows[d->row_count].provider = 0; d->rows[d->row_count].model = NULL; d->rows[d->row_count].is_default = true; d->rows[d->row_count].unavailable = false;
        if (!d->has_runtime) selected = (int)d->row_count;
        d->row_count++;
    }
    for (size_t p = 0; p < d->catalog.provider_count; p++) {
        const RuntimeProvider *provider = &d->catalog.providers[p];
        if (!runtime_provider_available(provider)) {
            char *text = xstrfmt("%s (unavailable)", provider->label); combo_add(combo, text); free(text);
            d->rows = xrealloc(d->rows, (d->row_count + 1) * sizeof *d->rows);
            d->rows[d->row_count].provider = provider->id; d->rows[d->row_count].model = NULL; d->rows[d->row_count].is_default = false; d->rows[d->row_count].unavailable = true;
            d->row_count++;
            continue;
        }
        if (!provider->model_count) {
            combo_add(combo, provider->label);
            d->rows = xrealloc(d->rows, (d->row_count + 1) * sizeof *d->rows);
            d->rows[d->row_count].provider = provider->id; d->rows[d->row_count].model = NULL; d->rows[d->row_count].is_default = false; d->rows[d->row_count].unavailable = false;
            if (d->has_runtime && has_eff && eff.provider_id == provider->id) selected = (int)d->row_count;
            d->row_count++;
            continue;
        }
        for (size_t m = 0; m < provider->model_count; m++) {
            char *text = xstrfmt("%s \xC2\xB7 %s", provider->label, runtime_model_title(&provider->models[m]));
            combo_add(combo, text); free(text);
            d->rows = xrealloc(d->rows, (d->row_count + 1) * sizeof *d->rows);
            d->rows[d->row_count].provider = provider->id; d->rows[d->row_count].model = xstrdup(provider->models[m].id); d->rows[d->row_count].is_default = false; d->rows[d->row_count].unavailable = false;
            if (d->has_runtime && has_eff && eff.provider_id == provider->id && str_eq(eff.model, provider->models[m].id)) selected = (int)d->row_count;
            d->row_count++;
        }
    }
    if (selected >= 0) SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
    if (has_eff) runtime_choice_free(&eff);
    SetDlgItemTextW(d->dialog, IDC_NOTE, L"Starting a conversation runs a paid agent on the selected model.");
    fill_effort(d);
}
static void update_start(NewConversation *d) {
    char *prompt = control_text(d->dialog, IDC_PROMPT); char *trimmed = str_trim(prompt);
    RuntimeChoice eff; bool has_eff = effective_choice(d, &eff); if (has_eff) runtime_choice_free(&eff);
    bool needs_runtime = d->has_catalog && !has_eff;
    EnableWindow(GetDlgItem(d->dialog, IDC_START), !d->busy && !d->uncertain && !needs_runtime && *trimmed);
    free(prompt); free(trimmed);
}
static void show_catalog(NewConversation *d, const Json *value) {
    RuntimeCatalog c;
    if (!runtime_catalog_parse(value, &c)) return;
    if (d->has_catalog) runtime_catalog_free(&d->catalog);
    d->catalog = c; d->has_catalog = true;
    if (!c.has_default && !d->has_runtime) { RuntimeChoice first; if (runtime_catalog_first_available(&c, &first)) { d->runtime = first; d->has_runtime = true; } }
    fill_models(d); update_start(d);
}
static void runtimes_done(void *owner, Request *req) {
    NewConversation *d = owner;
    if (!req->ok) return;
    show_catalog(d, req->result);
    char *key = xstrfmt("runtimes:%s", d->project->repo); cache_store(g_store.cache, req->result, key); free(key);
}
static void show_branches(NewConversation *d, const Json *value) {
    str_array_free(d->branches, d->branch_count); d->branches = NULL; d->branch_count = 0;
    const Json *list = json_get(value, "branches");
    d->branches = xmalloc((json_count(list) ? json_count(list) : 1) * sizeof *d->branches);
    for (size_t i = 0; i < json_count(list); i++) { const char *b = json_str(json_at(list, i)); if (b) d->branches[d->branch_count++] = xstrdup(b); }
    set_string(&d->default_branch, json_str(json_get(value, "defaultBranch")));
    HWND combo = GetDlgItem(d->dialog, IDC_BRANCH);
    char *typed = control_text(d->dialog, IDC_BRANCH);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    char *first = d->default_branch ? xstrfmt("%s (default)", d->default_branch) : xstrdup("Default branch");
    combo_add(combo, first); free(first);
    for (size_t i = 0; i < d->branch_count; i++) if (!str_eq(d->branches[i], d->default_branch)) combo_add(combo, d->branches[i]);
    if (str_empty(typed)) SendMessageW(combo, CB_SETCURSEL, 0, 0); else { wchar_t *w = utf8_to_wide(typed); SetWindowTextW(combo, w); free(w); }
    free(typed);
}
static void branches_done(void *owner, Request *req) {
    NewConversation *d = owner;
    if (!req->ok) return;
    show_branches(d, req->result);
    char *key = xstrfmt("branches:%s", d->project->repo); cache_store(g_store.cache, req->result, key); free(key);
}
static char *chosen_branch(NewConversation *d) {
    HWND combo = GetDlgItem(d->dialog, IDC_BRANCH);
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == 0) return xstrdup("");
    char *text = control_text(d->dialog, IDC_BRANCH);
    char *trimmed = str_trim(text); free(text);
    if (d->default_branch) { char *first = xstrfmt("%s (default)", d->default_branch); if (str_eq(trimmed, first) || str_eq(trimmed, "Default branch")) { free(trimmed); trimmed = xstrdup(""); } free(first); }
    else if (str_eq(trimmed, "Default branch")) { free(trimmed); trimmed = xstrdup(""); }
    return trimmed;
}
static void start_done(void *owner, Request *req) {
    NewConversation *d = owner;
    d->busy = false;
    if (req->ok && session_parse(json_get(req->result, "session"), d->started)) { EndDialog(d->dialog, IDOK); return; }
    char *text = request_error_or_unexpected(req);
    set_control_text(d->dialog, IDC_ERROR, text); free(text);
    d->uncertain = true;
    ShowWindow(GetDlgItem(d->dialog, IDC_NOTE), SW_HIDE); ShowWindow(GetDlgItem(d->dialog, IDC_ERROR), SW_SHOW);
    ShowWindow(GetDlgItem(d->dialog, IDC_UNCERTAIN), SW_SHOW); ShowWindow(GetDlgItem(d->dialog, IDC_RETURN), SW_SHOW);
    EnableWindow(GetDlgItem(d->dialog, IDCANCEL), TRUE);
    update_start(d);
}
static void start(NewConversation *d) {
    if (d->busy) return;
    char *prompt = control_text(d->dialog, IDC_PROMPT);
    Json *args = json_object();
    json_set_str(args, "repo", d->project->repo); json_set_str(args, "prompt", prompt);
    char *branch = chosen_branch(d);
    if (*branch) json_set_str(args, "branch", branch);
    free(branch); free(prompt);
    if (d->has_runtime) { Json *rt = runtime_choice_arguments(&d->runtime); json_object_merge(args, rt); json_free(rt); }
    d->busy = true;
    SetDlgItemTextW(d->dialog, IDC_ERROR, L"");
    EnableWindow(GetDlgItem(d->dialog, IDCANCEL), FALSE);
    update_start(d);
    store_call("start_session", args, 0, d, start_done, 0, &d->req_start);
}
static void model_changed(NewConversation *d) {
    int sel = (int)SendMessageW(GetDlgItem(d->dialog, IDC_MODEL), CB_GETCURSEL, 0, 0);
    if (sel < 0 || (size_t)sel >= d->row_count) return;
    if (d->rows[sel].unavailable) { fill_models(d); return; }
    if (d->rows[sel].is_default) { if (d->has_runtime) runtime_choice_free(&d->runtime); d->has_runtime = false; }
    else {
        RuntimeChoice c;
        if (runtime_catalog_choice(&d->catalog, d->rows[sel].provider, d->rows[sel].model, &c)) { if (d->has_runtime) runtime_choice_free(&d->runtime); d->runtime = c; d->has_runtime = true; }
    }
    fill_effort(d); update_start(d);
}
static void effort_changed(NewConversation *d) {
    RuntimeChoice eff; if (!effective_choice(d, &eff)) return;
    size_t n; const char *const *efforts = runtime_catalog_efforts(&d->catalog, &eff, &n);
    int sel = (int)SendMessageW(GetDlgItem(d->dialog, IDC_EFFORT), CB_GETCURSEL, 0, 0);
    if (sel >= 0 && (size_t)sel < n) {
        RuntimeChoice c = { eff.provider_id, xstrdup(eff.model), xstrdup(efforts[sel]) };
        if (d->has_runtime) runtime_choice_free(&d->runtime);
        d->runtime = c; d->has_runtime = true;
    }
    runtime_choice_free(&eff);
}
static INT_PTR CALLBACK new_conversation_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    NewConversation *d = (NewConversation *)GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        d = (NewConversation *)lp; d->dialog = dialog;
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lp);
        dialog_theme(dialog);
        dialog_prepare_edit(dialog, IDC_PROMPT);
        SendMessageW(dialog, DM_SETDEFID, IDC_START, 0);
        { const char *t = project_title(d->project); char *line = str_eq(t, d->project->repo) ? xstrdup(t) : xstrfmt("%s \xC2\xB7 %s", t, d->project->repo); set_control_text(dialog, IDC_PROJECT_NAME, line); free(line); }
        ShowWindow(GetDlgItem(dialog, IDC_MODEL), SW_HIDE); ShowWindow(GetDlgItem(dialog, IDC_MODEL_LABEL), SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDC_EFFORT), SW_HIDE); ShowWindow(GetDlgItem(dialog, IDC_EFFORT_LABEL), SW_HIDE);
        combo_add(GetDlgItem(dialog, IDC_BRANCH), "Default branch");
        SendMessageW(GetDlgItem(dialog, IDC_BRANCH), CB_SETCURSEL, 0, 0);
        if (!store_can_transcribe()) ShowWindow(GetDlgItem(dialog, IDC_MIC), SW_HIDE);
        else { d->voice.dialog = dialog; d->voice.text_id = IDC_PROMPT; d->voice.note = voice_new(dialog_voice_changed, &d->voice); }
        if (store_supports("runtimes")) {
            // Without the catalog the server still starts on the project's configured runtime.
            char *key = xstrfmt("runtimes:%s", d->project->repo);
            Json *saved = cache_value(g_store.cache, key);
            if (saved) { show_catalog(d, saved); json_free(saved); }
            free(key);
            Json *args = json_object(); json_set_str(args, "repo", d->project->repo);
            store_call("runtimes", args, 0, d, runtimes_done, 0, &d->req_runtimes);
        }
        if (store_supports("branches")) {
            char *key = xstrfmt("branches:%s", d->project->repo);
            Json *saved = cache_value(g_store.cache, key);
            if (saved) { show_branches(d, saved); json_free(saved); }
            free(key);
            Json *args = json_object(); json_set_str(args, "repo", d->project->repo);
            store_call("branches", args, 0, d, branches_done, 0, &d->req_branches);
        }
        update_start(d);
        SetFocus(GetDlgItem(dialog, IDC_PROMPT));
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_ERROR) return dialog_static_color(dialog, wp, lp, theme.danger);
        if (id == IDC_PROJECT_NAME || id == IDC_BRANCH_LABEL || id == IDC_MODEL_LABEL || id == IDC_EFFORT_LABEL || id == IDC_NOTE || id == IDC_UNCERTAIN || id == IDC_RECORDING) return dialog_static_color(dialog, wp, lp, theme.secondary);
        return dialog_ctl_color(dialog, msg, wp, lp);
    }
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_PROMPT }; dialog_paint_frames(dialog, ids, 1); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDC_START) ? TRUE : FALSE;
    case WM_TIMER: if (wp == 7 && d) dialog_voice_tick(&d->voice); return TRUE;
    case WM_COMMAND:
        if (!d) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_PROMPT:
            if (HIWORD(wp) == EN_CHANGE) update_start(d);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, IDC_PROMPT);
            return TRUE;
        case IDC_MODEL: if (HIWORD(wp) == CBN_SELCHANGE) model_changed(d); return TRUE;
        case IDC_EFFORT: if (HIWORD(wp) == CBN_SELCHANGE) effort_changed(d); return TRUE;
        case IDC_MIC: dialog_voice_toggle(&d->voice); return TRUE;
        case IDC_START: start(d); return TRUE;
        case IDC_RETURN: EndDialog(dialog, IDCANCEL); return TRUE;
        case IDCANCEL: if (!d->busy) EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: if (d && !d->busy) EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_new_conversation(HWND owner, const Project *project, Session *started) {
    NewConversation d; memset(&d, 0, sizeof d);
    d.project = project; d.started = started;
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_NEW_CONVERSATION), owner, new_conversation_proc, (LPARAM)&d);
    request_cancel(&d.req_runtimes); request_cancel(&d.req_branches); request_cancel(&d.req_start);
    if (d.voice.note) voice_free(d.voice.note);
    if (d.has_catalog) runtime_catalog_free(&d.catalog);
    if (d.has_runtime) runtime_choice_free(&d.runtime);
    str_array_free(d.branches, d.branch_count); free(d.default_branch);
    for (size_t i = 0; i < d.row_count; i++) free(d.rows[i].model);
    free(d.rows);
    return result == IDOK;
}

// MARK: - Rename

typedef struct { const char *caption, *label, *ok_label, *current; char *result; } RenameState;
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
    RenameState r = { caption, label, ok_label, current ? current : "", NULL };
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_RENAME), owner, rename_proc, (LPARAM)&r) != IDOK) { free(r.result); return NULL; }
    return r.result;
}
char *dialog_rename(HWND owner, const char *current) { return dialog_text(owner, "Rename conversation", "Title", "Save", current); }

// MARK: - Action input

typedef struct { const BoardAction *action; int number; char *input; HWND dialog; DialogVoice voice; } InputState;
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
        if (!store_can_transcribe()) ShowWindow(GetDlgItem(dialog, IDC_MIC), SW_HIDE);
        else { s->voice.dialog = dialog; s->voice.text_id = IDC_INPUT; s->voice.note = voice_new(dialog_voice_changed, &s->voice); }
        input_update(s);
        SetFocus(GetDlgItem(dialog, IDC_INPUT));
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: return dialog_static_color(dialog, wp, lp, GetDlgCtrlID((HWND)lp) == IDC_HINT ? theme.text : theme.secondary);
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: return dialog_ctl_color(dialog, msg, wp, lp);
    case WM_PAINT: { static const int ids[] = { IDC_INPUT }; dialog_paint_frames(dialog, ids, 1); return TRUE; }
    case WM_DRAWITEM: return dialog_draw_item(wp, lp, IDC_START) ? TRUE : FALSE;
    case WM_TIMER: if (wp == 7 && s) dialog_voice_tick(&s->voice); return TRUE;
    case WM_COMMAND:
        if (!s) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_INPUT:
            if (HIWORD(wp) == EN_CHANGE) input_update(s);
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) dialog_invalidate_frame(dialog, IDC_INPUT);
            return TRUE;
        case IDC_MIC: dialog_voice_toggle(&s->voice); return TRUE;
        case IDC_START: { char *text = control_text(dialog, IDC_INPUT); s->input = str_trim(text); free(text); EndDialog(dialog, IDOK); return TRUE; }
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
bool dialog_action_input(HWND owner, const BoardAction *action, int number, char **input) {
    InputState s; memset(&s, 0, sizeof s);
    s.action = action; s.number = number;
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ACTION_INPUT), owner, input_proc, (LPARAM)&s);
    if (s.voice.note) voice_free(s.voice.note);
    if (result != IDOK) { free(s.input); return false; }
    *input = s.input ? s.input : xstrdup("");
    return true;
}
