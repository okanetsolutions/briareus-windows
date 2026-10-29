#include "screen_projects_internal.h"

typedef struct { char text[40]; int badge; bool active, wide; StripMark mark; } StripData;

static void paint_whatsapp_mark(Canvas *cv, const RECT *rc);
static void paint_mail_mark(Canvas *cv, const RECT *rc);
static void paint_strip(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static StripData *strip_button(Doc *doc, const RECT *rc, const char *text, bool wide, int badge, bool active, int action);
static int player_height(void);
static wchar_t volume_glyph(const MediaState *st);
static void volume_paint(Canvas *cv, const MediaState *st, int left, int right, int y, FooterRects *out);
static int player_paint(Canvas *cv, const RECT *rc, FooterRects *out);
static void player_volume_at(Pane *pane, const FooterRects *f, int x);
static void sign_out(void);

/// WhatsApp's mark, which no font has: a green chat bubble with its tail at the lower left, and a white handset.
static void paint_whatsapp_mark(Canvas *cv, const RECT *rc) {
    const COLORREF green = RGB(0x25, 0xD3, 0x66);
    int d = px(18), cx = (rc->left + rc->right) / 2, cy = (rc->top + rc->bottom) / 2;
    RECT bubble = { cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d };
    draw_thick_line(cv, bubble.left + px(4), bubble.bottom - px(5), bubble.left + px(1), bubble.bottom, green, px(4));
    fill_round_rect(cv, &bubble, d / 2, green, green);
    draw_glyph(cv, 0xE717, &bubble, FONT_ICON_SMALL, RGB(0xFF, 0xFF, 0xFF));
}

/// An envelope, which the icon font draws too thinly at this size: a blue body and a white flap.
static void paint_mail_mark(Canvas *cv, const RECT *rc) {
    const COLORREF blue = RGB(0x3D, 0x8B, 0xF2), paper = RGB(0xFF, 0xFF, 0xFF);
    int w = px(16), h = px(12), cx = (rc->left + rc->right) / 2, cy = (rc->top + rc->bottom) / 2;
    RECT body = { cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h };
    fill_round_rect(cv, &body, px(2), blue, blue);
    int flap = body.top + h * 58 / 100, stroke = px(1) + 1;
    draw_thick_line(cv, body.left + px(2), body.top + px(2), cx, flap, paper, stroke);
    draw_thick_line(cv, body.right - px(3), body.top + px(2), cx, flap, paper, stroke);
}

static void paint_strip(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StripData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF border = d->active ? theme.accent : hovered ? theme.accent_dim : theme.line;
    fill_round_rect(cv, rc, px(8), theme.raise, border);
    if (d->mark == MARK_WHATSAPP) {
        paint_whatsapp_mark(cv, rc);
    } else if (d->mark == MARK_MAIL) {
        paint_mail_mark(cv, rc);
    } else if (d->wide) {
        RECT t = { rc->left + px(6), rc->top, rc->right - px(6), rc->bottom };
        draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT t = *rc;
        draw_text(cv, d->text, &t, FONT_EMOJI, d->active ? theme.accent : theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (d->badge) {
        // `absolute -right-1.5 -top-1.5 min-w-4 rounded-full bg-accent px-1 text-[10px] font-semibold leading-4`
        char n[16]; snprintf(n, sizeof n, "%d", d->badge);
        int tw = text_width(cv, n, FONT_TINY_SEMIBOLD) + px(8);
        if (tw < px(16)) tw = px(16);
        RECT b = { rc->right + px(6) - tw, rc->top - px(6), rc->right + px(6), rc->top - px(6) + px(16) };
        fill_round_rect(cv, &b, px(8), theme.accent, theme.accent);
        draw_text(cv, n, &b, FONT_TINY_SEMIBOLD, theme.on_accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static StripData *strip_button(Doc *doc, const RECT *rc, const char *text, bool wide, int badge, bool active, int action) {
    int i = doc_add(doc, rc, paint_strip);
    StripData *d = xcalloc(1, sizeof *d);
    snprintf(d->text, sizeof d->text, "%s", text); d->wide = wide; d->badge = badge; d->active = active;
    Item *it = doc_item(doc, i);
    it->data = d; it->free_data = free; it->action = action; it->hand = true;
    return d;
}

/// The strip; `selected` is the detail pane's root id, for the WhatsApp, Mail, 📊 and ⚑ switches' accent.
void screen_projects_sidebar_top(Doc *doc, int w, const char *selected) {
    w = doc->full_width;
    doc_space(doc, px(10));
    int y = doc->y, h = px(STRIP_H), iw = px(ICON_W), gap = px(STRIP_GAP);
    bool mail = mail_inbox_offered() || mail_settings_offered();
    int icons = STRIP_ICONS + (mail ? 1 : 0);
    int icons_w = iw * icons + gap * (icons - 1);
    RECT nr = { 0, y, w - icons_w - gap, y + h };
    strip_button(doc, &nr, "\xEF\xBC\x8B New session", true, 0, false, ACT_NEW);
    int x = w - icons_w;
    RECT wr = { x, y, x + iw, y + h }; strip_button(doc, &wr, "", false, 0, str_eq(selected, "whatsapp"), ACT_WHATSAPP)->mark = MARK_WHATSAPP;
    x += iw + gap;
    if (mail) {
        RECT mr = { x, y, x + iw, y + h };
        bool on = str_eq(selected, "mail") || str_eq(selected, "mail-settings") || str_has_prefix(selected, "mail-settings:");
        strip_button(doc, &mr, "", false, 0, on, ACT_MAIL)->mark = MARK_MAIL;
        doc_item(doc, (int)doc->count - 1)->tip = xstrdup("Mail");
        x += iw + gap;
    }
    RECT sr = { x, y, x + iw, y + h }; strip_button(doc, &sr, "\xE2\x97\x89", false, (int)screen_projects_g_status, str_eq(selected, "status"), ACT_STATUS);
    doc_item(doc, (int)doc->count - 1)->tip = xstrdup("Status");
    x += iw + gap;
    RECT dr = { x, y, x + iw, y + h }; strip_button(doc, &dr, "\xF0\x9F\x93\x8A", false, 0, str_eq(selected, "dashboard"), ACT_DASHBOARD);
    x += iw + gap;
    RECT fr = { x, y, x + iw, y + h }; strip_button(doc, &fr, "\xE2\x9A\x91", false, (int)screen_projects_g_waiting, str_eq(selected, "findings"), ACT_FINDINGS);
    doc->y = y + h;
    doc_space(doc, px(14));
    doc_pin(doc, (int)doc->count, doc->y);
}

static int player_height(void) {
    MediaState st; media_state(&st);
    if (!st.available) return 0;
    return px(6) + 1 + px(8) + px(PLAYER_ROW) + (st.has_volume ? px(VOLUME_ROW) : 0) + px(2);
}

/// The speaker for a level: crossed out when muted, then with one to three waves.
static wchar_t volume_glyph(const MediaState *st) {
    if (st->muted || st->volume <= 0.001f) return 0xE74F;
    return st->volume < 0.34f ? 0xE993 : st->volume < 0.67f ? 0xE994 : 0xE995;
}

static void volume_paint(Canvas *cv, const MediaState *st, int left, int right, int y, FooterRects *out) {
    int h = px(VOLUME_ROW), cy = y + h / 2;
    RECT m = { left - px(4), y, left + px(20), y + h };
    draw_glyph(cv, volume_glyph(st), &m, FONT_ICON_SMALL, st->muted ? theme.muted : theme.ink);
    out->mute_rc = m;
    // The level in percent at the right, under ⏭; the track between the speaker and it, aligned with the title.
    char pct[8]; snprintf(pct, sizeof pct, "%d%%", (int)(st->volume * 100 + 0.5f));
    RECT t = { right - px(36), y, right - px(4), y + h };
    draw_text(cv, pct, &t, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    int tl = left + px(24), tr = t.left - px(8), th = px(4), knob = px(6);
    if (tr - tl < px(24)) { SetRectEmpty(&out->volume_rc); return; }
    int fx = tl + (int)((tr - tl) * st->volume + 0.5f);
    RECT track = { tl, cy - th / 2, tr, cy - th / 2 + th }, done = { tl, track.top, fx, track.bottom };
    fill_round_rect(cv, &track, th / 2, theme.line, theme.line);
    COLORREF level = st->muted ? blend(theme.muted, theme.sidebar, 0.6) : st->spotify ? RGB(0x1D, 0xB9, 0x54) : theme.accent;
    if (done.right > done.left) fill_round_rect(cv, &done, th / 2, level, level);
    fill_circle(cv, fx, cy, knob, st->muted ? theme.muted : theme.ink);
    out->track_left = tl; out->track_right = tr;
    out->volume_rc = (RECT){ tl - knob, y, tr + knob, y + h };
}

static int player_paint(Canvas *cv, const RECT *rc, FooterRects *out) {
    SetRectEmpty(&out->previous_rc); SetRectEmpty(&out->toggle_rc); SetRectEmpty(&out->next_rc);
    SetRectEmpty(&out->player_rc); SetRectEmpty(&out->mute_rc); SetRectEmpty(&out->volume_rc);
    MediaState st; media_state(&st);
    if (!st.available) return rc->top;
    int y = rc->top + px(6);
    draw_line(cv, rc->left + px(10), y, rc->right - px(10), y, theme.line);
    y += 1 + px(8);
    int left = rc->left + px(16), right = rc->right - px(10), h = px(PLAYER_ROW), bw = px(PLAYER_BUTTON);
    // Spotify's green on its own session, the muted note on another app's.
    RECT note = { left, y, left + px(16), y + h };
    draw_glyph(cv, 0xE8D6, &note, FONT_ICON, st.spotify ? RGB(0x1D, 0xB9, 0x54) : theme.muted);
    RECT buttons[3]; const wchar_t glyphs[3] = { 0xE892, st.playing ? 0xE769 : 0xE768, 0xE893 };
    const bool enabled[3] = { st.can_previous, st.can_toggle, st.can_next };
    for (int i = 0; i < 3; i++) {
        RECT b = { right - (3 - i) * bw, y + (h - bw) / 2, right - (2 - i) * bw, y + (h - bw) / 2 + bw };
        if (i == 1 && enabled[i]) fill_round_rect(cv, &b, bw / 2, theme.raise, theme.line);
        draw_glyph(cv, glyphs[i], &b, i == 1 ? FONT_ICON : FONT_ICON_SMALL, enabled[i] ? theme.ink : blend(theme.muted, theme.sidebar, 0.5));
        buttons[i] = enabled[i] ? b : (RECT){ 0, 0, 0, 0 };
    }
    out->previous_rc = buttons[0]; out->toggle_rc = buttons[1]; out->next_rc = buttons[2];
    int tl = note.right + px(8), tr = right - 3 * bw - px(6);
    const char *title = st.title[0] ? st.title : st.spotify ? "Spotify" : "Nothing playing";
    RECT t = { tl, y + px(1), tr, y + px(19) }, a = { tl, y + px(19), tr, y + h - px(1) };
    draw_text(cv, title, &t, FONT_CAPTION_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (st.artist[0]) draw_text(cv, st.artist, &a, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int bottom = y + h;
    if (st.has_volume) { volume_paint(cv, &st, left, right, bottom, out); bottom += px(VOLUME_ROW); }
    out->player_rc = (RECT){ rc->left, y, rc->right, bottom };
    return bottom + px(2);
}

/// True when the click was on one of the player's buttons.
bool screen_projects_player_click(Pane *pane, const FooterRects *f, POINT pt) {
    MediaCommand command;
    if (PtInRect(&f->previous_rc, pt)) command = MEDIA_PREVIOUS;
    else if (PtInRect(&f->toggle_rc, pt)) command = MEDIA_TOGGLE;
    else if (PtInRect(&f->next_rc, pt)) command = MEDIA_NEXT;
    else if (PtInRect(&f->mute_rc, pt)) command = MEDIA_MUTE;
    else return false;
    media_command(command);
    pane_footer_changed(pane);
    return true;
}

static void player_volume_at(Pane *pane, const FooterRects *f, int x) {
    if (f->track_right <= f->track_left) return;
    media_set_volume((float)(x - f->track_left) / (float)(f->track_right - f->track_left));
    pane_footer_changed(pane);
}

/// A press on the slider sets the level there and starts a drag.
bool screen_projects_player_press(Pane *pane, const FooterRects *f, POINT pt) {
    if (!PtInRect(&f->volume_rc, pt)) return false;
    player_volume_at(pane, f, pt.x);
    return true;
}

void screen_projects_player_drag(Pane *pane, const FooterRects *f, POINT pt) { player_volume_at(pane, f, pt.x); }

bool screen_projects_player_wheel(Pane *pane, const FooterRects *f, POINT pt, int delta) {
    if (!PtInRect(&f->player_rc, pt)) return false;
    MediaState st; media_state(&st);
    if (!st.has_volume) return false;
    // From the level shown, rounded to the step, so a few notches land on 50% and not 47%.
    int level = (int)(st.volume * 100 + 0.5f) + VOLUME_STEP * delta / WHEEL_DELTA;
    level = (level + (level >= 0 ? VOLUME_STEP / 2 : 0)) / VOLUME_STEP * VOLUME_STEP;
    media_set_volume(level / 100.0f);
    pane_footer_changed(pane);
    return true;
}

int screen_projects_sidebar_footer_height(int width) { return (width < px(250) ? px(22) : 0) + player_height() + px(6) + 1 + px(8) + px(FOOT_BUTTON) + px(8); }

void screen_projects_sidebar_footer_paint(Canvas *cv, const RECT *rc, FooterRects *out, bool select_on) {
    fill_rect(cv, rc, theme.sidebar);
    int top = player_paint(cv, rc, out) + px(6);
    draw_line(cv, rc->left + px(10), top, rc->right - px(10), top, theme.line);
    int y = top + 1 + px(8), h = px(FOOT_BUTTON);
    bool stacked = rc->right - rc->left < px(250);
    if (stacked) y += px(22);
    int left = rc->left + px(16), right = rc->right - px(10);
    bool update = false;
    const char *sel = "\xE2\x98\x91 Select", *version = updater_label(&update);
    int sw = text_width(cv, sel, FONT_FOOTNOTE), ow = h, vw = text_width(cv, version, FONT_CAPTION2);
    RECT c = { right - ow, y, right, y + h }, g = { c.left - px(6) - h, y, c.left - px(6), y + h };
    RECT ssh = { g.left - px(6) - h, y, g.left - px(6), y + h };
    if (!project_ssh_offered()) SetRectEmpty(&ssh);
    int buttons_left = IsRectEmpty(&ssh) ? g.left : ssh.left;
    RECT a = { left, y, left + sw, y + h };
    // The version sits between ☑ Select and the buttons, and gives way when the sidebar is too narrow for it.
    int mid = (a.right + buttons_left) / 2;
    RECT v = { mid - vw / 2, y, mid + vw / 2 + 1, y + h };
    draw_text(cv, sel, &a, FONT_FOOTNOTE, select_on ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    out->version_rc = (RECT){ 0, 0, 0, 0 };
    if (stacked) v = (RECT){ left, y - px(22), right, y - px(2) };
    if (stacked || v.left > a.right + px(6)) {
        draw_text(cv, version, &v, FONT_CAPTION2, update ? theme.accent : theme.tertiary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        out->version_rc = v; InflateRect(&out->version_rc, px(4), 0);
    }
    fill_round_rect(cv, &g, px(7), theme.raise, theme.line);
    draw_text(cv, "\xE2\x9A\x99", &g, FONT_EMOJI, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    fill_round_rect(cv, &c, px(7), theme.raise, theme.line);
    draw_glyph(cv, FOOT_SIGN_OUT_GLYPH, &c, FONT_ICON_SMALL, theme.ink);
    InflateRect(&a, px(4), 0);
    out->select_rc = a; out->ssh_rc = ssh; out->settings_rc = g; out->signout_rc = c;
}

static void sign_out(void) {
    if (!app_confirm("Sign out of this dashboard?", "The device token and the saved conversations are removed from this computer. Revoke the token itself in web Settings.", "Sign out", true)) return;
    store_forget();
}

/// The strip's own actions, the same on both screens. True when handled.
bool screen_projects_sidebar_common_action(Pane *pane, int action) {
    switch (action) {
    case ACT_SSH: if (project_ssh_offered()) app_show_detail(ssh_servers_screen_new()); return true;
    case ACT_MAIL: if (mail_inbox_offered() || mail_settings_offered()) app_show_detail(mail_inbox_offered() ? mail_screen_new() : mail_settings_screen_new()); return true;
    case ACT_STATUS: app_show_detail(status_screen_new()); return true;
    case ACT_DASHBOARD: app_show_detail(dashboard_screen_new()); return true;
    case ACT_WHATSAPP: app_show_detail(web_app_screen_new(WEB_APP_WHATSAPP)); return true;
    case ACT_FINDINGS: app_show_detail(findings_screen_new()); return true;
    // Settings take the sidebar's place, as the dashboard's settings page has a sidebar of its own.
    case ACT_SETTINGS: pane_push(pane, settings_screen_new()); return true;
    case ACT_SIGN_OUT: sign_out(); return true;
    }
    return false;
}
