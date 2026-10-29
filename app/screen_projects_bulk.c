#include "screen_projects_internal.h"

typedef struct { RECT all, close_, delete_, delete_all; } BulkRects;

static int bulk_bar_height(SessionsScreen *s);

/// The bulk bar above the foot while ☑ Select is on: the count, Select all, ⏻ Close and 🗑 Delete, 🗑 Delete all.
static int bulk_bar_height(SessionsScreen *s) { return s->select_mode ? px(6) + 1 + px(10) + px(18) + px(6) + px(30) + px(6) + px(18) + px(10) : 0; }

int screen_projects_sessions_footer_height(Screen *base, int width) { SessionsScreen *s = (SessionsScreen *)base; return screen_projects_sidebar_footer_height(width) + bulk_bar_height(s); }

static BulkRects g_bulk;

void screen_projects_sessions_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    SessionsScreen *s = (SessionsScreen *)base;
    fill_rect(cv, rc, theme.sidebar);
    int y = rc->top;
    memset(&g_bulk, 0, sizeof g_bulk);
    if (s->select_mode) {
        int left = rc->left + px(16), right = rc->right - px(16);
        y += px(6); draw_line(cv, rc->left + px(10), y, rc->right - px(10), y, theme.line); y += 1 + px(10);
        char *count = xstrfmt("%zu selected", s->picked_count);
        RECT cr = { left, y, right - px(70), y + px(18) }; draw_text(cv, count, &cr, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); free(count);
        int aw = text_width(cv, "Select all", FONT_FOOTNOTE);
        RECT ar = { right - aw, y, right, y + px(18) }; draw_text(cv, "Select all", &ar, FONT_FOOTNOTE, s->count ? theme.muted : blend(theme.muted, theme.sidebar, 0.5), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        g_bulk.all = ar;
        y += px(18) + px(6);
        int bw = (right - left - px(6)) / 2, bh = px(30);
        bool can = s->picked_count && !s->req_bulk;
        RECT c1 = { left, y, left + bw, y + bh }, c2 = { left + bw + px(6), y, right, y + bh };
        fill_round_rect(cv, &c1, px(7), theme.raise, theme.line); draw_text(cv, s->req_bulk && !s->queue_delete ? "Closing\xE2\x80\xA6" : "\xE2\x8F\xBB Close", &c1, FONT_CAPTION, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        fill_round_rect(cv, &c2, px(7), theme.raise, theme.line); draw_text(cv, s->req_bulk && s->queue_delete ? "Deleting\xE2\x80\xA6" : "\xF0\x9F\x97\x91 Delete", &c2, FONT_CAPTION, can ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        g_bulk.close_ = c1; g_bulk.delete_ = c2;
        y += bh + px(6);
        RECT dr = { left, y, right, y + px(18) }; draw_text(cv, "\xF0\x9F\x97\x91 Delete all", &dr, FONT_CAPTION, s->count && !s->req_bulk ? theme.muted : blend(theme.muted, theme.sidebar, 0.5), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        g_bulk.delete_all = dr;
        y += px(18) + px(10);
        if (s->bulk_error) { RECT er = { left, y - px(8), right, y + px(6) }; draw_text(cv, s->bulk_error, &er, FONT_CAPTION2, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); }
    }
    RECT foot = { rc->left, y, rc->right, rc->bottom };
    screen_projects_sidebar_footer_paint(cv, &foot, &s->footer, s->select_mode);
    screen_projects_footer_controls(base, &s->footer);
}

bool screen_projects_sessions_footer_press(Screen *base, POINT pt) { return screen_projects_player_press(base->pane, &((SessionsScreen *)base)->footer, pt); }

void screen_projects_sessions_footer_drag(Screen *base, POINT pt) { screen_projects_player_drag(base->pane, &((SessionsScreen *)base)->footer, pt); }

bool screen_projects_sessions_footer_wheel(Screen *base, POINT pt, int delta) { return screen_projects_player_wheel(base->pane, &((SessionsScreen *)base)->footer, pt, delta); }

void screen_projects_sessions_footer_click(Screen *base, POINT pt) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (screen_projects_player_click(base->pane, &s->footer, pt)) return;
    if (PtInRect(&s->footer.select_rc, pt)) { s->select_mode = !s->select_mode; if (!s->select_mode) screen_projects_clear_picks(s); pane_footer_changed(base->pane); return; }
    if (PtInRect(&s->footer.version_rc, pt)) { updater_menu(base->pane, pt); return; }
    if (PtInRect(&s->footer.ssh_rc, pt)) { screen_projects_sidebar_common_action(base->pane, ACT_SSH); return; }
    if (PtInRect(&s->footer.settings_rc, pt)) { screen_projects_sidebar_common_action(base->pane, ACT_SETTINGS); return; }
    if (PtInRect(&s->footer.signout_rc, pt)) { screen_projects_sidebar_common_action(base->pane, ACT_SIGN_OUT); return; }
    if (!s->select_mode) return;
    if (PtInRect(&g_bulk.all, pt)) { screen_projects_clear_picks(s); for (size_t i = 0; i < s->count; i++) screen_projects_toggle_pick(s, session_id(&s->sessions[i])); pane_footer_changed(base->pane); }
    else if (PtInRect(&g_bulk.close_, pt)) screen_projects_bulk_start(s, false, false);
    else if (PtInRect(&g_bulk.delete_, pt)) screen_projects_bulk_start(s, true, false);
    else if (PtInRect(&g_bulk.delete_all, pt)) screen_projects_bulk_start(s, true, true);
}
