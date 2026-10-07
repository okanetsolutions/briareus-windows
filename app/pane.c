#include "pane.h"
#include "canvas.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>
#include <commctrl.h>

#define PANE_CLASS L"BriareusPane"
// The window button's action, below any a screen uses.
#define ACTION_MOVE_WINDOW (-0x4D57)

struct Pane {
    HWND hwnd;
    bool sidebar;
    Screen **stack; size_t depth, cap;
    Doc doc;
    bool dirty;
    int scroll_y, scroll_x, content_height;
    bool stick_bottom, show_bottom_button;
    int header_h, footer_h;
    HeaderInfo header;
    RECT button_rects[HEADER_BUTTONS]; RECT back_rect; RECT bottom_button_rect;
    int hover_button;       // -1 none, -2 back, -3 bottom button, -4 the title's ✎, 0...7 header buttons
    RECT title_action_rect;
    int pressed_button;
    bool root_back; void (*root_back_cb)(void *); void *root_back_ctx;
    bool overlay;           // a side panel over the main column: ✕ at its root, a line down its left edge
    bool titles_window; char *window_title;   // the title last given the window the pane fills
    wchar_t move_glyph; const char *move_tip; void (*move_cb)(void *); void *move_ctx;   // a detachable root's window button
    char *selected_id;
    bool tracking;
    HBRUSH edit_brush;
    // scrollbar drag
    bool dragging_thumb; int drag_offset;
    bool dragging_hthumb;   // the sideways bar at the bottom, while the content is wider than the pane
    int dragging_region;    // a document region's own bar (a board's column), or -1
    bool dragging_footer;   // the top screen's footer_drag follows the mouse
    bool carrying;          // the pressed item, laid out with `drag`, follows the mouse
    POINT carry_from, carry_grab, carry_at;   // the press and the mouse now (client), and where on the item it took it
    RECT thumb_rect, hthumb_rect;
    Canvas *canvas;         // Direct2D, drawing to the window on the GPU
    HWND tip;               // the tooltip of the hovered item's `tip`, created on first use
    int tip_item;           // the item it shows for, or -1
};

static Pane **all_panes; static size_t pane_count;

// The dashboard's own padding: the sidebar is `p-2.5` (10px), the main column's lists are `px-[18px]`.
static int margin(Pane *p) { return px(p->sidebar ? 10 : 18); }
static COLORREF pane_bg(Pane *p) { return p->sidebar ? theme.sidebar : theme.canvas; }

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void register_class(void) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = pane_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = PANE_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
}

Pane *pane_create(HWND parent, bool sidebar) {
    register_class();
    Pane *p = xcalloc(1, sizeof *p);
    p->sidebar = sidebar; p->hover_button = -1; p->pressed_button = -1; p->tip_item = -1; p->dragging_region = -1;
    doc_init(&p->doc);
    // WS_CLIPSIBLINGS: a side panel's pane lies over the detail's, which must not paint through it.
    p->hwnd = CreateWindowExW(0, PANE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), p);
    p->canvas = canvas_for_window(p->hwnd);
    all_panes = xrealloc(all_panes, (pane_count + 1) * sizeof *all_panes);
    all_panes[pane_count++] = p;
    return p;
}
void pane_destroy(Pane *p) {
    if (!p) return;
    pane_set_root(p, NULL);
    for (size_t i = 0; i < pane_count; i++) if (all_panes[i] == p) { all_panes[i] = all_panes[--pane_count]; break; }
    if (p->hwnd) DestroyWindow(p->hwnd);
    doc_free(&p->doc); free(p->stack); free(p->selected_id); free(p->window_title);
    if (p->edit_brush) DeleteObject(p->edit_brush);
    canvas_free(p->canvas);
    free(p);
}
HWND pane_hwnd(Pane *p) { return p->hwnd; }
void pane_set_bounds(Pane *p, const RECT *rc) { MoveWindow(p->hwnd, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE); p->dirty = true; }
void pane_show(Pane *p, bool shown) { ShowWindow(p->hwnd, shown ? SW_SHOW : SW_HIDE); }
bool pane_is_sidebar(Pane *p) { return p->sidebar; }

void screen_release(Screen *s) { if (!s) return; free(s->id); free(s); }

static void set_visible(Screen *s, bool shown) { if (s && s->vt->visible) s->vt->visible(s, shown); }

void pane_push(Pane *p, Screen *s) {
    if (p->depth) set_visible(p->stack[p->depth - 1], false);
    if (p->depth == p->cap) { p->cap = p->cap ? p->cap * 2 : 8; p->stack = xrealloc(p->stack, p->cap * sizeof *p->stack); }
    s->pane = p;
    p->stack[p->depth++] = s;
    p->scroll_y = 0; p->scroll_x = 0; p->stick_bottom = false; p->show_bottom_button = false;
    doc_free(&p->doc); doc_init(&p->doc);
    set_visible(s, true);
    pane_relayout(p);
}
static void destroy_top(Pane *p) {
    Screen *s = p->stack[--p->depth];
    set_visible(s, false);
    // Timers a screen forgot die with it.
    s->vt->destroy(s);
}
void pane_pop(Pane *p) {
    if (p->depth <= 1) return;
    destroy_top(p);
    p->scroll_y = 0; p->stick_bottom = false; p->show_bottom_button = false;
    doc_free(&p->doc); doc_init(&p->doc);
    set_visible(p->stack[p->depth - 1], true);
    pane_relayout(p);
}
void pane_pop_to_root(Pane *p) { while (p->depth > 1) destroy_top(p); if (p->depth) { set_visible(p->stack[0], true); pane_relayout(p); } }
Screen *pane_take_root(Pane *p) {
    if (p->depth != 1) return NULL;
    Screen *s = p->stack[0];
    set_visible(s, false);
    p->depth = 0;
    s->pane = NULL;
    p->scroll_y = 0; p->scroll_x = 0; p->stick_bottom = false; p->show_bottom_button = false;
    doc_free(&p->doc); doc_init(&p->doc);
    pane_relayout(p);
    return s;
}
void pane_set_root(Pane *p, Screen *s) {
    while (p->depth) destroy_top(p);
    p->scroll_y = 0; p->scroll_x = 0; p->stick_bottom = false; p->show_bottom_button = false;
    doc_free(&p->doc); doc_init(&p->doc);
    if (s) pane_push(p, s); else pane_relayout(p);
}
Screen *pane_top(Pane *p) { return p->depth ? p->stack[p->depth - 1] : NULL; }
Screen *pane_root(Pane *p) { return p->depth ? p->stack[0] : NULL; }
size_t pane_depth(Pane *p) { return p->depth; }

