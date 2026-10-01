// Pairing with a server, and the empty right-hand side.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_CONNECT = 1000 };
enum { ID_SERVER = 101, ID_TOKEN = 102 };

typedef struct {
    Screen base;
    HWND server, token;
    RECT server_rc, token_rc;   // content coordinates from the last layout
    bool placed;
} PairingScreen;

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w);
    free(w);
    return text;
}

static bool can_connect(PairingScreen *s) {
    if (g_store.connecting) return false;
    char *server = edit_text(s->server), *token = edit_text(s->token);
    bool ok = !str_empty(server) && !str_empty(token);
    free(server); free(token);
    return ok;
}

static void pairing_connect(PairingScreen *s) {
    if (!can_connect(s)) return;
    char *server = edit_text(s->server), *token = edit_text(s->token);
    store_connect(server, token);
    free(server); free(token);
    pane_relayout(s->base.pane);
}

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    PairingScreen *s = (PairingScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (id == ID_SERVER) SetFocus(s->token); else pairing_connect(s);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, edit_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void pairing_destroy(Screen *base) {
    PairingScreen *s = (PairingScreen *)base;
    if (s->server) DestroyWindow(s->server);
    if (s->token) DestroyWindow(s->token);
    screen_release(base);
}

static void paint_logo(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    COLORREF fill = blend(theme.accent, theme.background, 0.14);
    fill_round_rect(hdc, rc, px(14), fill, fill);
    draw_glyph(hdc, 0xE81E, rc, FONT_ICON_HUGE, theme.accent);
}
static void paint_field_glyph(Doc *doc, Item *it, HDC hdc, const RECT *rc) { draw_glyph(hdc, (wchar_t)it->arg, rc, FONT_ICON, theme.secondary); }

static void pairing_layout(Screen *base, Doc *doc) {
    PairingScreen *s = (PairingScreen *)base;
    int width = doc->width;
    int col = width < px(520) ? width : px(520);
    int x = (width - col) / 2;
    doc_space(doc, px(48));
    RECT logo = { x, doc->y, x + px(52), doc->y + px(52) };
    doc_add(doc, &logo, paint_logo);
    doc->y += px(52) + px(14);
    doc_text(doc, x, col, "Connect to Briareus", FONT_LARGE_TITLE, theme.text, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(24));
    int box = doc_box_begin(doc, x, col, 0, theme.elevated, theme.border, px(18));
    doc_item(doc, box)->hover_fill = false;
    int row_h = px(48);
    RECT g1 = { x + px(14), doc->y + (row_h - px(20)) / 2, x + px(14) + px(20), doc->y + (row_h + px(20)) / 2 };
    int gi = doc_add(doc, &g1, paint_field_glyph); doc_item(doc, gi)->arg = 0xE774;
    RECT e1 = { x + px(44), doc->y + (row_h - px(20)) / 2, x + col - px(14), doc->y + (row_h + px(20)) / 2 };
    s->server_rc = e1;
    doc->y += row_h;
    doc_rule(doc, x + px(44), col - px(44));
    RECT g2 = { x + px(14), doc->y + (row_h - px(20)) / 2, x + px(14) + px(20), doc->y + (row_h + px(20)) / 2 };
    gi = doc_add(doc, &g2, paint_field_glyph); doc_item(doc, gi)->arg = 0xE8D7;
    RECT e2 = { x + px(44), doc->y + (row_h - px(20)) / 2, x + col - px(14), doc->y + (row_h + px(20)) / 2 };
    s->token_rc = e2;
    doc->y += row_h;
    doc_box_end(doc, box, 0);
    doc_space(doc, px(8));
    doc_text(doc, x + px(4), col - px(8), "Create a token on the web dashboard under Settings \xE2\x86\x92 Mobile devices.", FONT_FOOTNOTE, theme.secondary, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(24));
    bool enabled = can_connect(s);
    doc_button(doc, x, col, g_store.connecting ? "Connecting\xE2\x80\xA6" : "Connect", BUTTON_PROMINENT, ACT_CONNECT, 0, enabled);
    if (g_store.connection_error) { doc_space(doc, px(16)); doc_notice_box(doc, x, col, g_store.connection_error); }
    doc_space(doc, px(40));
}

static void pairing_place(Screen *base, const RECT *content, int scroll_y) {
    PairingScreen *s = (PairingScreen *)base;
    int m = px(16);
    MoveWindow(s->server, content->left + m + s->server_rc.left, content->top + s->server_rc.top - scroll_y, s->server_rc.right - s->server_rc.left, s->server_rc.bottom - s->server_rc.top, TRUE);
    MoveWindow(s->token, content->left + m + s->token_rc.left, content->top + s->token_rc.top - scroll_y, s->token_rc.right - s->token_rc.left, s->token_rc.bottom - s->token_rc.top, TRUE);
    if (!s->placed) { s->placed = true; SetFocus(str_empty(g_store.server) ? s->server : s->token); }
}

static void pairing_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }
static void pairing_action(Screen *base, int action, intptr_t arg, POINT pt) { (void)arg; (void)pt; if (action == ACT_CONNECT) pairing_connect((PairingScreen *)base); }
static void pairing_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    if ((id == ID_SERVER || id == ID_TOKEN) && code == EN_CHANGE) pane_relayout(base->pane);
}
static void pairing_visible(Screen *base, bool shown) {
    PairingScreen *s = (PairingScreen *)base;
    if (!s->server) return;
    ShowWindow(s->server, shown ? SW_SHOW : SW_HIDE); ShowWindow(s->token, shown ? SW_SHOW : SW_HIDE);
}

/// The fields are created once the pane is known: a classic Edit control notifies the parent it was created with,
/// so they cannot be made first and re-parented later.
static void pairing_ensure_controls(PairingScreen *s) {
    if (s->server) return;
    HWND owner = pane_hwnd(s->base.pane);
    s->server = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)ID_SERVER, GetModuleHandleW(NULL), NULL);
    s->token = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)ID_TOKEN, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->server, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->token, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->server, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://briareus.example.com");
    SendMessageW(s->token, EM_SETCUEBANNER, TRUE, (LPARAM)L"Device token");
    wchar_t *server = utf8_to_wide(g_store.server ? g_store.server : "");
    SetWindowTextW(s->server, server); free(server);
    SetWindowSubclass(s->server, edit_proc, ID_SERVER, (DWORD_PTR)s);
    SetWindowSubclass(s->token, edit_proc, ID_TOKEN, (DWORD_PTR)s);
    theme_apply_control(s->server); theme_apply_control(s->token);
}
static void pairing_visible_creating(Screen *base, bool shown) { if (shown) pairing_ensure_controls((PairingScreen *)base); pairing_visible(base, shown); }
static const ScreenVTable pairing_vt = {
    .destroy = pairing_destroy, .layout = pairing_layout, .header = pairing_header, .action = pairing_action,
    .place = pairing_place, .visible = pairing_visible_creating, .command = pairing_command,
};
Screen *pairing_screen_new(void) {
    PairingScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pairing_vt; s->base.id = xstrdup("pairing");
    return &s->base;
}

// MARK: - Placeholder

/// The dashboard opens on Welcome back and its composer; so does the empty right-hand side here.
Screen *placeholder_screen_new(void) { return new_session_screen_new(NULL, NULL, 0); }
