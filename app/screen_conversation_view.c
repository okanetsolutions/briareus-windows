#include "screen_conversation_internal.h"

static void header_button(HeaderInfo *info, wchar_t glyph, const char *label, int action, bool enabled, bool destructive);
static char *status_line(const Session *ss);

void screen_conversation_conversation_layout(Screen *base, Doc *doc) {
    ConversationScreen *s = (ConversationScreen *)base;
    int x = screen_conversation_column_x(doc->width), w = screen_conversation_column_w(doc->width);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); }
    if (s->write_error) {
        int box = doc_box_begin(doc, x, w, px(10), blend(theme.danger, theme.canvas, 0.15), theme.danger, px(8));
        doc_item(doc, box)->hover_fill = false;
        doc_notice(doc, x + px(12), w - px(24), s->write_error);
        // Only a failure that is not a refusal leaves the outcome unknown; a refusal changed nothing.
        if (s->uncertain) {
            doc_space(doc, px(4));
            doc_text(doc, x + px(12), w - px(24), "The action may have completed. Check the latest conversation before trying again.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
            doc_space(doc, px(8));
            doc_button(doc, x + px(12), 0, "Refresh and check outcome", BUTTON_BORDERED, ACT_REFRESH_OUTCOME, 0, !s->loading && !s->busy);
        } else {
            doc_space(doc, px(8));
            doc_button(doc, x + px(12), 0, "Dismiss", BUTTON_BORDERED, ACT_DISMISS_ERROR, 0, true);
        }
        doc_box_end(doc, box, px(10));
        doc_space(doc, px(12));
    }
    size_t visible = 0;
    for (size_t i = 0; i < s->transcript.count; i++) if (event_visible(&s->transcript.events[i])) visible++;
    if (!visible && !s->error) doc_text(doc, x, w, s->loaded ? "No messages yet." : "Waiting for the conversation\xE2\x80\xA6", FONT_FOOTNOTE, theme.muted, DT_LEFT);
    // The dashboard folds workspace preparation into one block per turn and each run of tool calls into another.
    bool prep_done = false;
    size_t i = 0;
    while (i < s->transcript.count) {
        const Event *e = &s->transcript.events[i];
        if (str_eq(e->kind, "user") || str_eq(e->kind, "result")) prep_done = false;
        if (!prep_done && screen_conversation_is_prep_line(e) && !screen_conversation_prep_closed(e)) {
            size_t j = i, n = 0;
            while (j < s->transcript.count && screen_conversation_is_prep_line(&s->transcript.events[j]) && !screen_conversation_prep_closed(&s->transcript.events[j])) { j++; n++; }
            bool open = screen_conversation_is_expanded(s, e->seq);
            char *summary = xstrfmt("Preparing workspace\xE2\x80\xA6 (%zu step%s)", n, n == 1 ? "" : "s");
            doc_space(doc, px(8));
            int top = doc->y;
            int inner_x = open ? x + px(12) : x, inner_w = open ? w - px(12) : w;
            screen_conversation_doc_summary(doc, inner_x, inner_w, summary, open, ACT_TOOL_TOGGLE, e->seq);
            free(summary);
            if (open) {
                for (size_t k = i; k < j; k++) {
                    const Event *line = &s->transcript.events[k];
                    char *text = str_eq(line->kind, "cmd") ? xstrfmt("$ %s", line->text) : xstrdup(line->text);
                    screen_conversation_ev_log(doc, inner_x, inner_w, text, theme.muted);
                    free(text);
                }
                RECT bar = { x, top, x + px(2), doc->y };
                doc_add(doc, &bar, screen_conversation_paint_left_border);
            }
            doc_space(doc, px(8));
            i = j;
            continue;
        }
        if (screen_conversation_is_prep_line(e) && screen_conversation_prep_closed(e)) prep_done = true;
        if (screen_conversation_is_tool(e)) {
            size_t j = i, n = 0;
            while (j < s->transcript.count && screen_conversation_is_tool(&s->transcript.events[j])) { j++; n++; }
            const Event *last = &s->transcript.events[j - 1];
            bool open = screen_conversation_is_expanded(s, e->seq);
            char *summary = xstrfmt("%zu step%s \xC2\xB7 %s", n, n == 1 ? "" : "s", screen_conversation_tool_title(last));
            doc_space(doc, px(8));
            int top = doc->y;
            int inner_x = open ? x + px(12) : x, inner_w = open ? w - px(12) : w;
            screen_conversation_doc_summary(doc, inner_x, inner_w, summary, open, ACT_TOOL_TOGGLE, e->seq);
            free(summary);
            if (open) {
                for (size_t k = i; k < j; k++) screen_conversation_layout_step(doc, inner_x, inner_w, &s->transcript.events[k]);
                RECT bar = { x, top, x + px(2), doc->y };
                doc_add(doc, &bar, screen_conversation_paint_left_border);
            }
            doc_space(doc, px(8));
            i = j;
            continue;
        }
        if (event_visible(e)) screen_conversation_layout_event(s, doc, x, w, e);
        i++;
    }
    // Why the session failed: the dashboard's `⚠ error` in the head, said where the transcript stops.
    const char *failure = json_str_nonempty(json_get(screen_conversation_session(s)->raw, "error"));
    if (failure) { char *why = xstrfmt("\xE2\x9A\xA0 %s", failure); doc_space(doc, px(10)); doc_notice_box(doc, x, w, why); free(why); }
    const Json *queued = session_queued(screen_conversation_session(s));
    for (size_t q = 0; q < json_count(queued); q++) {
        const char *text = json_str(json_get(json_at(queued, q), "text"));
        if (!text) text = "Message";
        doc_space(doc, px(10));
        int th = measure_text(doc->cv, text, w - px(28), FONT_BODY, DT_WORDBREAK);
        RECT br = { x, doc->y, x + w, doc->y + th + px(20) };
        doc_add(doc, &br, screen_conversation_paint_queued_box);
        RECT tr = { br.left + px(14), br.top + px(10), br.right - px(14), br.bottom - px(10) };
        doc_text_at(doc, &tr, text, FONT_BODY, theme.muted, DT_WORDBREAK | DT_EDITCONTROL);
        doc->y = br.bottom + px(4);
        bool removable = store_supports("drop_message") && !s->busy && !s->uncertain;
        int lh = px(18);
        RECT qr = { x, doc->y, x + w - px(60), doc->y + lh };
        doc_text_at(doc, &qr, "Queued for the next turn", FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (removable) {
            RECT rr = { x + w - px(60), doc->y, x + w, doc->y + lh };
            int ri = doc_text_at(doc, &rr, "Remove", FONT_CAPTION2, theme.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            doc_item(doc, ri)->action = ACT_DROP_QUEUED; doc_item(doc, ri)->arg = (intptr_t)q; doc_item(doc, ri)->hand = true;
        }
        doc->y += lh;
    }
    const Json *triage = session_held_triage(screen_conversation_session(s));
    if (triage && store_supports("complete_findings")) { doc_space(doc, px(14)); screen_conversation_layout_triage(s, doc, x, w, triage); }
    if (session_is_active(screen_conversation_session(s))) {
        doc_space(doc, px(12));
        int wi = doc_custom(doc, x, w, px(24), screen_conversation_paint_working, s, NULL, 0, 0);
        doc_item(doc, wi)->id = 2;
    }
    doc_space(doc, px(30));
    RECT anchor = { x, doc->y, x + w, doc->y + 1 };
    int ai = doc_add(doc, &anchor, NULL); doc_item(doc, ai)->id = 1;
    doc->y += 1;
}

// MARK: - Header and menu

static void header_button(HeaderInfo *info, wchar_t glyph, const char *label, int action, bool enabled, bool destructive) {
    if (info->button_count >= HEADER_BUTTONS) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = glyph; b->action = action; b->enabled = enabled; b->destructive = destructive;
    snprintf(b->label, sizeof b->label, "%s", label);
}

/// `#chat-sub`: provider · model · effort · state · 🔁 loop round · branch · tokens · cost · session id · the pull request.
static char *status_line(const Session *ss) {
    Str sub; str_init(&sub);
    const char *parts[3] = { session_provider(ss), session_model(ss), json_str_nonempty(json_get(ss->raw, "effort")) };
    for (int i = 0; i < 3; i++) if (parts[i]) str_appendf(&sub, "%s%s", sub.len ? " \xC2\xB7 " : "", parts[i]);
    const char *state = str_eq(session_status(ss), "idle") && json_bool_is(json_get(ss->raw, "awaitingAnswer"), true) ? "waiting" : session_status(ss);
    str_appendf(&sub, "%s%s", sub.len ? " \xC2\xB7 " : "", state);
    const Json *loop = json_get(ss->raw, "reviewLoop");
    double round;
    if (json_is_object(loop)) { if (json_num(json_get(loop, "rounds"), &round)) str_appendf(&sub, " \xC2\xB7 \xF0\x9F\x94\x81 loop round %d", (int)round); else str_appendz(&sub, " \xC2\xB7 \xF0\x9F\x94\x81 loop"); }
    const char *branch = json_str_nonempty(json_get(ss->raw, "branch"));
    if (branch) str_appendf(&sub, " \xC2\xB7 %s", branch);
    bool browser_up = false;
    if (browser_session_on(ss->raw, &browser_up)) str_appendz(&sub, browser_up ? " \xC2\xB7 \xF0\x9F\x8C\x90 browser" : " \xC2\xB7 \xF0\x9F\x8C\x90 browser starts next turn");
    double in = 0, out = 0, cost;
    bool has_in = json_num(json_get(ss->raw, "inputTokens"), &in), has_out = json_num(json_get(ss->raw, "outputTokens"), &out);
    if (has_in || has_out) { char *tk = format_tokens(in + out); str_appendf(&sub, " \xC2\xB7 %s tok", tk); free(tk); }
    if (json_num(json_get(ss->raw, "costUsd"), &cost)) { char *c = format_cost(cost); str_appendf(&sub, " \xC2\xB7 %s", c); free(c); }
    str_appendf(&sub, " \xC2\xB7 session %s", session_id(ss));
    const Json *pr = json_get(ss->raw, "prStatus");
    double number;
    if (json_is_object(pr) && json_num(json_get(pr, "number"), &number)) {
        const char *pstate = json_str(json_get(pr, "state"));
        bool draft = json_bool_is(json_get(pr, "draft"), true);
        const char *light = str_eq(pstate, "merged") ? "\xF0\x9F\x9F\xA3" : str_eq(pstate, "closed") ? "\xF0\x9F\x94\xB4" : draft ? "\xE2\x9A\xAA" : "\xF0\x9F\x9F\xA2";
        str_appendf(&sub, " \xC2\xB7 %s PR #%d %s", light, (int)number, draft && str_eq(pstate, "open") ? "draft" : pstate ? pstate : "");
        int failed = json_int_or(json_get(json_get(pr, "checks"), "failed"), 0), pending = json_int_or(json_get(json_get(pr, "checks"), "pending"), 0), passed = json_int_or(json_get(json_get(pr, "checks"), "passed"), 0);
        if (failed) str_appendf(&sub, " \xC2\xB7 \xE2\x9C\x97%d", failed);
        else if (pending) str_appendf(&sub, " \xC2\xB7 \xE2\x80\xA6%d", pending);
        else if (passed) str_appendf(&sub, " \xC2\xB7 \xE2\x9C\x93%d", passed);
    }
    return str_detach(&sub);
}

void screen_conversation_conversation_header(Screen *base, HeaderInfo *info) {
    ConversationScreen *s = (ConversationScreen *)base;
    const Session *ss = screen_conversation_session(s);
    snprintf(info->title, sizeof info->title, "%s", session_display_title(ss));
    char *sub = status_line(ss);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub);
    free(sub);
    if (store_supports("rename")) info->title_action = ACT_MENU_ITEM + MENU_RENAME;
    bool can = !s->busy && !s->uncertain;
    bool closed = str_eq(session_status(ss), "closed");
    if (browser_offered()) {
        bool running = false, on = browser_session_on(ss->raw, &running);
        header_button(info, 0xE774, on && running ? "\xF0\x9F\x8C\x90 Browser \xE2\x97\x8F" : "\xF0\x9F\x8C\x90 Browser", ACT_MENU_ITEM + MENU_BROWSER, true, false);
    }
    // An admin token's; the session record carries the settings, so the button says when deliveries are on.
    if (store_supports("session_webhook")) {
        bool armed = json_bool_is(json_get(json_get(ss->raw, "webhook"), "armed"), true);
        header_button(info, 0xE945, armed ? "\xE2\x9A\xA1 Webhook \xE2\x97\x8F" : "\xE2\x9A\xA1 Webhook", ACT_MENU_ITEM + MENU_WEBHOOK, true, false);
    }
    if (store_supports("cancel") && session_is_active(ss)) header_button(info, 0xE71A, "\xE2\x8F\xB9 Stop", ACT_MENU_ITEM + MENU_STOP, can, false);
    if (store_supports("reopen") && closed) header_button(info, 0xE7A7, "\xE2\x9F\xB3 Reopen", ACT_MENU_ITEM + MENU_REOPEN, can, false);
    if (store_supports("close") && !closed) header_button(info, 0xE8BB, "Close", ACT_MENU_ITEM + MENU_CLOSE, can, false);
    if (store_supports("delete")) header_button(info, 0xE74D, "\xF0\x9F\x97\x91 Delete", ACT_MENU_ITEM + MENU_DELETE, can, true);
}

void screen_conversation_menu_choice(ConversationScreen *s, int chosen) {
    const Session *ss = screen_conversation_session(s);
    int number = session_pull_number(ss);
    const char *repo = session_repo(ss);
    if ((chosen == MENU_CHANGES || chosen == MENU_PULL) && !(number && repo)) return;
    switch (chosen) {
    case MENU_CHANGES: { Project p = { xstrdup(repo), NULL }; app_push_from(&s->base, pull_files_screen_new(&p, number)); project_free(&p); break; }
    case MENU_PULL: { Project p = { xstrdup(repo), NULL }; app_push_from(&s->base, pull_detail_screen_new(&p, number, NULL, NULL)); project_free(&p); break; }
    case MENU_COPY: break;
    case MENU_BROWSER: browser_open(ss); break;
    case MENU_WEBHOOK: app_push_from(&s->base, webhook_screen_new(ss)); break;
    case MENU_LOOP_OFF:
    case MENU_LOOP_ON: { Json *extra = json_object(); json_set_bool(extra, "on", chosen == MENU_LOOP_ON); screen_conversation_mutate(s, "review_loop", extra); break; }
    case MENU_RENAME: {
        s->renaming = true; s->dialog_open = true;
        char *title = dialog_rename(app_dialog_owner(), session_display_title(ss));
        s->renaming = false; s->dialog_open = false;
        if (title) { char *trimmed = str_trim(title); if (*trimmed) { Json *extra = json_object(); json_set_str(extra, "title", title); screen_conversation_mutate(s, "rename", extra); } free(trimmed); free(title); }
        break;
    }
    case MENU_STOP: screen_conversation_confirm_and_mutate(s, "cancel"); break;
    case MENU_CLOSE: screen_conversation_confirm_and_mutate(s, "close"); break;
    case MENU_REOPEN: screen_conversation_confirm_and_mutate(s, "reopen"); break;
    case MENU_DELETE: screen_conversation_confirm_and_mutate(s, "delete"); break;
    }
}

void screen_conversation_conversation_action(Screen *base, int action, intptr_t arg, POINT pt) {
    ConversationScreen *s = (ConversationScreen *)base;
    if (action > ACT_MENU_ITEM && action <= ACT_MENU_ITEM + MENU_WEBHOOK) { screen_conversation_menu_choice(s, action - ACT_MENU_ITEM); return; }
    switch (action) {
    case ACT_ANSWER: {
        int seq = (int)(arg >> 8), index = (int)(arg & 0xFF);
        for (size_t i = 0; i < s->transcript.count; i++) {
            const Event *e = &s->transcript.events[i];
            if (e->seq != seq) continue;
            const char *label = json_str(json_get(json_at(e->options, (size_t)index), "label"));
            if (label) { screen_conversation_set_composer_text(s, label); SetFocus(s->composer); }
            break;
        }
        break;
    }
    case ACT_TOOL_TOGGLE: screen_conversation_toggle_expanded(s, (int)arg); pane_relayout(base->pane); break;
    case ACT_DROP_QUEUED: { Json *extra = json_object(); json_set_num(extra, "index", (double)arg); screen_conversation_mutate(s, "drop_message", extra); break; }
    case ACT_TRIAGE_DECISION: {
        const Json *triage = session_held_triage(screen_conversation_session(s));
        size_t index = (size_t)(arg / 4); int decision = (int)(arg % 4);
        const char *key = json_str(json_get(json_at(json_get(triage, "findings"), index), "key"));
        if (key && decision >= 0 && decision < FINDING_DECISION_COUNT) { screen_conversation_set_decision(s, key, finding_decision_ids[decision]); pane_relayout(base->pane); }
        break;
    }
    case ACT_TRIAGE_NOTE: {
        s->dialog_open = true;
        char *note = dialog_rename(app_dialog_owner(), s->triage_note ? s->triage_note : "");
        s->dialog_open = false;
        if (note) { set_string(&s->triage_note, note); free(note); pane_relayout(base->pane); }
        break;
    }
    case ACT_TRIAGE_COMPLETE: screen_conversation_complete_triage(s, (size_t)arg); break;
    case ACT_FINDING_LINK: {
        const Json *triage = session_held_triage(screen_conversation_session(s));
        const char *url = json_str(json_get(json_at(json_get(triage, "findings"), (size_t)arg), "url"));
        open_web_url(url);
        break;
    }
    case ACT_REFRESH_OUTCOME: s->uncertain = false; s->reloading = true; set_string(&s->write_error, NULL); screen_conversation_refresh(s, false); pane_relayout(base->pane); break;
    case ACT_DISMISS_ERROR: set_string(&s->write_error, NULL); pane_relayout(base->pane); break;
    case ACT_REOPEN: screen_conversation_confirm_and_mutate(s, "reopen"); break;
    }
}