void pane_relayout(Pane *p) { p->dirty = true; InvalidateRect(p->hwnd, NULL, FALSE); }
void pane_repaint(Pane *p) { InvalidateRect(p->hwnd, NULL, FALSE); }
static void refresh_header(Pane *p);
/// The screen's header is asked again (a button enabled, a subtitle changed); a new height lays the pane out again.
void pane_header_changed(Pane *p) {
    int height = p->header_h;
    refresh_header(p);
    if (p->header_h != height) { pane_relayout(p); return; }
    RECT rc = { 0, 0, 10000, p->header_h };
    InvalidateRect(p->hwnd, &rc, FALSE);
}
void pane_footer_changed(Pane *p) { p->dirty = true; InvalidateRect(p->hwnd, NULL, FALSE); }
void pane_place_control(HWND control, const RECT *rc) {
    RECT now; GetWindowRect(control, &now); MapWindowPoints(NULL, GetParent(control), (POINT *)&now, 2);
    bool moved = !EqualRect(&now, rc), shown = IsWindowVisible(control);
    if (moved) SetWindowPos(control, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (!shown) ShowWindow(control, SW_SHOW);
    if (moved || !shown) RedrawWindow(control, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

static RECT client(Pane *p) { RECT rc; GetClientRect(p->hwnd, &rc); return rc; }
RECT pane_content_rect(Pane *p) {
    RECT rc = client(p);
    rc.top += p->header_h; rc.bottom -= p->footer_h;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}
int pane_content_width(Pane *p) { RECT rc = client(p); return rc.right - rc.left - 2 * margin(p); }
int pane_scroll_y(Pane *p) { return p->scroll_y; }
int pane_scroll_x(Pane *p) { return p->scroll_x; }

static int max_scroll(Pane *p) {
    RECT rc = pane_content_rect(p);
    int visible = rc.bottom - rc.top;
    int m = p->content_height + px(12) - visible;
    return m > 0 ? m : 0;
}
bool pane_at_bottom(Pane *p) { return p->scroll_y >= max_scroll(p) - px(24); }
static int max_scroll_x(Pane *p) { int m = p->doc.content_width - pane_content_width(p); return m > 0 ? m : 0; }
static void set_scroll_x(Pane *p, int x) {
    int m = max_scroll_x(p);
    if (x > m) x = m;
    if (x < 0) x = 0;
    if (x == p->scroll_x) return;
    p->scroll_x = x;
    // A screen's controls sit over the content, so they move with it.
    Screen *s = pane_top(p);
    if (s && s->vt->place) { RECT rc = pane_content_rect(p); s->vt->place(s, &rc, p->scroll_y); }
    InvalidateRect(p->hwnd, NULL, FALSE);
}

// MARK: - Tooltip

static void hide_tip(Pane *p) {
    if (p->tip_item < 0) return;
    p->tip_item = -1;
    TTTOOLINFOW ti = { TTTOOLINFOW_V2_SIZE, 0, p->hwnd, 1 };
    SendMessageW(p->tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}
/// Shows the hovered item's tip under it, or hides the tooltip when it has none.
static void update_tip(Pane *p) {
    Item *it = doc_item(&p->doc, p->doc.hover);
    if (!it || !it->tip) { hide_tip(p); return; }
    if (p->tip_item == p->doc.hover) return;
    TTTOOLINFOW ti = { TTTOOLINFOW_V2_SIZE, TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT, p->hwnd, 1 };
    if (!p->tip) {
        p->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 0, 0, 0, 0, p->hwnd, NULL, GetModuleHandleW(NULL), NULL);
        theme_apply_control(p->tip);
        ti.lpszText = L"";
        SendMessageW(p->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(p->tip, TTM_SETMAXTIPWIDTH, 0, px(360));
    }
    p->tip_item = p->doc.hover;
    wchar_t *text = utf8_to_wide(it->tip);
    ti.lpszText = text;
    SendMessageW(p->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    free(text);
    RECT content = pane_content_rect(p);
    POINT at = { it->rc.left + margin(p) - p->scroll_x, it->rc.bottom + content.top - p->scroll_y + px(4) };
    ClientToScreen(p->hwnd, &at);
    SendMessageW(p->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(at.x, at.y));
    SendMessageW(p->tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

static void after_scroll(Pane *p) {
    hide_tip(p);
    Screen *s = pane_top(p);
    RECT rc = pane_content_rect(p);
    doc_set_view(&p->doc, p->scroll_y, rc.bottom - rc.top);
    if (s && s->vt->place) s->vt->place(s, &rc, p->scroll_y);
    if (s && s->vt->scrolled) s->vt->scrolled(s, pane_at_bottom(p));
    InvalidateRect(p->hwnd, NULL, FALSE);
}
static void set_scroll(Pane *p, int y) {
    int m = max_scroll(p);
    if (y > m) y = m;
    if (y < 0) y = 0;
    if (y == p->scroll_y) return;
    p->scroll_y = y;
    after_scroll(p);
}
void pane_scroll_to_bottom(Pane *p) { p->stick_bottom = p->stick_bottom || false; set_scroll(p, max_scroll(p)); if (p->dirty) p->scroll_y = 0x3fffffff; }
void pane_scroll_to_top(Pane *p) { set_scroll(p, 0); }
void pane_scroll_to(Pane *p, int content_y) { set_scroll(p, content_y - px(8)); }
void pane_stick_to_bottom(Pane *p, bool stick) { p->stick_bottom = stick; }
void pane_show_bottom_button(Pane *p, bool show) { if (p->show_bottom_button != show) { p->show_bottom_button = show; InvalidateRect(p->hwnd, NULL, FALSE); } }
void pane_set_root_back(Pane *p, bool show, void (*callback)(void *), void *ctx) { p->root_back = show; p->root_back_cb = callback; p->root_back_ctx = ctx; pane_relayout(p); }
void pane_set_move_button(Pane *p, wchar_t glyph, const char *tip, void (*move)(void *), void *ctx) {
    p->move_glyph = glyph; p->move_tip = tip; p->move_cb = move; p->move_ctx = ctx;
    pane_header_changed(p);
}
void pane_set_titles_window(Pane *p) { p->titles_window = true; pane_header_changed(p); }
void pane_set_overlay(Pane *p, void (*close)(void *), void *ctx) { p->overlay = true; pane_set_root_back(p, true, close, ctx); }
void pane_set_selected_id(Pane *p, const char *id) {
    if (str_eq(p->selected_id, id)) return;
    free(p->selected_id); p->selected_id = xstrdup(id);
    pane_relayout(p);
}
const char *pane_selected_id(Pane *p) { return p->selected_id; }
void pane_activate_all(bool active) {
    for (size_t i = 0; i < pane_count; i++) {
        Screen *s = pane_top(all_panes[i]);
        if (s && s->vt->activated) s->vt->activated(s, active);
    }
}

// MARK: - Layout and paint

static bool shows_back(Pane *p) { return p->depth > 1 || p->root_back; }

static void refresh_header(Pane *p) {
    Screen *s = pane_top(p);
    memset(&p->header, 0, sizeof p->header);
    if (s && s->vt->header) s->vt->header(s, &p->header);
    if (p->move_cb && s && p->depth == 1 && s->vt->detachable && p->header.button_count < HEADER_BUTTONS) {
        HeaderButton *b = &p->header.buttons[p->header.button_count++];
        b->glyph = p->move_glyph; b->action = ACTION_MOVE_WINDOW; b->enabled = true; b->tip = p->move_tip;
    }
    if (p->titles_window && p->header.title[0] && !str_eq(p->header.title, p->window_title)) {
        free(p->window_title); p->window_title = xstrdup(p->header.title);
        char *title = xstrfmt("%s \xC2\xB7 Briareus", p->header.title);
        wchar_t *w = utf8_to_wide(title);
        SetWindowTextW(GetAncestor(p->hwnd, GA_ROOT), w);
        free(w); free(title);
    }
    bool has_header = s && (p->header.title[0] || p->header.button_count || (shows_back(p) && !p->sidebar));
    // `px-[18px] py-2.5` around a 15px title (22px line) and a 13px subtitle (18px line), or 30px buttons, plus the border.
    int content = p->header.subtitle[0] ? px(40) : (p->header.button_count || shows_back(p)) ? px(32) : px(22);
    p->header_h = has_header ? content + px(20) + 1 : 0;
}

static void end_carry(Pane *p, POINT at, DragPhase phase);
static void layout_if_needed(Pane *p, Canvas *cv) {
    if (!p->dirty) return;
    p->dirty = false;
    // The carried item is laid out again, perhaps elsewhere: it goes back.
    if (p->carrying) end_carry(p, p->carry_at, DRAG_CANCEL);
    Screen *s = pane_top(p);
    refresh_header(p);
    RECT rc = client(p);
    int width = rc.right - rc.left;
    p->footer_h = s && s->vt->footer_height ? s->vt->footer_height(s, width) : 0;
    bool was_bottom = p->scroll_y >= 0x3fffffff || pane_at_bottom(p);
    int content_width = width - 2 * margin(p);
    if (content_width < px(120)) content_width = px(120);
    hide_tip(p);   // the items move; the next mouse move shows it again
    doc_begin(&p->doc, cv, content_width);
    if (s && s->vt->layout) s->vt->layout(s, &p->doc);
    doc_end(&p->doc);
    p->content_height = doc_height(&p->doc);
    // The sidebar's padding is no wider than its scrollbar, so once it scrolls the bar takes its own 10px of
    // width, as `::-webkit-scrollbar` does, instead of covering the ⚑ badge and the rows' edges.
    if (p->sidebar && max_scroll(p) > 0) {
        content_width -= px(10);
        doc_begin(&p->doc, cv, content_width);
        if (s && s->vt->layout) s->vt->layout(s, &p->doc);
        doc_end(&p->doc);
        p->content_height = doc_height(&p->doc);
    }
    int m = max_scroll(p);
    if ((p->stick_bottom && was_bottom) || p->scroll_y >= 0x3fffffff) p->scroll_y = m;
    if (p->scroll_y > m) p->scroll_y = m;
    if (p->scroll_y < 0) p->scroll_y = 0;
    int max_x = p->doc.content_width - content_width; if (max_x < 0) max_x = 0;
    if (p->scroll_x > max_x) p->scroll_x = max_x;
    RECT content = pane_content_rect(p);
    doc_set_view(&p->doc, p->scroll_y, content.bottom - content.top);
    if (s && s->vt->footer_layout) { RECT fr = { rc.left, content.bottom, rc.right, rc.bottom }; s->vt->footer_layout(s, &fr); }
    if (s && s->vt->place) s->vt->place(s, &content, p->scroll_y);
    if (s && s->vt->scrolled) s->vt->scrolled(s, pane_at_bottom(p));
}

static int header_button_width(Canvas *cv, const HeaderButton *b, bool labels) {
    if (!b->label[0] || !labels) return px(32);
    return px(10) * 2 + text_width(cv, b->label, FONT_FOOTNOTE) + 2;
}
/// A `.btn-icon`: a 32px square with a border, as the dashboard's ☰ ＋ ⓘ and ⟳.
static void paint_icon_button(Canvas *cv, const RECT *rc, bool hovered) {
    fill_round_rect(cv, rc, px(8), theme.raise, hovered ? theme.accent_dim : theme.line);
}
static void paint_header(Pane *p, Canvas *cv, const RECT *rc) {
    if (!p->header_h) return;
    RECT hr = { rc->left, rc->top, rc->right, rc->top + p->header_h };
    fill_rect(cv, &hr, pane_bg(p));
    draw_line(cv, hr.left, hr.bottom - 1, hr.right, hr.bottom - 1, theme.line);
    int x = px(18);
    int size = px(32);
    int cy = hr.top + (p->header_h - 1) / 2;
    memset(&p->back_rect, 0, sizeof p->back_rect);
    if (shows_back(p)) {
        RECT br = { x, cy - size / 2, x + size, cy + size / 2 };
        p->back_rect = br;
        paint_icon_button(cv, &br, p->hover_button == -2);
        if (p->overlay && p->depth <= 1) draw_glyph(cv, 0xE711, &br, FONT_ICON_SMALL, theme.ink);   // ✕ closes the side panel
        else draw_text(cv, "\xE2\x80\xB9", &br, FONT_BODY, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += size + px(8);
    }
    int right = hr.right - px(18);
    // The dashboard's `.btn` pills, 8px apart; when they would leave the title no room at all, they fall back to icons.
    int labelled_w = 0;
    for (int i = 0; i < p->header.button_count; i++) labelled_w += header_button_width(cv, &p->header.buttons[i], true) + px(8);
    bool labels = labelled_w <= right - x - px(120);
    for (int i = p->header.button_count - 1; i >= 0; i--) {
        HeaderButton *b = &p->header.buttons[i];
        bool pill = b->label[0] && labels;
        int bw = header_button_width(cv, b, labels), bh = pill ? px(30) : size;
        RECT br = { right - bw, cy - bh / 2, right, cy + bh / 2 };
        p->button_rects[i] = br;
        bool hovered = p->hover_button == i && b->enabled;
        COLORREF border = hovered ? (b->destructive ? theme.danger : theme.accent_dim) : theme.line;
        COLORREF text = !b->enabled ? theme.muted : (hovered && b->destructive) ? theme.danger : theme.ink;
        COLORREF fill = theme.raise;
        if (b->prominent && b->enabled) { fill = hovered ? blend(theme.accent, theme.white, 0.9) : theme.accent; border = fill; text = theme.on_accent; }
        if (pill) {
            fill_round_rect(cv, &br, px(7), fill, border);
            RECT t = { br.left + px(10), br.top, br.right - px(10) + 2, br.bottom };
            draw_text(cv, b->label, &t, FONT_FOOTNOTE, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            fill_round_rect(cv, &br, px(8), fill, border);
            draw_glyph(cv, b->glyph, &br, FONT_ICON_SMALL, text);
        }
        right -= bw + px(8);
    }
    RECT tr = { x, hr.top, right - px(8), hr.bottom - 1 };
    memset(&p->title_action_rect, 0, sizeof p->title_action_rect);
    // `#btn-edit-title`: a ✎ right after the title, 13px muted, ink on hover.
    int pencil_w = p->header.title_action ? px(8) + text_width(cv, "\xE2\x9C\x8E", FONT_FOOTNOTE) + px(8) : 0;
    if (p->header.subtitle[0]) {
        int th = px(22), sh = px(18);
        int top = cy - (th + sh) / 2;
        RECT t1 = { tr.left, top, tr.right - pencil_w, top + th };
        draw_text(cv, p->header.title, &t1, FONT_HEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (pencil_w) {
            int tw = text_width(cv, p->header.title, FONT_HEADLINE);
            int px_ = t1.left + (tw < t1.right - t1.left ? tw : t1.right - t1.left);
            RECT pr = { px_ + px(2), top, px_ + pencil_w, top + th };
            p->title_action_rect = pr;
            draw_text(cv, "\xE2\x9C\x8E", &pr, FONT_FOOTNOTE, p->hover_button == -4 ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT t2 = { tr.left, top + th, tr.right, top + th + sh };
        if (p->header.status[0]) { draw_status_dot(cv, t2.left + px(4), (t2.top + t2.bottom) / 2, p->header.status); t2.left += px(13); }
        draw_text(cv, p->header.subtitle, &t2, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        draw_text(cv, p->header.title, &tr, FONT_HEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

/// The screens' Edit controls follow the palette's mode (their scrollbars, their context menus).
static BOOL CALLBACK retheme_child(HWND child, LPARAM lp) {
    (void)lp;
    wchar_t cls[16]; GetClassNameW(child, cls, 16);
    if (_wcsicmp(cls, L"Edit") == 0) { theme_apply_control(child); InvalidateRect(child, NULL, TRUE); }
    return TRUE;
}

/// The HWND render target presents a new frame even when an Edit has not moved. WS_CLIPCHILDREN keeps the Edit out
/// of the pane's GDI update region, so it needs its own paint after the Direct2D frame has been presented.
static BOOL CALLBACK repaint_edit(HWND child, LPARAM lp) {
    (void)lp;
    wchar_t cls[16]; GetClassNameW(child, cls, 16);
    if (IsWindowVisible(child) && _wcsicmp(cls, L"Edit") == 0)
        RedrawWindow(child, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    return TRUE;
}

static void paint_scrollbar(Pane *p, Canvas *cv, const RECT *content) {
    // `::-webkit-scrollbar { width: 10px }` with the palette's thumb and no track.
    int m = max_scroll(p);
    memset(&p->thumb_rect, 0, sizeof p->thumb_rect);
    if (m <= 0) return;
    int track = content->bottom - content->top;
    int visible = content->bottom - content->top;
    int total = p->content_height + px(12);
    int thumb = total > 0 ? track * visible / total : track;
    if (thumb < px(28)) thumb = px(28);
    int y = content->top + (track - thumb) * p->scroll_y / m;
    RECT r = { content->right - px(10), y, content->right, y + thumb };
    p->thumb_rect = r;
    COLORREF c = p->dragging_thumb ? blend(theme.ink, theme.thumb, 0.15) : theme.thumb;
    fill_round_rect(cv, &r, px(6), c, c);
}

/// The sideways bar along the content's bottom while it is wider than the pane, as the dashboard's board has.
static void paint_hscrollbar(Pane *p, Canvas *cv, const RECT *content) {
    int m = max_scroll_x(p);
    memset(&p->hthumb_rect, 0, sizeof p->hthumb_rect);
    if (m <= 0) return;
    int left = content->left + margin(p), track = content->right - margin(p) - left;
    int total = p->doc.content_width;
    int thumb = total > 0 ? track * (total - m) / total : track;
    if (thumb < px(28)) thumb = px(28);
    int x = left + (track - thumb) * p->scroll_x / m;
    RECT r = { x, content->bottom - px(10), x + thumb, content->bottom - px(2) };
    p->hthumb_rect = r;
    COLORREF c = p->dragging_hthumb ? blend(theme.ink, theme.thumb, 0.15) : theme.thumb;
    fill_round_rect(cv, &r, px(4), c, c);
}

static void paint(Pane *p) {
    RECT rc = client(p);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    Canvas *cv = p->canvas;
    if (!canvas_begin(cv)) return;
    layout_if_needed(p, cv);
    fill_rect(cv, &rc, pane_bg(p));
    Screen *s = pane_top(p);
    RECT content = pane_content_rect(p);
    // Content, clipped.
    canvas_clip(cv, &content);
    canvas_offset(cv, margin(p), content.top);
    RECT local_clip = { content.left - margin(p), 0, content.right - margin(p), content.bottom - content.top };
    doc_paint(&p->doc, cv, p->scroll_x, p->scroll_y, &local_clip);
    if (p->carrying) doc_paint_dragged(&p->doc, cv, p->carry_at.x - p->carry_grab.x - margin(p), p->carry_at.y - p->carry_grab.y - content.top);
    canvas_offset(cv, 0, 0);
    canvas_unclip(cv);
    paint_scrollbar(p, cv, &content);
    paint_hscrollbar(p, cv, &content);
    if (p->show_bottom_button && !pane_at_bottom(p) && p->doc.count) {
        int size = px(32);
        RECT b = { (content.left + content.right) / 2 - size / 2, content.bottom - size - px(10), (content.left + content.right) / 2 + size / 2, content.bottom - px(10) };
        p->bottom_button_rect = b;
        fill_round_rect(cv, &b, px(8), theme.raise, p->hover_button == -3 ? theme.accent_dim : theme.line);
        draw_glyph(cv, 0xE74B, &b, FONT_ICON_SMALL, theme.ink);
    } else memset(&p->bottom_button_rect, 0, sizeof p->bottom_button_rect);
    if (s && s->vt->footer_paint && p->footer_h) { RECT fr = { rc.left, content.bottom, rc.right, rc.bottom }; s->vt->footer_paint(s, cv, &fr); }
    paint_header(p, cv, &rc);
    if (p->overlay) draw_line(cv, rc.left, rc.top, rc.left, rc.bottom, theme.line);
    canvas_end(cv);
}

// MARK: - Mouse

static POINT to_content(Pane *p, int x, int y) {
    RECT content = pane_content_rect(p);
    POINT pt = { x - margin(p) + p->scroll_x, y - content.top + p->scroll_y };
    return pt;
}
static bool in_rect(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static int header_hit(Pane *p, int x, int y) {
    if (shows_back(p) && in_rect(&p->back_rect, x, y)) return -2;
    if (p->header.title_action && in_rect(&p->title_action_rect, x, y)) return -4;
    for (int i = 0; i < p->header.button_count; i++) if (in_rect(&p->button_rects[i], x, y)) return i;
    if (p->show_bottom_button && in_rect(&p->bottom_button_rect, x, y)) return -3;
    return -1;
}

// MARK: - Text selection

enum { TIMER_AUTOSCROLL = 0x7F01 };
enum { MENU_COPY = 1, MENU_COPY_TEXT, MENU_SELECT_ALL };

/// The text position under a client point.
static bool position_at(Pane *p, int x, int y, DocPos *pos) {
    POINT c = to_content(p, x, y);
    return doc_position_at(&p->doc, NULL, c.x, c.y, pos);
}
static void copy_selection(Pane *p) {
    char *text = doc_selection_text(&p->doc);
    if (!text) return;
    copy_to_clipboard(p->hwnd, text);
    free(text);
}
static void select_all(Pane *p) { doc_select_all(&p->doc); InvalidateRect(p->hwnd, NULL, FALSE); }
static void clear_selection(Pane *p) {
    if (!doc_has_selection(&p->doc) && !p->doc.selecting) return;
    doc_clear_selection(&p->doc);
    InvalidateRect(p->hwnd, NULL, FALSE);
}
/// Moves the selection's end to the mouse; past the content's edges the view scrolls, on a timer while the mouse stays there.
static void drag_selection(Pane *p, int x, int y) {
    RECT content = pane_content_rect(p);
    bool outside = y < content.top || y >= content.bottom;
    if (y < content.top) { set_scroll(p, p->scroll_y - px(24)); y = content.top; }
    else if (y >= content.bottom) { set_scroll(p, p->scroll_y + px(24)); y = content.bottom - 1; }
    if (outside) SetTimer(p->hwnd, TIMER_AUTOSCROLL, 60, NULL); else KillTimer(p->hwnd, TIMER_AUTOSCROLL);
    DocPos pos;
    if (position_at(p, x, y, &pos) && (pos.item != p->doc.sel_focus.item || pos.offset != p->doc.sel_focus.offset)) {
        p->doc.sel_focus = pos;
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
}
static void end_selection(Pane *p) {
    p->doc.selecting = false;
    KillTimer(p->hwnd, TIMER_AUTOSCROLL);
    if (GetCapture() == p->hwnd) ReleaseCapture();
    if (!doc_has_selection(&p->doc)) doc_clear_selection(&p->doc);
    InvalidateRect(p->hwnd, NULL, FALSE);
}
/// The copy menu on a right click over text or a selection; returns false when there is neither.
static bool context_menu(Pane *p, int x, int y) {
    POINT c = to_content(p, x, y);
    int text_item = doc_text_item_at(&p->doc, c.x, c.y);
    bool has_sel = doc_has_selection(&p->doc);
    if (!has_sel && text_item < 0) return false;
    POINT sp = { x, y }; ClientToScreen(p->hwnd, &sp);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), MENU_COPY, L"Copy\tCtrl+C");
    if (text_item >= 0) AppendMenuW(menu, MF_STRING, MENU_COPY_TEXT, L"Copy text");
    AppendMenuW(menu, MF_STRING, MENU_SELECT_ALL, L"Select all\tCtrl+A");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, sp.x, sp.y, 0, p->hwnd, NULL);
    DestroyMenu(menu);
    if (chosen == MENU_COPY) copy_selection(p);
    else if (chosen == MENU_COPY_TEXT) { char *t = doc_item_plain_text(&p->doc, text_item); if (t) { copy_to_clipboard(p->hwnd, t); free(t); } }
    else if (chosen == MENU_SELECT_ALL) select_all(p);
    return true;
}

// MARK: - Carrying an item

/// The carried item follows the mouse; near the content's sides the view scrolls sideways, on a timer while it stays.
static void carry_to(Pane *p, int x, int y) {
    p->carry_at.x = x; p->carry_at.y = y;
    RECT content = pane_content_rect(p);
    int edge = px(48);
    bool left = x < content.left + edge, right = x >= content.right - edge;
    if (left) set_scroll_x(p, p->scroll_x - px(24)); else if (right) set_scroll_x(p, p->scroll_x + px(24));
    if (left || right) SetTimer(p->hwnd, TIMER_AUTOSCROLL, 60, NULL); else KillTimer(p->hwnd, TIMER_AUTOSCROLL);
    Screen *s = pane_top(p);
    Item *it = doc_item(&p->doc, p->doc.pressed);
    if (s && it && s->vt->drag) s->vt->drag(s, it->action, it->arg, to_content(p, x, y), DRAG_MOVE);
    InvalidateRect(p->hwnd, NULL, FALSE);
}
/// Starts carrying the pressed item once the mouse has moved past the system's drag distance.
static bool carry_begins(Pane *p, int x, int y) {
    Item *it = doc_item(&p->doc, p->doc.pressed);
    if (!it || !it->drag) return false;
    if (abs(x - p->carry_from.x) <= GetSystemMetrics(SM_CXDRAG) && abs(y - p->carry_from.y) <= GetSystemMetrics(SM_CYDRAG)) return false;
    POINT c = to_content(p, p->carry_from.x, p->carry_from.y);
    p->carry_grab.x = c.x - it->rc.left; p->carry_grab.y = c.y - it->rc.top;
    p->carrying = true;
    hide_tip(p);
    doc_drag(&p->doc, p->doc.pressed);
    return true;
}
/// Lets the carried item go: dropped where the mouse is, or put back.
static void end_carry(Pane *p, POINT at, DragPhase phase) {
    Item *it = doc_item(&p->doc, p->doc.pressed);
    int action = it ? it->action : 0; intptr_t arg = it ? it->arg : 0;
    POINT c = to_content(p, at.x, at.y);
    // Cleared before the capture is let go: its WM_CAPTURECHANGED must find nothing left to cancel.
    p->carrying = false; p->doc.pressed = -1;
    doc_drag(&p->doc, -1);
    KillTimer(p->hwnd, TIMER_AUTOSCROLL);
    if (GetCapture() == p->hwnd) ReleaseCapture();
    Screen *s = pane_top(p);
    if (s && action && s->vt->drag) s->vt->drag(s, action, arg, c, phase);
    InvalidateRect(p->hwnd, NULL, FALSE);
}
void pane_cancel_carry(Pane *p, int action) {
    Item *it = doc_item(&p->doc, p->doc.pressed);
    if (p->carrying && it && it->action == action) end_carry(p, p->carry_at, DRAG_CANCEL);
}

static void mouse_move(Pane *p, int x, int y, bool left_down) {
    if (!p->tracking) { TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, p->hwnd, 0 }; TrackMouseEvent(&tme); p->tracking = true; }
    if (p->doc.selecting) { drag_selection(p, x, y); return; }
    // A press whose capture was lost (Alt+Tab, another window) never sees its button up: with the button
    // no longer held, it must not start a carry.
    if (p->carrying || (p->doc.pressed >= 0 && left_down && carry_begins(p, x, y))) { carry_to(p, x, y); return; }
    if (p->dragging_footer) {
        Screen *s = pane_top(p);
        POINT pt = { x, y };
        if (s && s->vt->footer_drag) s->vt->footer_drag(s, pt);
        return;
    }
    if (p->dragging_hthumb) {
        RECT content = pane_content_rect(p);
        int track = content.right - content.left - 2 * margin(p), thumb = p->hthumb_rect.right - p->hthumb_rect.left;
        if (track - thumb > 0) set_scroll_x(p, (x - p->drag_offset - content.left - margin(p)) * max_scroll_x(p) / (track - thumb));
        return;
    }
    if (p->dragging_region >= 0) {
        POINT c = to_content(p, x, y);
        doc_region_drag(&p->doc, p->dragging_region, c.y - p->drag_offset);
        InvalidateRect(p->hwnd, NULL, FALSE);
        return;
    }
    if (p->dragging_thumb) {
        RECT content = pane_content_rect(p);
        int track = content.bottom - content.top;
        int thumb = p->thumb_rect.bottom - p->thumb_rect.top;
        int m = max_scroll(p);
        if (track - thumb > 0) set_scroll(p, (y - p->drag_offset - content.top) * m / (track - thumb));
        return;
    }
    int button = header_hit(p, x, y);
    int item = -1;
    RECT content = pane_content_rect(p);
    if (button == -1 && in_rect(&content, x, y)) { POINT c = to_content(p, x, y); item = doc_hit(&p->doc, c.x, c.y); }
    if (button != p->hover_button || item != p->doc.hover) {
        p->hover_button = button; p->doc.hover = item;
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
    update_tip(p);
}
static void mouse_down(Pane *p, int x, int y, bool right) {
    SetFocus(p->hwnd);
    if (!right && in_rect(&p->thumb_rect, x, y)) { p->dragging_thumb = true; p->drag_offset = y - p->thumb_rect.top; SetCapture(p->hwnd); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    if (!right && in_rect(&p->hthumb_rect, x, y)) { p->dragging_hthumb = true; p->drag_offset = x - p->hthumb_rect.left; SetCapture(p->hwnd); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    int button = header_hit(p, x, y);
    RECT content = pane_content_rect(p);
    if (!right && button == -1 && in_rect(&content, x, y)) {
        POINT c = to_content(p, x, y);
        int thumb_top, region = doc_region_thumb_at(&p->doc, c.x, c.y, &thumb_top);
        if (region >= 0) { hide_tip(p); p->dragging_region = region; p->drag_offset = c.y - thumb_top; SetCapture(p->hwnd); return; }
    }
    if (button != -1) { p->pressed_button = button; SetCapture(p->hwnd); return; }
    if (in_rect(&content, x, y)) {
        POINT c = to_content(p, x, y);
        int item = doc_hit(&p->doc, c.x, c.y);
        if (right) {
            if (context_menu(p, x, y)) return;
            Screen *s = pane_top(p);
            if (s && s->vt->context) {
                POINT sp = { x, y }; ClientToScreen(p->hwnd, &sp);
                Item *it = doc_item(&p->doc, item);
                s->vt->context(s, it ? it->action : 0, it ? it->arg : (intptr_t)(item), sp);
            }
            return;
        }
        clear_selection(p);
        if (item < 0) {
            // Off any control, the press anchors a text selection that a drag extends.
            DocPos pos;
            if (position_at(p, x, y, &pos)) { p->doc.sel_anchor = p->doc.sel_focus = pos; p->doc.selecting = true; SetCapture(p->hwnd); }
            return;
        }
        p->doc.pressed = item; p->carry_from.x = x; p->carry_from.y = y; SetCapture(p->hwnd); InvalidateRect(p->hwnd, NULL, FALSE);
        return;
    }
    if (!right && p->footer_h && y >= content.bottom) {
        Screen *s = pane_top(p);
        POINT pt = { x, y };
        if (s && s->vt->footer_press && s->vt->footer_press(s, pt)) { p->dragging_footer = true; SetCapture(p->hwnd); return; }
        if (s && s->vt->footer_click) s->vt->footer_click(s, pt);
    }
}
static void mouse_up(Pane *p, int x, int y) {
    if (p->doc.selecting) { end_selection(p); return; }
    if (p->carrying) { POINT at = { x, y }; end_carry(p, at, DRAG_DROP); return; }
    if (p->dragging_thumb) { p->dragging_thumb = false; ReleaseCapture(); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    if (p->dragging_hthumb) { p->dragging_hthumb = false; ReleaseCapture(); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    if (p->dragging_region >= 0) { p->dragging_region = -1; ReleaseCapture(); return; }
    if (p->dragging_footer) { p->dragging_footer = false; ReleaseCapture(); return; }
    if (GetCapture() == p->hwnd) ReleaseCapture();
    Screen *s = pane_top(p);
    POINT sp = { x, y }; ClientToScreen(p->hwnd, &sp);
    if (p->pressed_button != -1) {
        int b = p->pressed_button; p->pressed_button = -1;
        if (header_hit(p, x, y) == b) {
            if (b == -2) { if (p->depth > 1) pane_pop(p); else if (p->root_back_cb) p->root_back_cb(p->root_back_ctx); }
            else if (b == -3) { set_scroll(p, max_scroll(p)); }
            else if (b == -4) { if (s && s->vt->action) s->vt->action(s, p->header.title_action, 0, sp); }
            else if (p->header.buttons[b].action == ACTION_MOVE_WINDOW) { if (p->move_cb) p->move_cb(p->move_ctx); }
            else if (s && p->header.buttons[b].enabled && s->vt->action) s->vt->action(s, p->header.buttons[b].action, 0, sp);
        }
        InvalidateRect(p->hwnd, NULL, FALSE);
        return;
    }
    int pressed = p->doc.pressed; p->doc.pressed = -1;
    if (pressed >= 0) {
        POINT c = to_content(p, x, y);
        int item = doc_hit(&p->doc, c.x, c.y);
        Item *it = doc_item(&p->doc, pressed);
        if (item == pressed && it && s) {
            if (it->action == ACTION_OPEN_LINK) { const char *url = doc_link_at(&p->doc, pressed, c.x, c.y); if (url) open_web_url(url); }
            else if (it->action == ACTION_COPY_CODE) { copy_to_clipboard(p->hwnd, (const char *)it->arg); }
            else if (it->action == ACTION_TIP) { }
            else if (s->vt->action) s->vt->action(s, it->action, it->arg, sp);
        }
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) { CREATESTRUCTW *cs = (CREATESTRUCTW *)lp; SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams); return 0; }
    if (!p) return DefWindowProcW(hwnd, msg, wp, lp);
    Screen *s = pane_top(p);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); paint(p); EndPaint(hwnd, &ps);
        EnumChildWindows(hwnd, repaint_edit, 0);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: canvas_resize(p->canvas, LOWORD(lp), HIWORD(lp)); p->dirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE: mouse_move(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_LBUTTON) != 0); return 0;
    case WM_MOUSELEAVE: p->tracking = false; hide_tip(p); if (p->hover_button != -1 || p->doc.hover != -1) { p->hover_button = -1; p->doc.hover = -1; InvalidateRect(hwnd, NULL, FALSE); } return 0;
    case WM_LBUTTONDOWN: mouse_down(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false); return 0;
    case WM_RBUTTONDOWN: mouse_down(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true); return 0;
    case WM_LBUTTONUP: mouse_up(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    // A footer drag the mouse was taken from (another window, Alt+Tab) ends where it was.
    case WM_CAPTURECHANGED:
        p->dragging_footer = false; p->dragging_hthumb = false; p->dragging_region = -1;
        if (p->carrying) end_carry(p, p->carry_at, DRAG_CANCEL);
        return 0;
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT content = pane_content_rect(p);
        POINT c = to_content(p, x, y);
        DocPos pos;
        if (in_rect(&content, x, y) && header_hit(p, x, y) == -1 && doc_hit(&p->doc, c.x, c.y) < 0 && position_at(p, x, y, &pos)) {
            // A double click takes the word.
            SetFocus(hwnd);
            doc_select_word(&p->doc, pos);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        mouse_down(p, x, y, false);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_SHIFT) & 0x8000) { set_scroll_x(p, p->scroll_x - delta / 2); return 0; }
        // Over a sticky sidebar that overflows, such as the file tree, the wheel scrolls it and not the page.
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &pt);
        RECT content = pane_content_rect(p);
        if (p->footer_h && pt.y >= content.bottom && s && s->vt->footer_wheel && s->vt->footer_wheel(s, pt, delta)) return 0;
        int dy = -delta * px(40) / WHEEL_DELTA;
        if (in_rect(&content, pt.x, pt.y)) {
            POINT c = to_content(p, pt.x, pt.y);
            if (doc_sticky_wheel(&p->doc, c.x, c.y, dy)) { InvalidateRect(hwnd, NULL, FALSE); return 0; }
            // Over a region that overflows, such as a board's column, the wheel scrolls it; the hover follows the cards.
            if (doc_region_wheel(&p->doc, c.x, c.y, dy)) { hide_tip(p); p->doc.hover = doc_hit(&p->doc, c.x, c.y); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        }
        set_scroll(p, p->scroll_y + dy);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        set_scroll_x(p, p->scroll_x + GET_WHEEL_DELTA_WPARAM(wp) / 2);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        if (p->carrying) { SetCursor(LoadCursorW(NULL, IDC_SIZEALL)); return TRUE; }
        bool hand = p->hover_button != -1;
        Item *it = doc_item(&p->doc, p->doc.hover);
        if (it && it->hand) hand = true;
        if (hand) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
        RECT content = pane_content_rect(p);
        if (p->doc.selecting || in_rect(&content, pt.x, pt.y)) {
            // Plain text takes the text cursor; text that acts as a button keeps the arrow.
            POINT c = to_content(p, pt.x, pt.y);
            Item *t = doc_item(&p->doc, doc_text_item_at(&p->doc, c.x, c.y));
            if (p->doc.selecting || (t && !t->action)) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
        }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }
    case WM_TIMER:
        if (wp == TIMER_AUTOSCROLL) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (p->carrying) { carry_to(p, pt.x, pt.y); return 0; }
            if (!p->doc.selecting) { KillTimer(hwnd, TIMER_AUTOSCROLL); return 0; }
            drag_selection(p, pt.x, pt.y);
            return 0;
        }
        if (s && s->vt->timer) s->vt->timer(s, (UINT)wp);
        return 0;
    case WM_COMMAND: if (s && s->vt->command) s->vt->command(s, LOWORD(wp), HIWORD(wp), (HWND)lp); return 0;
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (p->carrying && wp == VK_ESCAPE) { end_carry(p, p->carry_at, DRAG_CANCEL); return 0; }
        if (s && s->vt->key && s->vt->key(s, wp, ctrl, shift)) return 0;
        if (ctrl && wp == 'C' && doc_has_selection(&p->doc)) { copy_selection(p); return 0; }
        if (ctrl && wp == 'A') { select_all(p); return 0; }
        if (wp == VK_ESCAPE && doc_has_selection(&p->doc)) { clear_selection(p); return 0; }
        RECT content = pane_content_rect(p);
        int page = content.bottom - content.top - px(40);
        switch (wp) {
        case VK_PRIOR: set_scroll(p, p->scroll_y - page); return 0;
        case VK_NEXT: set_scroll(p, p->scroll_y + page); return 0;
        case VK_HOME: if (ctrl) { set_scroll(p, 0); return 0; } break;
        case VK_END: if (ctrl) { set_scroll(p, max_scroll(p)); return 0; } break;
        case VK_UP: set_scroll(p, p->scroll_y - px(40)); return 0;
        case VK_DOWN: set_scroll(p, p->scroll_y + px(40)); return 0;
        case VK_F5: if (s && s->vt->refresh) s->vt->refresh(s); return 0;
        case VK_BACK: case VK_ESCAPE: case VK_LEFT:
            if (wp == VK_LEFT && !(GetKeyState(VK_MENU) & 0x8000)) break;
            if (p->depth > 1) pane_pop(p); else if (p->root_back && p->root_back_cb) p->root_back_cb(p->root_back_ctx);
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, theme.ink); SetBkColor(hdc, theme.raise);
        if (!p->edit_brush) p->edit_brush = CreateSolidBrush(theme.raise);
        return (LRESULT)p->edit_brush;
    }
    case WM_THEMECHANGED:
        if (p->edit_brush) { DeleteObject(p->edit_brush); p->edit_brush = NULL; }
        EnumChildWindows(hwnd, retheme_child, 0);
        p->dirty = true; InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
