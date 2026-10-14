#include "screen_mail_inbox_internal.h"

typedef struct { wchar_t glyph; bool on, muted; } IconData;

typedef struct { bool on; } MenuRow;

static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_command(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_menu_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

void screen_mail_inbox_search_free(void *p) { SearchData *d = p; free(d->shown); free(d); }

void screen_mail_inbox_mailbtn_free(void *p) { MailBtn *d = p; free(d->shown); free(d); }

void screen_mail_inbox_chip_free(void *p) { ChipData *d = p; free(d->shown); free(d); }

void screen_mail_inbox_paint_search(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SearchData *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(8), theme.field, hot ? theme.line_strong : theme.line);
    RECT g = { rc->left + px(6), rc->top, rc->left + px(28), rc->bottom };
    draw_glyph(cv, 0xE721, &g, FONT_ICON_SMALL, theme.muted);
    RECT slash = { rc->right - px(28), rc->top + px(6), rc->right - px(8), rc->bottom - px(6) };
    if (slash.left < g.right) slash.left = g.right;
    fill_round_rect(cv, &slash, px(4), theme.raise, theme.line);
    draw_text(cv, "/", &slash, FONT_CAPTION2, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT t = { g.right + px(2), rc->top, slash.left - px(4), rc->bottom };
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, d && d->placeholder ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_mail_inbox_paint_mailbtn(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MailBtn *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, hot || (d && d->on) ? theme.field : theme.canvas, theme.line);
    int left = rc->left + px(12);
    if (d && d->unread > 0) {
        fill_circle(cv, left + px(4), (rc->top + rc->bottom) / 2, px(4), theme.accent);
        left += px(16);
    }
    RECT t = { left, rc->top, rc->right - px(28), rc->bottom };
    if (d && d->unread > 0) {
        char num[16]; snprintf(num, sizeof num, "%d", d->unread > 999 ? 999 : d->unread);
        int nw = text_width(cv, num, FONT_CAPTION2) + px(12);
        RECT b = { rc->right - px(28) - nw, rc->top + px(7), rc->right - px(26), rc->bottom - px(7) };
        if (b.left < left) b.left = left;
        fill_round_rect(cv, &b, (b.bottom - b.top) / 2, theme.raise, theme.line);
        draw_text(cv, num, &b, FONT_CAPTION2, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        t.right = b.left - px(4);
    }
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT chev = { rc->right - px(24), rc->top, rc->right - px(4), rc->bottom };
    draw_glyph(cv, 0xE70D, &chev, FONT_ICON_SMALL, theme.muted);
}

void screen_mail_inbox_paint_chip(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ChipData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    COLORREF ink = d && d->accent ? theme.accent : d && d->on ? theme.ink : theme.muted;
    if (!d || !d->accent) {
        COLORREF fill = d && d->on ? theme.field : theme.canvas;
        COLORREF border = (d && d->on) || hot ? theme.line_strong : theme.line;
        fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, fill, border);
    }
    int pad = d && d->caret ? px(22) : px(12);
    RECT t = { rc->left + px(12), rc->top, rc->right - pad, rc->bottom };
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (d && d->caret) {
        RECT g = { rc->right - px(22), rc->top, rc->right - px(2), rc->bottom };
        draw_glyph(cv, 0xE70D, &g, FONT_ICON_SMALL, theme.muted);
    }
}

static void paint_icon(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    IconData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    if (hot) fill_round_rect(cv, rc, px(8), theme.field, theme.line);
    COLORREF ink = d && d->on ? theme.accent : (!it->action || (d && d->muted)) ? theme.muted : theme.ink;
    draw_glyph(cv, d ? d->glyph : 0, rc, FONT_ICON_SMALL, ink);
}

static void paint_command(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    IconData *d = it->data;
    bool hot = doc_item_hovered(doc, it) && it->action;
    fill_round_rect(cv, rc, px(8), hot ? theme.field : theme.raise, theme.line);
    RECT g = { rc->left + px(6), rc->top, rc->left + px(26), rc->bottom };
    if (d && d->glyph) draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, theme.ink);
    RECT t = { rc->left + (d && d->glyph ? px(26) : px(10)), rc->top, rc->right - px(8), rc->bottom };
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION, it->action ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_mail_inbox_paint_dot(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    fill_circle(cv, (rc->left + rc->right) / 2, (rc->top + rc->bottom) / 2, px(3), theme.accent);
}

void screen_mail_inbox_paint_count(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    fill_round_rect(cv, rc, px(8), theme.field, theme.line);
    RECT t = *rc;
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION2, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void screen_mail_inbox_paint_tag(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    COLORREF fill = blend(theme.accent, theme.canvas, theme.dark ? 0.28 : 0.16);
    fill_round_rect(cv, rc, px(4), fill, fill);
    RECT t = *rc;
    draw_text(cv, it->text ? it->text : "", &t, FONT_TINY_SEMIBOLD, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void screen_mail_inbox_paint_avatar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; Avatar *a = it->data;
    int cx = (rc->left + rc->right) / 2, cy = (rc->top + rc->bottom) / 2, rad = (rc->right - rc->left) / 2;
    fill_circle(cv, cx, cy, rad, a && a->mine ? theme.accent : theme.field);
    RECT t = *rc;
    draw_text(cv, a ? a->initials : "?", &t, FONT_CAPTION_SEMIBOLD, a && a->mine ? theme.on_accent : theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_menu_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MenuRow *d = it->data;
    bool hot = doc_item_hovered(doc, it);
    if ((d && d->on) || hot) fill_round_rect(cv, rc, px(6), theme.field, theme.field);
    RECT t = { rc->left + px(10), rc->top, rc->right - px(10), rc->bottom };
    draw_text(cv, it->text ? it->text : "", &t, FONT_CAPTION, it->action ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_mail_inbox_add_icon(Doc *doc, const RECT *rc, wchar_t glyph, bool on, bool muted, const char *text, int screen_mail_inbox_action, const char *tip) {
    IconData *d = xcalloc(1, sizeof *d); d->glyph = glyph; d->on = on; d->muted = muted;
    Item *it = doc_item(doc, doc_add(doc, rc, paint_icon));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (screen_mail_inbox_action) { it->action = screen_mail_inbox_action; it->hand = true; }
    if (tip) it->tip = xstrdup(tip);
}

void screen_mail_inbox_add_command(Doc *doc, const RECT *rc, wchar_t glyph, const char *text, int screen_mail_inbox_action, const char *tip) {
    IconData *d = xcalloc(1, sizeof *d); d->glyph = glyph;
    Item *it = doc_item(doc, doc_add(doc, rc, paint_command));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (screen_mail_inbox_action) { it->action = screen_mail_inbox_action; it->hand = true; }
    if (tip) it->tip = xstrdup(tip);
}

int screen_mail_inbox_chip_span(Doc *doc, const char *shown, bool accent, bool caret) {
    int tw = text_width(doc->cv, shown, FONT_CAPTION) + px(24) + (caret ? px(14) : 0);
    if (caret && tw > px(280)) tw = px(280);
    if (!accent && tw < px(36)) tw = px(36);
    return tw;
}

// `max_w` clamps a chip that would otherwise paint past the row; 0 leaves the measured width.
int screen_mail_inbox_add_chip(Doc *doc, int x, int y, int h, const char *text, const char *shown, bool on, bool accent, bool caret, int screen_mail_inbox_action, intptr_t arg, int max_w) {
    int tw = screen_mail_inbox_chip_span(doc, shown, accent, caret);
    if (max_w > 0 && tw > max_w) tw = max_w;
    RECT rc = { x, y, x + tw, y + h };
    ChipData *d = xcalloc(1, sizeof *d);
    d->shown = xstrdup(shown ? shown : ""); d->on = on; d->accent = accent; d->caret = caret;
    Item *it = doc_item(doc, doc_add(doc, &rc, screen_mail_inbox_paint_chip));
    it->data = d; it->free_data = screen_mail_inbox_chip_free; it->text = xstrdup(text ? text : "");
    if (screen_mail_inbox_action) { it->action = screen_mail_inbox_action; it->arg = arg; it->hand = true; }
    return tw;
}

int screen_mail_inbox_menu_row(Doc *doc, int x, int y, int w, const char *text, bool on, int screen_mail_inbox_action, intptr_t arg) {
    RECT rc = { x, y, x + w, y + px(28) };
    MenuRow *d = xcalloc(1, sizeof *d); d->on = on;
    Item *it = doc_item(doc, doc_add(doc, &rc, paint_menu_row));
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : "");
    if (screen_mail_inbox_action) { it->action = screen_mail_inbox_action; it->arg = arg; it->hand = true; }
    return px(28);
}
