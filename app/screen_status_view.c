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
static void heading(StatusScreen *s, Doc *doc, size_t index, int x, int w) {
    StatusCard *c = s->cards[index]; const Session *session = &c->session;
    int i = doc_text(doc, x, w, session_display_title(session), FONT_SUBHEADLINE, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    Item *it = doc_item(doc, i); it->action = STATUS_OPEN; it->arg = status_action_add(s, index, -1); it->hand = true;
    char *detail = xstrfmt("%s · %s", session_repo(session), session_state_label(session_state(session)));
    doc_text(doc, x, w, detail, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(detail);
}
static void waiting(StatusScreen *s, Doc *doc, size_t index, int w) {
    StatusCard *c = s->cards[index];
    int pad = px(12), box = doc_box_begin(doc, 0, w, pad, theme.raise, theme.line, px(12));
    int iw = w - pad * 2; heading(s, doc, index, pad, iw); doc_space(doc, px(8));
    const Event *question = session_state(&c->session) == SESSION_WAITING ? transcript_pending_question(&c->transcript) : NULL;
    const char *text = question ? question->question ? question->question : question->text : json_str(json_get(c->session.raw, "lastText"));
    if (text) doc_text(doc, pad, iw, text, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (session_state(&c->session) == SESSION_FINDINGS) {
        doc_button(doc, pad, iw, "Decide findings →", BUTTON_BORDERED, STATUS_OPEN, status_action_add(s, index, -1), true);
    } else if (store_supports("message") && question) {
        bool can = status_can_answer(s, c);
        for (size_t i = 0; i < json_count(question->options); i++) {
            const char *label = json_str_nonempty(json_get(json_at(question->options, i), "label"));
            if (label) doc_button(doc, pad, iw, label, BUTTON_BORDERED, STATUS_OPTION, status_action_add(s, index, (int)i), can);
        }
        c->laid = true; c->rect = (RECT){ pad, doc->y, pad + iw, doc->y + px(36) }; doc_space(doc, px(40));
        c->reply_action = status_action_add(s, index, -1);
        doc_button(doc, pad, iw, "Send reply", BUTTON_PROMINENT, STATUS_SEND, c->reply_action, can && !str_empty(c->draft));
    }
    if (c->read_error) doc_notice(doc, pad, iw, c->read_error);
    if (c->error) doc_notice(doc, pad, iw, c->error);
    if (status_uncertain(s, c)) {
        doc_notice(doc, pad, iw, "The send outcome is unknown. Check the conversation before allowing another send.");
        doc_button(doc, pad, iw, "I checked the conversation; refresh", BUTTON_BORDERED, STATUS_RESOLVE, status_action_add(s, index, -1), !s->write && !s->check_read);
    }
    doc_box_end(doc, box, pad); doc_space(doc, px(10));
}
void status_layout(Screen *base, Doc *doc) {
    StatusScreen *s = (StatusScreen *)base; int w = doc->width;
    s->action_first = s->action_next; s->action_count = 0;
    for (size_t i = 0; i < s->card_count; i++) s->cards[i]->laid = false;
    if (!status_current(s)) { doc_notice(doc, 0, w, "The connection changed. Reopen Status on the current server."); return; }
    char *counts = xstrfmt("Need you  %u     Working  %u     Ready  %u", (unsigned)s->overview.waiting_count, (unsigned)s->overview.working_count, (unsigned)s->overview.ready);
    doc_text(doc, 0, w, counts, FONT_SUBHEADLINE, theme.ink, DT_WORDBREAK); free(counts); doc_space(doc, px(12));
    if (s->error) doc_notice(doc, 0, w, s->error);
    if (s->missing) doc_notice(doc, 0, w, "Some project lists are unavailable. Counts reflect the saved lists available here.");
    if (!s->loaded) doc_loading(doc, 0, w, "Loading Status…");
    if (s->overview.waiting_count) doc_section(doc, 0, w, "Waiting for you");
    for (size_t i = 0; i < s->overview.waiting_count; i++) {
        size_t index = card_index(s, &s->sessions[s->overview.waiting[i]]);
        if (index < s->card_count) waiting(s, doc, index, w);
    }
    if (s->overview.working_count) doc_section(doc, 0, w, "Working");
    for (size_t i = 0; i < s->overview.working_count; i++) {
        size_t index = card_index(s, &s->sessions[s->overview.working[i]]);
        if (index < s->card_count) { heading(s, doc, index, 0, w); doc_space(doc, px(10)); }
    }
    if (s->loaded && !s->error && !s->missing && !s->overview.waiting_count && !s->overview.working_count) doc_text(doc, 0, w, "All quiet. Nothing is running and nothing waits for you.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}
void status_controls_hide(StatusScreen *s) { for (size_t i = 0; i < s->card_count; i++) if (s->cards[i]->edit) ShowWindow(s->cards[i]->edit, SW_HIDE); }
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
    c->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, pane_hwnd(s->base.pane), NULL, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(c->edit, reply_proc, 0, (DWORD_PTR)s);
    theme_apply_control(c->edit);
    SendMessageW(c->edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Reply");
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
        SetParent(c->edit, pane_hwnd(base->pane)); EnableWindow(c->edit, status_can_answer(s, c));
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
