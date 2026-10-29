#include "action_runtime.h"
#include "dialogs.h"
#include "screens.h"
#include "resource.h"
#include "str.h"
#include <stdlib.h>

typedef struct {
    HWND window, source_window; Pane *pane; Screen *source;
    ActionRuntime runtime; RuntimeChoice *out;
} ActionDialog;
static bool source_current(ActionDialog *d) {
    return IsWindow(d->source_window) && pane_top(d->pane) == d->source;
}
static void add(HWND window, int control, const char *text) {
    wchar_t *w = utf8_to_wide(text); SendDlgItemMessageW(window, control, CB_ADDSTRING, 0, (LPARAM)w); free(w);
}
static void efforts(ActionDialog *d) {
    SendDlgItemMessageW(d->window, IDC_EFFORT, CB_RESETCONTENT, 0, 0);
    size_t n; const char *const *offered = runtime_catalog_efforts(&d->runtime.catalog, &d->runtime.choice, &n);
    for (size_t i = 0; i < n; i++) {
        add(d->window, IDC_EFFORT, offered[i]);
        if (str_eq(offered[i], d->runtime.choice.effort)) SendDlgItemMessageW(d->window, IDC_EFFORT, CB_SETCURSEL, i, 0);
    }
    EnableWindow(GetDlgItem(d->window, IDC_EFFORT), n != 0);
}
static void loaded(void *ctx) {
    ActionDialog *d = ctx;
    if (!source_current(d)) { EndDialog(d->window, IDCANCEL); return; }
    const char *note = d->runtime.error ? d->runtime.error : "Starts a paid agent on the PR head. Published comments remain on the pull request after the review session is deleted.";
    wchar_t *w = utf8_to_wide(note); SetDlgItemTextW(d->window, IDC_NOTE, w); free(w);
    SendDlgItemMessageW(d->window, IDC_MODEL, CB_RESETCONTENT, 0, 0);
    int row = 0;
    for (size_t p = 0; p < d->runtime.catalog.provider_count; p++) {
        const RuntimeProvider *provider = &d->runtime.catalog.providers[p];
        for (size_t m = 0; m < provider->model_count; m++, row++) {
            const RuntimeModel *model = &provider->models[m];
            char *label = xstrfmt("%s · %s", provider->label, runtime_model_title(model)); add(d->window, IDC_MODEL, label); free(label);
            if (provider->id == d->runtime.choice.provider_id && str_eq(model->id, d->runtime.choice.model))
                SendDlgItemMessageW(d->window, IDC_MODEL, CB_SETCURSEL, row, 0);
        }
    }
    efforts(d); EnableWindow(GetDlgItem(d->window, IDC_START), d->runtime.ready);
}
static void select_model(ActionDialog *d) {
    int selected = (int)SendDlgItemMessageW(d->window, IDC_MODEL, CB_GETCURSEL, 0, 0), row = 0;
    for (size_t p = 0; p < d->runtime.catalog.provider_count; p++) {
        const RuntimeProvider *provider = &d->runtime.catalog.providers[p];
        for (size_t m = 0; m < provider->model_count; m++, row++) if (row == selected) {
            action_runtime_select(&d->runtime, provider->id, provider->models[m].id, NULL); efforts(d); return;
        }
    }
}
static INT_PTR CALLBACK action_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    ActionDialog *d = (ActionDialog *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        d = (ActionDialog *)lp; d->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, lp);
        dialog_theme(window); EnableWindow(GetDlgItem(window, IDC_START), FALSE);
        action_runtime_load(&d->runtime); return TRUE;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        return dialog_ctl_color(window, msg, wp, lp);
    case WM_COMMAND:
        if (!d) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_MODEL: if (HIWORD(wp) == CBN_SELCHANGE) select_model(d); return TRUE;
        case IDC_EFFORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                size_t n; const char *const *offered = runtime_catalog_efforts(&d->runtime.catalog, &d->runtime.choice, &n);
                size_t row = (size_t)SendDlgItemMessageW(window, IDC_EFFORT, CB_GETCURSEL, 0, 0);
                if (row < n) action_runtime_select(&d->runtime, d->runtime.choice.provider_id, d->runtime.choice.model, offered[row]);
            }
            return TRUE;
        case IDC_START:
            if (!source_current(d) || !action_runtime_current(&d->runtime)) { EndDialog(window, IDCANCEL); return TRUE; }
            if (action_runtime_take(&d->runtime, d->out)) EndDialog(window, IDOK);
            return TRUE;
        case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_CLOSE: EndDialog(window, IDCANCEL); return TRUE;
    case WM_DESTROY: if (d) request_cancel(&d->runtime.read); return TRUE;
    }
    return FALSE;
}
bool dialog_action_runtime(HWND owner, Screen *source, const char *repo, const char *binary, RuntimeChoice *out) {
    ActionDialog d = {0}; d.source = source; d.pane = source->pane; d.source_window = pane_hwnd(d.pane); d.out = out;
    action_runtime_init(&d.runtime, repo, binary, loaded, &d);
    if (owner) app_modal_begin(owner);
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ACTION_RUNTIME), owner, action_proc, (LPARAM)&d);
    if (owner) app_modal_end();
    action_runtime_free(&d.runtime); return result == IDOK;
}
