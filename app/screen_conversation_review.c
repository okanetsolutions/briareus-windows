#include "screen_conversation_internal.h"

// A missing one-shot review is terminal, but does not establish a successful verdict.
bool screen_conversation_review_removed(ConversationScreen *s, const Request *req) {
    const Session *last = screen_conversation_session(s);
    if (req->client != g_store.client || req->error.kind != API_HTTP || req->error.status != 404) return false;
    if (!str_eq(json_str(json_get(last->raw, "activity")), "review")) return false;
    if (!session_pull_number(last) || str_empty(session_repo(last))) return false;
    Session ended; session_copy(&ended, last); json_set_str(ended.raw, "status", "closed");
    if (s->has_snapshot) session_free(&s->snapshot);
    s->snapshot = ended; s->has_snapshot = s->removed = s->loaded = true;
    s->pending_full = s->pending_refresh = s->reloading = false;
    set_string(&s->error, NULL); poller_stop(&s->poller);
    KillTimer(pane_hwnd(s->base.pane), TIMER_WORKING);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    return true;
}
