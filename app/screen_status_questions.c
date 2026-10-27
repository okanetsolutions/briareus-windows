#include "screen_status_internal.h"

bool status_can_answer(StatusScreen *s, StatusCard *c) {
    if (!status_current(s) || !s->shown || !c->present || !c->confirmed) return false;
    if (s->write || s->check_read || c->uncertain || c->answered) return false;
    if ((double)GetTickCount64() < s->detail_retry_until) return false;
    return session_state(&c->session) == SESSION_WAITING && transcript_pending_question(&c->transcript) && store_supports("message");
}
void status_cancel_questions(StatusScreen *s) {
    request_cancel(&s->question_read); request_cancel(&s->check_read); s->next_question = 0;
    for (size_t i = 0; i < s->card_count; i++) s->cards[i]->confirmed = false;
}
static bool adopt(StatusScreen *s, StatusCard *c, Request *req) {
    if (!req->ok && req->error.retry_after > 0) {
        double until = (double)GetTickCount64() + req->error.retry_after * 1000;
        if (until > s->detail_retry_until) s->detail_retry_until = until;
    }
    c->confirmed = false;
    // A read can change what a still-rendered button means before the deferred repaint.
    s->action_first = s->action_next; s->action_count = 0;
    if (!status_current(s) || !c->present || !req->ok) return false;
    Session session;
    if (!session_parse(json_get(req->result, "session"), &session)) { set_string(&c->read_error, "Could not confirm the current question."); return false; }
    Json *identity = session_question_identity(&session);
    bool same = json_equal(identity, c->identity) && session_state(&session) == SESSION_WAITING;
    json_free(identity); session_free(&session);
    if (!same) { set_string(&c->read_error, "The conversation changed. Refresh to read its current question."); return false; }
    const Json *events = json_get(req->result, "events");
    if (!json_is_array(events)) { set_string(&c->read_error, "Could not read the current question."); return false; }
    transcript_append(&c->transcript, events);
    const Event *pending = transcript_pending_question(&c->transcript);
    if (pending && !json_equal(pending->raw, c->answered_question)) c->answered = false;
    char *key = xstrfmt("transcript:%s", session_id(&c->session));
    cache_append(g_store.cache, events, key); free(key);
    c->confirmed = true;
    set_string(&c->read_error, NULL);
    return true;
}
static void question_done(void *owner, Request *req) {
    StatusScreen *s = owner; StatusCard *c = s->cards[s->reading_card];
    if (!adopt(s, c, req) && !req->ok) request_error_into(&c->read_error, req);
    if (!req->ok && req->error.retry_after > 0) s->next_question = s->reading_card;
    pane_relayout(s->base.pane); status_questions_next(s);
}
void status_questions_next(StatusScreen *s) {
    if (!s->shown || !status_current(s) || s->question_read || !store_supports("session")) return;
    if ((double)GetTickCount64() < s->detail_retry_until) return;
    while (s->next_question < s->card_count) {
        size_t index = s->next_question++; StatusCard *c = s->cards[index];
        if (!c->present || session_state(&c->session) != SESSION_WAITING) continue;
        s->reading_card = index;
        Json *args = json_object(); json_set_str(args, "sessionId", session_id(&c->session)); json_set_num(args, "since", c->transcript.cursor);
        store_call("session", args, 0, s, question_done, 0, &s->question_read); return;
    }
}
static void write_done(void *owner, Request *req) {
    StatusScreen *s = owner; StatusCard *c = s->cards[s->sending_card];
    if (!status_current(s)) return;
    c->confirmed = false;
    if (req->ok) {
        c->answered = true;
        json_free(c->answered_question); c->answered_question = json_clone(s->send_question);
        char *trim = str_trim(c->draft ? c->draft : "");
        if (str_eq(trim, s->send_text)) {
            set_string(&c->draft, ""); if (c->edit) SetWindowTextW(c->edit, L"");
        }
        free(trim); set_string(&c->error, NULL);
    } else {
        request_error_into(&c->error, req); c->uncertain = request_outcome_unknown(req);
    }
    status_load(s); pane_relayout(s->base.pane);
}
static void check_done(void *owner, Request *req) {
    StatusScreen *s = owner; StatusCard *c = s->cards[s->sending_card];
    bool current = adopt(s, c, req);
    const Event *question = transcript_pending_question(&c->transcript);
    bool same = question && json_equal(question->raw, s->send_question) && json_equal(c->identity, s->send_identity);
    if (!current || !same) {
        if (!req->ok) request_error_into(&c->error, req);
        else set_string(&c->error, "The question changed or was answered. Your reply was not sent.");
        pane_relayout(s->base.pane); return;
    }
    if (!status_can_answer(s, c)) { pane_relayout(s->base.pane); return; }
    // Cancel the background question read before writing, so it cannot reconfirm a pre-send transcript.
    request_cancel(&s->question_read);
    Json *args = json_object(); json_set_str(args, "sessionId", session_id(&c->session)); json_set_str(args, "text", s->send_text);
    c->confirmed = false;
    store_call("message", args, 0, s, write_done, 0, &s->write); pane_relayout(s->base.pane);
}
void status_send(StatusScreen *s, size_t index, const char *text) {
    StatusCard *c = s->cards[index];
    if (!status_can_answer(s, c) || !text) return;
    char *trim = str_trim(text);
    if (!*trim) { free(trim); return; }
    const Event *question = transcript_pending_question(&c->transcript);
    s->sending_card = index; set_string(&s->send_text, trim); free(trim);
    json_free(s->send_question); s->send_question = json_clone(question->raw);
    json_free(s->send_identity); s->send_identity = json_clone(c->identity);
    set_string(&c->error, NULL);
    Json *args = json_object(); json_set_str(args, "sessionId", session_id(&c->session)); json_set_num(args, "since", c->transcript.cursor);
    store_call("session", args, 0, s, check_done, 0, &s->check_read); pane_relayout(s->base.pane);
}
