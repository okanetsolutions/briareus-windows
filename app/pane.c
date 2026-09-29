#include "pane.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#define PANE_CLASS L"BriareusPane"

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
    RECT button_rects[4]; RECT back_rect; RECT bottom_button_rect;
    int hover_button;       // -1 none, -2 back, -3 bottom button, 0...3 header buttons
    int pressed_button;
    bool root_back; void (*root_back_cb)(void *); void *root_back_ctx;
    char *selected_id;
    bool tracking;
    HBRUSH edit_brush;
    // scrollbar drag
    bool dragging_thumb; int drag_offset;
    RECT thumb_rect;
    HDC mem_dc; HBITMAP mem_bmp; int mem_w, mem_h;
};

static Pane **all_panes; static size_t pane_count;

static int margin(Pane *p) { return px(p->sidebar ? 12 : 16); }

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
    p->sidebar = sidebar; p->hover_button = -1; p->pressed_button = -1;
    doc_init(&p->doc);
    p->hwnd = CreateWindowExW(0, PANE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), p);
    all_panes = xrealloc(all_panes, (pane_count + 1) * sizeof *all_panes);
    all_panes[pane_count++] = p;
    return p;
}
void pane_destroy(Pane *p) {
    if (!p) return;
    pane_set_root(p, NULL);
    for (size_t i = 0; i < pane_count; i++) if (all_panes[i] == p) { all_panes[i] = all_panes[--pane_count]; break; }
    if (p->hwnd) DestroyWindow(p->hwnd);
    doc_free(&p->doc); free(p->stack); free(p->selected_id);
    if (p->edit_brush) DeleteObject(p->edit_brush);
    if (p->mem_dc) DeleteDC(p->mem_dc);
    if (p->mem_bmp) DeleteObject(p->mem_bmp);
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
void pane_header_changed(Pane *p) { RECT rc = { 0, 0, 10000, p->header_h }; InvalidateRect(p->hwnd, &rc, FALSE); }
void pane_footer_changed(Pane *p) { p->dirty = true; InvalidateRect(p->hwnd, NULL, FALSE); }

static RECT client(Pane *p) { RECT rc; GetClientRect(p->hwnd, &rc); return rc; }
RECT pane_content_rect(Pane *p) {
    RECT rc = client(p);
    rc.top += p->header_h; rc.bottom -= p->footer_h;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}
int pane_content_width(Pane *p) { RECT rc = client(p); return rc.right - rc.left - 2 * margin(p); }
int pane_scroll_y(Pane *p) { return p->scroll_y; }

static int max_scroll(Pane *p) {
    RECT rc = pane_content_rect(p);
    int visible = rc.bottom - rc.top;
    int m = p->content_height + px(12) - visible;
    return m > 0 ? m : 0;
}
bool pane_at_bottom(Pane *p) { return p->scroll_y >= max_scroll(p) - px(24); }

static void after_scroll(Pane *p) {
    Screen *s = pane_top(p);
    RECT rc = pane_content_rect(p);
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
void pane_set_selected_id(Pane *p, const char *id) {
    if (str_eq(p->selected_id, id)) return;
    free(p->selected_id); p->selected_id = id ? xstrdup(id) : NULL;
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
    bool has_header = s && (p->header.title[0] || p->header.button_count || shows_back(p));
    p->header_h = has_header ? px(p->header.large ? 60 : 52) : 0;
}

static void layout_if_needed(Pane *p, HDC hdc) {
    if (!p->dirty) return;
    p->dirty = false;
    Screen *s = pane_top(p);
    refresh_header(p);
    RECT rc = client(p);
    int width = rc.right - rc.left;
    p->footer_h = s && s->vt->footer_height ? s->vt->footer_height(s, width) : 0;
    bool was_bottom = p->scroll_y >= 0x3fffffff || pane_at_bottom(p);
    int content_width = width - 2 * margin(p);
    if (content_width < px(120)) content_width = px(120);
    doc_begin(&p->doc, hdc, content_width);
    if (s && s->vt->layout) s->vt->layout(s, &p->doc);
    doc_end(&p->doc);
    p->content_height = doc_height(&p->doc);
    int m = max_scroll(p);
    if ((p->stick_bottom && was_bottom) || p->scroll_y >= 0x3fffffff) p->scroll_y = m;
    if (p->scroll_y > m) p->scroll_y = m;
    if (p->scroll_y < 0) p->scroll_y = 0;
    int max_x = p->doc.content_width - content_width; if (max_x < 0) max_x = 0;
    if (p->scroll_x > max_x) p->scroll_x = max_x;
    RECT content = pane_content_rect(p);
    if (s && s->vt->footer_layout) { RECT fr = { rc.left, content.bottom, rc.right, rc.bottom }; s->vt->footer_layout(s, &fr); }
    if (s && s->vt->place) s->vt->place(s, &content, p->scroll_y);
    if (s && s->vt->scrolled) s->vt->scrolled(s, pane_at_bottom(p));
}

static void paint_header(Pane *p, HDC hdc, const RECT *rc) {
    if (!p->header_h) return;
    RECT hr = { rc->left, rc->top, rc->right, rc->top + p->header_h };
    fill_rect(hdc, &hr, theme.background);
    draw_line(hdc, hr.left, hr.bottom - 1, hr.right, hr.bottom - 1, theme.border);
    int x = px(8);
    int size = px(32);
    int cy = hr.top + p->header_h / 2;
    memset(&p->back_rect, 0, sizeof p->back_rect);
    if (shows_back(p)) {
        RECT br = { x, cy - size / 2, x + size, cy + size / 2 };
        p->back_rect = br;
        if (p->hover_button == -2) fill_round_rect(hdc, &br, size / 2, blend(theme.text, theme.background, 0.06), blend(theme.text, theme.background, 0.06));
        draw_glyph(hdc, 0xE72B, &br, FONT_ICON, theme.accent);
        x += size + px(6);
    } else x = px(16);
    int right = hr.right - px(8);
    for (int i = p->header.button_count - 1; i >= 0; i--) {
        RECT br = { right - size, cy - size / 2, right, cy + size / 2 };
        p->button_rects[i] = br;
        HeaderButton *b = &p->header.buttons[i];
        if (p->hover_button == i && b->enabled) fill_round_rect(hdc, &br, size / 2, blend(theme.text, theme.background, 0.06), blend(theme.text, theme.background, 0.06));
        draw_glyph(hdc, b->glyph, &br, FONT_ICON, b->enabled ? theme.accent : blend(theme.accent, theme.background, 0.4));
        right -= size + px(4);
    }
    RECT tr = { x, hr.top, right - px(6), hr.bottom - 1 };
    if (p->header.large) {
        draw_text(hdc, p->header.title, &tr, FONT_LARGE_TITLE, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (p->header.subtitle[0]) {
        int th = font_height(hdc, FONT_HEADLINE), sh = font_height(hdc, FONT_CAPTION);
        int top = cy - (th + sh + px(1)) / 2;
        RECT t1 = { tr.left, top, tr.right, top + th };
        draw_text(hdc, p->header.title, &t1, FONT_HEADLINE, theme.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t2 = { tr.left, top + th + px(1), tr.right, top + th + px(1) + sh };
        if (p->header.status[0]) { draw_status_dot(hdc, t2.left + px(4), (t2.top + t2.bottom) / 2, p->header.status); t2.left += px(13); }
        draw_text(hdc, p->header.subtitle, &t2, FONT_CAPTION, theme.secondary, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        draw_text(hdc, p->header.title, &tr, FONT_HEADLINE, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void paint_scrollbar(Pane *p, HDC hdc, const RECT *content) {
    int m = max_scroll(p);
    memset(&p->thumb_rect, 0, sizeof p->thumb_rect);
    if (m <= 0) return;
    int track = content->bottom - content->top - px(8);
    int visible = content->bottom - content->top;
    int total = p->content_height + px(12);
    int thumb = total > 0 ? track * visible / total : track;
    if (thumb < px(28)) thumb = px(28);
    int y = content->top + px(4) + (track - thumb) * p->scroll_y / m;
    RECT r = { content->right - px(8), y, content->right - px(3), y + thumb };
    p->thumb_rect = r;
    COLORREF c = blend(theme.text, theme.background, p->dragging_thumb ? 0.4 : 0.22);
    fill_round_rect(hdc, &r, px(2), c, c);
}

static void paint(Pane *p, HDC target) {
    RECT rc = client(p);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (!p->mem_dc || p->mem_w != w || p->mem_h != h) {
        if (p->mem_dc) DeleteDC(p->mem_dc);
        if (p->mem_bmp) DeleteObject(p->mem_bmp);
        p->mem_dc = CreateCompatibleDC(target); p->mem_bmp = CreateCompatibleBitmap(target, w, h);
        SelectObject(p->mem_dc, p->mem_bmp); p->mem_w = w; p->mem_h = h;
    }
    HDC hdc = p->mem_dc;
    layout_if_needed(p, hdc);
    fill_rect(hdc, &rc, theme.background);
    Screen *s = pane_top(p);
    RECT content = pane_content_rect(p);
    // Content, clipped.
    HRGN clip = CreateRectRgn(content.left, content.top, content.right, content.bottom);
    SelectClipRgn(hdc, clip);
    RECT clip_rc = content;
    SetViewportOrgEx(hdc, margin(p), content.top, NULL);
    RECT local_clip = { clip_rc.left - margin(p), 0, clip_rc.right - margin(p), clip_rc.bottom - content.top };
    doc_paint(&p->doc, hdc, p->scroll_x, p->scroll_y, &local_clip);
    SetViewportOrgEx(hdc, 0, 0, NULL);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    paint_scrollbar(p, hdc, &content);
    if (p->show_bottom_button && !pane_at_bottom(p) && p->doc.count) {
        int size = px(36);
        RECT b = { (content.left + content.right) / 2 - size / 2, content.bottom - size - px(10), (content.left + content.right) / 2 + size / 2, content.bottom - px(10) };
        p->bottom_button_rect = b;
        fill_round_rect(hdc, &b, size / 2, p->hover_button == -3 ? blend(theme.text, theme.elevated, 0.06) : theme.elevated, theme.border);
        draw_glyph(hdc, 0xE74B, &b, FONT_ICON, theme.text);
    } else memset(&p->bottom_button_rect, 0, sizeof p->bottom_button_rect);
    if (s && s->vt->footer_paint && p->footer_h) { RECT fr = { rc.left, content.bottom, rc.right, rc.bottom }; s->vt->footer_paint(s, hdc, &fr); }
    paint_header(p, hdc, &rc);
    BitBlt(target, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
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
    for (int i = 0; i < p->header.button_count; i++) if (in_rect(&p->button_rects[i], x, y)) return i;
    if (p->show_bottom_button && in_rect(&p->bottom_button_rect, x, y)) return -3;
    return -1;
}
static void mouse_move(Pane *p, int x, int y) {
    if (!p->tracking) { TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, p->hwnd, 0 }; TrackMouseEvent(&tme); p->tracking = true; }
    if (p->dragging_thumb) {
        RECT content = pane_content_rect(p);
        int track = content.bottom - content.top - px(8);
        int thumb = p->thumb_rect.bottom - p->thumb_rect.top;
        int m = max_scroll(p);
        if (track - thumb > 0) set_scroll(p, (y - p->drag_offset - content.top - px(4)) * m / (track - thumb));
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
}
static void mouse_down(Pane *p, int x, int y, bool right) {
    SetFocus(p->hwnd);
    if (!right && in_rect(&p->thumb_rect, x, y)) { p->dragging_thumb = true; p->drag_offset = y - p->thumb_rect.top; SetCapture(p->hwnd); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    int button = header_hit(p, x, y);
    RECT content = pane_content_rect(p);
    if (button != -1) { p->pressed_button = button; SetCapture(p->hwnd); return; }
    if (in_rect(&content, x, y)) {
        POINT c = to_content(p, x, y);
        int item = doc_hit(&p->doc, c.x, c.y);
        if (right) {
            Screen *s = pane_top(p);
            if (s && s->vt->context) {
                POINT sp = { x, y }; ClientToScreen(p->hwnd, &sp);
                Item *it = doc_item(&p->doc, item);
                s->vt->context(s, it ? it->action : 0, it ? it->arg : (intptr_t)(item), sp);
            }
            return;
        }
        p->doc.pressed = item; SetCapture(p->hwnd); InvalidateRect(p->hwnd, NULL, FALSE);
        return;
    }
    if (!right && p->footer_h && y >= content.bottom) {
        Screen *s = pane_top(p);
        POINT pt = { x, y };
        if (s && s->vt->footer_click) s->vt->footer_click(s, pt);
    }
}
static void mouse_up(Pane *p, int x, int y) {
    if (p->dragging_thumb) { p->dragging_thumb = false; ReleaseCapture(); InvalidateRect(p->hwnd, NULL, FALSE); return; }
    if (GetCapture() == p->hwnd) ReleaseCapture();
    Screen *s = pane_top(p);
    POINT sp = { x, y }; ClientToScreen(p->hwnd, &sp);
    if (p->pressed_button != -1) {
        int b = p->pressed_button; p->pressed_button = -1;
        if (header_hit(p, x, y) == b) {
            if (b == -2) { if (p->depth > 1) pane_pop(p); else if (p->root_back_cb) p->root_back_cb(p->root_back_ctx); }
            else if (b == -3) { set_scroll(p, max_scroll(p)); }
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
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); paint(p, hdc); EndPaint(hwnd, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: p->dirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE: mouse_move(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSELEAVE: p->tracking = false; if (p->hover_button != -1 || p->doc.hover != -1) { p->hover_button = -1; p->doc.hover = -1; InvalidateRect(hwnd, NULL, FALSE); } return 0;
    case WM_LBUTTONDOWN: mouse_down(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false); return 0;
    case WM_RBUTTONDOWN: mouse_down(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true); return 0;
    case WM_LBUTTONUP: mouse_up(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONDBLCLK: mouse_down(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false); return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            int max_x = p->doc.content_width - pane_content_width(p); if (max_x < 0) max_x = 0;
            int x = p->scroll_x - delta / 2; if (x < 0) x = 0; if (x > max_x) x = max_x;
            if (x != p->scroll_x) { p->scroll_x = x; InvalidateRect(hwnd, NULL, FALSE); }
        } else set_scroll(p, p->scroll_y - delta * px(40) / WHEEL_DELTA);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int max_x = p->doc.content_width - pane_content_width(p); if (max_x < 0) max_x = 0;
        int x = p->scroll_x + delta / 2; if (x < 0) x = 0; if (x > max_x) x = max_x;
        if (x != p->scroll_x) { p->scroll_x = x; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        bool hand = p->hover_button != -1;
        Item *it = doc_item(&p->doc, p->doc.hover);
        if (it && it->hand) hand = true;
        if (hand) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }
    case WM_TIMER: if (s && s->vt->timer) s->vt->timer(s, (UINT)wp); return 0;
    case WM_COMMAND: if (s && s->vt->command) s->vt->command(s, LOWORD(wp), HIWORD(wp), (HWND)lp); return 0;
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (s && s->vt->key && s->vt->key(s, wp, ctrl, shift)) return 0;
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
        SetTextColor(hdc, theme.text); SetBkColor(hdc, theme.elevated);
        if (!p->edit_brush) p->edit_brush = CreateSolidBrush(theme.elevated);
        return (LRESULT)p->edit_brush;
    }
    case WM_THEMECHANGED: if (p->edit_brush) { DeleteObject(p->edit_brush); p->edit_brush = NULL; } p->dirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

char *pane_hovered_text(Pane *p) { return doc_item_plain_text(&p->doc, p->doc.hover); }
