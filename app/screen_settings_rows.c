#include "screen_settings_internal.h"

/// A project row's ↑ or ↓, laid over the right end of its first line; `row` is the row's item.
typedef struct { int row; bool up, enabled, selected; } MoveData;

static bool row_hovered(Doc *doc, Item *it);
static void paint_move(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

void screen_settings_paint_back(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    RECT t = { rc->left + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "\xE2\x86\x90 Back to sessions", &t, FONT_CAPTION, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// movable: ↑ and ↓ lie over it
void screen_settings_project_row_free(void *p) { ProjectRowData *d = p; free(d->label); free(d->repo); free(d); }

/// Whether the mouse is on the row or on one of its arrows.
static bool row_hovered(Doc *doc, Item *it) {
    if (doc_item_hovered(doc, it)) return true;
    Item *h = doc->hover >= 0 ? doc_item(doc, doc->hover) : NULL;
    return h && h->paint == paint_move && doc_item(doc, ((MoveData *)h->data)->row) == it;
}

static void paint_move(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MoveData *d = it->data;
    // Shown on the row under the mouse and on the open one, as the dashboard's row actions are.
    if (!d->selected && !row_hovered(doc, doc_item(doc, d->row))) return;
    bool hovered = d->enabled && doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(4), hovered ? theme.sidebar : theme.raise, hovered ? theme.accent_dim : theme.raise);
    draw_glyph(cv, d->up ? 0xE70E : 0xE70D, rc, FONT_ICON_SMALL, !d->enabled ? blend(theme.muted, theme.raise, 0.5) : hovered ? theme.ink : theme.muted);
}

void screen_settings_paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProjectRowData *d = it->data;
    bool hovered = row_hovered(doc, it);
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(6), lh = px(22);
    // `.dot.idle` for a project sessions can start on, the plain grey dot for one switched off.
    draw_status_dot(cv, x + px(3), top + lh / 2, d->enabled ? "idle" : "");
    // The label stops short of the arrows while they show.
    int label_right = d->movable && (hovered || d->selected) ? rc->right - px(6) - px(22) * 2 - px(2) - px(6) : rc->right - px(8);
    RECT t = { x + px(7) + px(7), top, label_right, top + lh };
    draw_text(cv, d->label, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int y2 = top + lh, right = rc->right - px(8);
    if (d->db) {
        // `rounded border border-line px-1 text-[11px]`: the database pool's tag.
        int h; int bw = draw_chip(NULL, 0, 0, "db", theme.muted, theme.sidebar, &h);
        int rw = text_width(cv, d->repo, FONT_CAPTION);
        int bx = x + (rw < right - x - bw - px(8) ? rw : right - x - bw - px(8)) + px(8);
        draw_chip(cv, bx, y2 + (px(18) - h) / 2, "db", theme.muted, hovered || d->selected ? theme.raise : theme.sidebar, &h);
        right = bx - px(8);
    }
    RECT r = { x, y2, right, y2 + px(18) };
    draw_text(cv, d->repo, &r, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_settings_provider_row_free(void *p) { ProviderRowData *d = p; free(d->label); free(d->binary); free(d); }

void screen_settings_paint_provider_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProviderRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF background = hovered || d->selected ? theme.raise : theme.sidebar;
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(6), lh = px(22);
    draw_status_dot(cv, x + px(3), top + lh / 2, d->active ? "idle" : "");
    RECT t = { x + px(7) + px(7), top, rc->right - px(8), top + lh };
    draw_text(cv, d->label, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // The dashboard's badges: the CLI it runs, and what sets it apart.
    const char *tags[] = { d->binary, d->active || d->unsaved ? NULL : "inactive", d->login ? "own login" : NULL, d->endpoint ? "custom endpoint" : NULL };
    int bx = x, y2 = top + lh, right = rc->right - px(8);
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        if (str_empty(tags[i])) continue;
        int bw = text_width(cv, tags[i], FONT_CAPTION2) + px(12) + 2;
        if (bx + bw > right) break;
        int h; draw_chip(cv, bx, y2 + px(1), tags[i], theme.muted, background, &h);
        bx += bw + px(6);
    }
}

void screen_settings_settings_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(json_get(s->projects, "list"), index);
    if (json_is_object(row)) app_show_detail(project_settings_screen_new(row, json_get(s->projects, "defaults")));
}

const Json *screen_settings_settings_rows(SettingsScreen *s) { return json_get(s->projects, "list"); }

const Json *screen_settings_provider_rows(SettingsScreen *s) { return json_get(s->providers, "list"); }

void screen_settings_settings_open_provider(SettingsScreen *s, size_t index) {
    const Json *row = json_at(screen_settings_provider_rows(s), index);
    if (json_is_object(row)) app_show_detail(provider_settings_screen_new(row, json_get(s->providers, "defaults")));
}

/// A section's summary: its title, a muted note after it when `note` is set, and its ＋ New when `new_action` is set.
void screen_settings_section_title(Doc *doc, int w, const char *title, const char *screen_settings_note, int new_action) {
    int y = doc->y, h = px(20);
    RECT tr = { px(8), y, w - px(60), y + h };
    doc_text_at(doc, &tr, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int nx = px(8) + text_width(doc->cv, title, FONT_CAPTION_SEMIBOLD) + px(6);
    if (screen_settings_note && nx < tr.right) {
        RECT nr = { nx, y, tr.right, y + h };
        doc_text_at(doc, &nr, screen_settings_note, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (new_action) {
        int nw = text_width(doc->cv, "\xEF\xBC\x8B New", FONT_CAPTION) + px(8);
        RECT nr = { w - px(4) - nw, y, w - px(4), y + h };
        Item *it = doc_item(doc, doc_text_at(doc, &nr, "\xEF\xBC\x8B New", FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE));
        it->action = new_action; it->hand = true;
    }
    doc->y = y + h;
    doc_space(doc, px(4));
}

/// The ↑ and ↓ that move project `i` of `n` up and down the list, at the right end of its row's first line.
void screen_settings_layout_move_arrows(SettingsScreen *s, Doc *doc, int row, size_t i, size_t n, bool selected) {
    RECT rc = doc_item(doc, row)->rc;
    int size = px(22), right = rc.right - px(6), top = rc.top + px(6);
    for (int k = 0; k < 2; k++) {
        bool up = k == 0, enabled = !s->req_order && (up ? i > 0 : i + 1 < n);
        RECT b = { right - (2 - k) * size - (1 - k) * px(2), top, right - (1 - k) * (size + px(2)), top + size };
        Item *it = doc_item(doc, doc_add(doc, &b, paint_move));
        MoveData *d = xcalloc(1, sizeof *d);
        d->row = row; d->up = up; d->enabled = enabled; d->selected = selected;
        it->data = d; it->free_data = free;
        // A disabled arrow still takes the click, so the end of the list does not open the project under it.
        it->action = enabled ? (up ? ACT_MOVE_UP : ACT_MOVE_DOWN) : ACTION_TIP; it->arg = (intptr_t)i; it->hand = enabled;
        it->tip = xstrdup(up ? "Move up" : "Move down");
    }
}
