#include "screen_status_internal.h"
#include <commctrl.h>

intptr_t status_action_add(StatusScreen *s, size_t card, int option) {
    s->actions = xrealloc(s->actions, (s->action_count + 1) * sizeof *s->actions);
    s->actions[s->action_count++] = (StatusAction){ card, option }; return s->action_next++;
}
static size_t card_index(StatusScreen *s, const Session *session) {
    for (size_t i = 0; i < s->card_count; i++) if (str_eq(session_id(&s->cards[i]->session), session_id(session)) && str_eq(session_repo(&s->cards[i]->session), session_repo(session))) return i;
    return s->card_count;
}
static void heading(StatusScreen *s, Doc *doc, size_t index, int x, int w, FontId font_id) {
    StatusCard *c = s->cards[index]; const Session *session = &c->session;
    int i = doc_text(doc, x, w, session_display_title(session), font_id, theme.ink, DT_WORDBREAK);
    Item *it = doc_item(doc, i); it->action = STATUS_OPEN; it->arg = status_action_add(s, index, -1); it->hand = true;
    char *detail = xstrfmt("%s · %s", session_repo(session), session_state_label(session_state(session)));
    doc_text(doc, x, w, detail, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(detail);
}
// Options are immediate answers, like the conversation's existing option actions.
static void paint_option(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hover = it->action && doc_item_hovered(doc, it);
    bool pressed = it->action && doc->pressed >= 0 && &doc->items[doc->pressed] == it;
    COLORREF fill = pressed ? blend(theme.accent, theme.raise, 0.2) : theme.canvas;
    fill_round_rect(cv, rc, px(7), fill, hover || pressed ? theme.accent : theme.line);
    RECT text = *rc; InflateRect(&text, -px(12), -px(12));
    draw_text(cv, it->text, &text, FONT_BODY, it->action ? theme.ink : theme.muted, DT_LEFT | DT_WORDBREAK);
}
static void option_button(StatusScreen *s, Doc *doc, int x, int w, const char *label, size_t index, int option, bool can) {
    int h = measure_text(doc->cv, label, w - px(24), FONT_BODY, DT_WORDBREAK) + px(24);
    int i = doc_custom(doc, x, w, h, paint_option, NULL, NULL, can ? STATUS_OPTION : 0, status_action_add(s, index, option));
    doc_item(doc, i)->text = xstrdup(label);
    doc_space(doc, px(6));
}
static void waiting(StatusScreen *s, Doc *doc, size_t index, int x, int w) {
    StatusCard *c = s->cards[index];
    int pad = px(20), box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    int iw = w - pad * 2; pad += x; heading(s, doc, index, pad, iw, FONT_SUBHEADLINE); doc_space(doc, px(20));
    const Event *question = session_state(&c->session) == SESSION_WAITING ? transcript_pending_question(&c->transcript) : NULL;
    const char *text = question ? question->question ? question->question : question->text : json_str(json_get(c->session.raw, "lastText"));
    if (text) doc_text(doc, pad, iw, text, FONT_HEADLINE, theme.ink, DT_WORDBREAK);
    doc_space(doc, px(12));
    if (session_state(&c->session) == SESSION_FINDINGS) {
        doc_button(doc, pad, 0, "Decide findings →", BUTTON_BORDERED, STATUS_OPEN, status_action_add(s, index, -1), true);
    } else if (store_supports("message") && question) {
        bool can = status_can_answer(s, c);
        for (size_t i = 0; i < json_count(question->options); i++) {
            const char *label = json_str_nonempty(json_get(json_at(question->options, i), "label"));
            if (label) option_button(s, doc, pad, iw, label, index, (int)i, can);
        }
        doc_space(doc, px(12));
        c->label_rect = (RECT){ pad, doc->y, pad + iw, doc->y + px(20) };
        doc_space(doc, px(20));
        doc_space(doc, px(8));
        int send_w = text_width(doc->cv, "Send reply", FONT_BODY) + px(32) + 2;
        int editor = doc_box_begin(doc, pad, iw, px(10), theme.raise, theme.secondary, px(7));
        c->laid = true; c->rect = (RECT){ pad + px(10), doc->y, pad + iw - px(10), doc->y + px(28) };
        doc_space(doc, px(28)); doc_box_end(doc, editor, px(10));
        doc_space(doc, px(12));
        c->reply_action = status_action_add(s, index, -1);
        char *trim = str_trim(c->draft ? c->draft : "");
        bool ready = can && *trim; free(trim);
        int send = doc_button(doc, pad, send_w, "Send reply", ready ? BUTTON_PROMINENT : BUTTON_BORDERED, STATUS_SEND, c->reply_action, ready);
        doc_item(doc, send)->rc.bottom = doc_item(doc, send)->rc.top + px(40);
        doc->y = doc_item(doc, send)->rc.bottom;
    }
    if (c->read_error) doc_notice(doc, pad, iw, c->read_error);
    if (c->error) doc_notice(doc, pad, iw, c->error);
    if (status_uncertain(s, c)) {
        doc_notice(doc, pad, iw, "The send outcome is unknown. Check the conversation before allowing another send.");
        doc_button(doc, pad, iw, "I checked the conversation; refresh", BUTTON_BORDERED, STATUS_RESOLVE, status_action_add(s, index, -1), !s->write && !s->check_read);
    }
    doc_box_end(doc, box, px(16)); doc_space(doc, px(16));
}
void status_layout(Screen *base, Doc *doc) {
    StatusScreen *s = (StatusScreen *)base; int w = doc->width;
    s->action_first = s->action_next; s->action_count = 0;
    for (size_t i = 0; i < s->card_count; i++) s->cards[i]->laid = false;
    if (!status_current(s)) { doc_notice(doc, 0, w, "The connection changed. Reopen Status on the current server."); return; }
    int available = w;
    if (w > px(1440)) w = px(1440);
    int origin = (available - w) / 2;
    doc_space(doc, px(20));
    if (s->error) doc_notice(doc, origin, w, s->error);
    if (s->missing) doc_notice(doc, origin, w, "Some project lists are unavailable. Showing saved sessions; totals may be incomplete.");
    if (!s->loaded) doc_loading(doc, origin, w, "Loading Status…");
    bool complete = s->loaded && !s->error && !s->missing;
    char *ready = complete ? xstrfmt("Need you  %u     Working  %u     Ready  %u", (unsigned)s->overview.waiting_count, (unsigned)s->overview.working_count, (unsigned)s->overview.ready) : xstrdup("Need you  —     Working  —     Ready  —");
    doc_text(doc, origin, w, ready, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK); free(ready);
    doc_space(doc, px(20));
    bool columns = w >= px(1040);
    int gap = px(32), working_w = columns ? (w - gap) * 36 / 100 : w;
    int waiting_w = columns ? w - gap - working_w : w;
    int top = doc->y;
    doc_section(doc, origin, waiting_w, "Need you");
    if (complete && !s->overview.waiting_count) doc_text(doc, origin, waiting_w, "You're all caught up. Sessions needing your answer will appear here.", FONT_BODY, theme.muted, DT_WORDBREAK);
    for (size_t i = 0; i < s->overview.waiting_count; i++) {
        size_t index = card_index(s, &s->sessions[s->overview.waiting[i]]);
        if (index < s->card_count) waiting(s, doc, index, origin, waiting_w);
    }
    int waiting_bottom = doc->y;
    if (columns) doc->y = top;
    else doc_space(doc, px(8));
    int x = origin + (columns ? waiting_w + gap : 0);
    doc_section(doc, x, working_w, "Working");
    if (complete && !s->overview.working_count) doc_text(doc, x, working_w, "No sessions running.", FONT_BODY, theme.muted, DT_WORDBREAK);
    for (size_t i = 0; i < s->overview.working_count; i++) {
        size_t index = card_index(s, &s->sessions[s->overview.working[i]]);
        if (index < s->card_count) {
            int box = doc_box_begin(doc, x, working_w, px(8), theme.canvas, theme.canvas, 0);
            heading(s, doc, index, x + px(8), working_w - px(16), FONT_BODY);
            doc_box_end(doc, box, px(8));
            doc_box_action(doc, box, STATUS_OPEN, status_action_add(s, index, -1));
            doc_rule(doc, x, working_w);
        }
    }
    if (doc->y < waiting_bottom) doc->y = waiting_bottom;
}
void status_controls_hide(StatusScreen *s) {
    for (size_t i = 0; i < s->card_count; i++) {
        if (s->cards[i]->label) ShowWindow(s->cards[i]->label, SW_HIDE);
        if (s->cards[i]->edit) ShowWindow(s->cards[i]->edit, SW_HIDE);
    }
}
static LRESULT CALLBACK reply_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    StatusScreen *s = (StatusScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        for (size_t i = 0; i < s->card_count; i++) if (s->cards[i]->edit == hwnd) {
            s->base.vt->action(&s->base, STATUS_SEND, s->cards[i]->reply_action, (POINT){0}); break;
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
        size_t current = 0;
        while (current < s->card_count && s->cards[current]->edit != hwnd) current++;
        for (size_t n = 1; n < s->card_count; n++) {
            size_t i = step > 0 ? (current + n) % s->card_count : (current + s->card_count - n) % s->card_count;
            HWND next = s->cards[i]->edit;
            if (next && IsWindowVisible(next) && IsWindowEnabled(next)) { SetFocus(next); return 0; }
        }
        SetFocus(GetParent(hwnd)); return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_F5) { s->base.vt->refresh(&s->base); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_TAB || wp == VK_ESCAPE)) return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, reply_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void ensure_edit(StatusScreen *s, StatusCard *c) {
    if (c->edit) return;
    c->label = CreateWindowExW(0, L"STATIC", L"Your &reply", WS_CHILD | SS_LEFT, 0, 0, 10, 10, pane_hwnd(s->base.pane), NULL, GetModuleHandleW(NULL), NULL);
    theme_apply_control(c->label);
    c->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, pane_hwnd(s->base.pane), NULL, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(c->edit, reply_proc, 0, (DWORD_PTR)s);
    theme_apply_control(c->edit);

    SendMessageW(c->edit, EM_SETLIMITTEXT, 100000, 0);
    wchar_t *text = utf8_to_wide(c->draft ? c->draft : ""); SetWindowTextW(c->edit, text); free(text);
}
void status_place(Screen *base, const RECT *content, int scroll_y) {
    StatusScreen *s = (StatusScreen *)base;
    RECT client; GetClientRect(pane_hwnd(base->pane), &client); int margin = (client.right - pane_content_width(base->pane)) / 2;
    for (size_t i = 0; i < s->card_count; i++) {
        StatusCard *c = s->cards[i];
        bool show = status_current(s) && s->shown && c->laid && store_supports("message");
        if (show) ensure_edit(s, c);
        if (!c->edit) continue;
        SendMessageW(c->edit, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
        HWND parent = pane_hwnd(base->pane);
        if (GetParent(c->label) != parent) SetParent(c->label, parent);
        SendMessageW(c->label, WM_SETFONT, (WPARAM)font(FONT_FOOTNOTE_SEMIBOLD), TRUE);
        RECT lr = c->label_rect, clipped = {0}; OffsetRect(&lr, content->left + margin, content->top - scroll_y);
        bool label_visible = show && IntersectRect(&clipped, &lr, content);
        MoveWindow(c->label, lr.left, lr.top, lr.right - lr.left, lr.bottom - lr.top, TRUE);
        if (label_visible) SetWindowRgn(c->label, CreateRectRgn(clipped.left - lr.left, clipped.top - lr.top, clipped.right - lr.left, clipped.bottom - lr.top), TRUE);
        ShowWindow(c->label, label_visible ? SW_SHOWNA : SW_HIDE);
        if (GetParent(c->edit) != parent) SetParent(c->edit, parent);
        EnableWindow(c->edit, status_can_answer(s, c));
        RECT r = c->rect, visible; OffsetRect(&r, content->left + margin, content->top - scroll_y);
        if (!show || !IntersectRect(&visible, &r, content)) { ShowWindow(c->edit, SW_HIDE); continue; }
        MoveWindow(c->edit, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        SetWindowRgn(c->edit, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        ShowWindow(c->edit, SW_SHOWNA);
    }
}
void status_command(Screen *base, int id, int code, HWND control) {
    (void)id; StatusScreen *s = (StatusScreen *)base;
    if (code != EN_CHANGE || !status_current(s)) return;
    for (size_t i = 0; i < s->card_count; i++) {
        StatusCard *c = s->cards[i]; if (c->edit != control) continue;
        int n = GetWindowTextLengthW(control); wchar_t *text = xcalloc((size_t)n + 1, sizeof *text); GetWindowTextW(control, text, n + 1);
        free(c->draft); c->draft = wide_to_utf8(text); free(text); pane_relayout(base->pane); return;
    }
}
