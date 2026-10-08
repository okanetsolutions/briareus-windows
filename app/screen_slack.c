// Global operator Slack inbox. No session, disk cache, attachment transport or live SSE.
#include "dialogs.h"
#include "screens.h"
#include "slack_inbox.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const OPERATIONS[] = { "slack_workspaces", "slack_conversations", "slack_people", "slack_conversation",
    "slack_history", "slack_open_dm", "slack_send", "slack_read", "slack_thread" };
static SlackScreen *screens;
static void load(SlackScreen *s, int tag);
static void refresh(Screen *base);
static void changed(SlackScreen *s) {
    if (!s->base.pane) return;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_footer_changed(s->base.pane);
}
bool slack_inbox_supports(const char *operation) {
    // The catalog is necessary but cannot weaken the local private-inbox admin boundary.
    return g_store.has_device && str_eq(g_store.device.permission, "admin") && store_supports(operation);
}
bool slack_inbox_offered(void) { return slack_inbox_supports("slack_workspaces"); }
static unsigned access_mask(void) {
    unsigned mask = 0;
    for (size_t i = 0; i < sizeof OPERATIONS / sizeof *OPERATIONS; i++) if (slack_inbox_supports(OPERATIONS[i])) mask |= 1u << i;
    return mask;
}
static SlackDraft *draft(SlackScreen *s) {
    return !str_empty(s->workspace) && !str_empty(s->channel) ? slack_draft(&s->state, s->workspace, s->channel, s->thread) : NULL;
}
static void composer_set(SlackScreen *s) {
    if (!s->composer) return;
    SlackDraft *d = draft(s); wchar_t *w = utf8_to_wide(d ? d->text : "");
    s->filling = true; SetWindowTextW(s->composer, w); s->filling = false; free(w);
    EnableWindow(s->composer, d && !d->sending && slack_inbox_supports("slack_send"));
}
static void view_clear(SlackScreen *s) {
    for (size_t i = 0; i < s->view_count; i++) free(s->view[i].ts);
    free(s->view); s->view = NULL; s->view_count = 0;
}
static void cancel(SlackScreen *s) {
    if (s->requests[TAG_SEND]) { SlackDraft *d = draft(s); if (d) slack_draft_cancel(d); }
    for (int i = 0; i < TAG_COUNT; i++) request_cancel(&s->requests[i]);
    slack_state_advance(&s->state); s->empty_pages = 0;
    if (s->base.pane) { KillTimer(pane_hwnd(s->base.pane), TIMER_EMPTY); KillTimer(pane_hwnd(s->base.pane), TIMER_READ); }
}
static void private_clear(SlackScreen *s) {
    cancel(s); slack_state_clear(&s->state); slack_page_clear(&s->page); view_clear(s);
    json_free(s->workspaces); s->workspaces = json_array();
    json_free(s->conversations); s->conversations = json_array();
    json_free(s->people); s->people = json_array(); json_free(s->detail); s->detail = NULL;
    set_string(&s->workspace, NULL); set_string(&s->channel, NULL); set_string(&s->thread, NULL);
    set_string(&s->conv_cursor, NULL); set_string(&s->people_cursor, NULL); set_string(&s->search, NULL);
    s->loaded = false; s->history_loaded = false; s->directory = false; s->cooldown = 0; s->read_failed = false;
    composer_set(s);
}
static bool sync_access(SlackScreen *s) {
    unsigned mask = access_mask();
    if (s->account != g_store.client || !str_eq(s->device, g_store.has_device ? g_store.device.id : NULL) || s->access != mask) {
        private_clear(s);
        api_client_release(s->account); s->account = api_client_retain(g_store.client);
        set_string(&s->device, g_store.has_device ? g_store.device.id : NULL); s->access = mask;
        set_string(&s->error, "Slack account or access changed. Private inbox state was cleared; refresh to reload.");
    }
    return store_connected() && slack_inbox_offered();
}
void slack_inbox_store_changed(void) {
    for (SlackScreen *s = screens; s; s = s->next) { sync_access(s); changed(s); }
}
static Json *arguments(SlackScreen *s) {
    Json *args = json_object();
    if (s->workspace) json_set_str(args, "id", s->workspace);
    if (s->channel) json_set_str(args, "channel", s->channel);
    return args;
}
static bool ready(SlackScreen *s, const char *operation) {
    return sync_access(s) && slack_inbox_supports(operation) && GetTickCount64() >= s->cooldown;
}
static void error_from(SlackScreen *s, const Request *req) {
    char *message = request_error_or_unexpected(req);
    if (req->error.status == 429) {
        double delay = req->error.retry_after > 0 ? req->error.retry_after : 30;
        // Bound conversion while honoring a server cooldown longer than the default.
        if (delay > 86400 * 30) delay = 86400 * 30;
        uint64_t now = GetTickCount64(), deadline = now + (uint64_t)(delay * 1000);
        if (deadline > s->cooldown) s->cooldown = deadline;
        double remaining = (double)((s->cooldown - now + 999) / 1000);
        free(message); message = xstrfmt("Slack is rate limiting this workspace. Wait %.0f seconds, then retry the read or action yourself.", remaining);
    } else if (req->error.status == 403) {
        char *detail = xstrfmt("Slack access was denied. Check the Admin device token and deployed routes. %s", message);
        free(message); message = detail;
    }
    set_string(&s->error, message); free(message);
}
static void navigate(SlackScreen *s, const char *workspace, const char *channel, const char *thread) {
    // Copy first: choices often borrow strings from state that cancellation/clear replaces.
    char *w = xstrdup(workspace), *c = xstrdup(channel), *t = xstrdup(thread);
    bool workspace_changed = !str_eq(s->workspace, w);
    cancel(s); slack_page_clear(&s->page); view_clear(s);
    set_string(&s->workspace, w); set_string(&s->channel, c); set_string(&s->thread, t);
    free(w); free(c); free(t);
    json_free(s->detail); s->detail = NULL; s->directory = false; s->history_loaded = false; s->read_failed = false;
    if (workspace_changed) {
        json_free(s->conversations); s->conversations = json_array(); json_free(s->people); s->people = json_array();
        set_string(&s->conv_cursor, NULL); set_string(&s->people_cursor, NULL);
    }
    set_string(&s->error, NULL); composer_set(s);
    if (s->base.pane) pane_scroll_to_top(s->base.pane);
    if (workspace_changed) { load(s, TAG_CONVERSATIONS); load(s, TAG_PEOPLE); }
    if (s->channel) { load(s, TAG_DETAIL); load(s, TAG_HISTORY); }
    changed(s);
}
static bool directory_page(SlackScreen *s, Request *req, bool people) {
    const char *key = people ? "people" : "conversations";
    const Json *rows = json_get(req->result, key); const char *cursor = json_str(json_get(req->result, "nextCursor"));
    if (!json_is_array(rows) || !cursor) return false;
    char **slot = people ? &s->people_cursor : &s->conv_cursor;
    if (*cursor && str_eq(cursor, *slot)) { set_string(&s->error, "Slack repeated a pagination cursor. Refresh before reading more."); set_string(slot, NULL); return true; }
    slack_rows_merge(people ? s->people : s->conversations, rows); set_string(slot, cursor);
    // Schedule, rather than recurse in completion: empty pages can still lead to results.
    unsigned bit = 1u << (people ? TAG_PEOPLE : TAG_CONVERSATIONS);
    s->empty_pages &= ~bit;
    if (!json_count(rows) && *cursor) {
        s->empty_pages |= bit;
        if (s->shown) SetTimer(pane_hwnd(s->base.pane), TIMER_EMPTY, 100, NULL);
    }
    return true;
}
void slack_inbox_done(void *owner, Request *req) {
    SlackScreen *s = owner;
    if (!sync_access(s) || req->client != s->account || !slack_inbox_supports(req->operation)
        || !slack_state_current(&s->state, (uint64_t)req->arg)) return;
    if (req->tag == TAG_SEND) {
        SlackDraft *d = slack_draft(&s->state, json_str(json_get(req->args, "id")), json_str(json_get(req->args, "channel")), json_str(json_get(req->args, "threadTs")));
        SlackSendResult result = slack_draft_finish(d, req->ok, api_error_is_refusal(&req->error), req->result);
        if (result == SLACK_SEND_WORKSPACE_CHANGED) {
            private_clear(s); set_string(&s->error, "Message confirmed sent. The workspace changed; refresh the workspace list. Do not resend this message.");
            load(s, TAG_WORKSPACES);
        } else if (result == SLACK_SEND_CONFIRMED) {
            Json *m = json_clone(json_get(req->result, "message"));
            if (!json_is_object(m)) { json_free(m); m = json_object(); }
            json_set_str(m, "ts", json_str(json_get(req->result, "ts")));
            if (!json_str_nonempty(json_get(m, "text"))) json_set_str(m, "text", json_str(json_get(req->args, "text")));
            if (!str_empty(d->thread)) json_set_str(m, "thread_ts", d->thread);
            slack_message_merge(&s->state, d->workspace, d->channel, m); json_free(m);
            set_string(&s->error, NULL); composer_set(s); if (s->base.pane) pane_scroll_to_bottom(s->base.pane);
        } else {
            error_from(s, req);
            if (req->error.status == 403) {
                // The server may learn of an admin downgrade before discovery is refreshed.
                char *message = xstrdup(s->error); private_clear(s); set_string(&s->error, message); free(message);
            }
            composer_set(s);
        }
        changed(s); return;
    }
    if (!req->ok) {
        error_from(s, req);
        if (req->tag == TAG_READ) s->read_failed = true;
        // Rotation/removal invalidates all private data fetched with those workspace credentials.
        if (req->error.status == 409 || req->error.status == 404 || req->error.status == 403
            || (req->error.message && strstr(req->error.message, "refused the workspace"))) {
            char *message = xstrdup(s->error); private_clear(s); set_string(&s->error, message); free(message);
        }
        changed(s); return;
    }
    bool parsed = true;
    switch (req->tag) {
    case TAG_WORKSPACES: {
        const Json *rows = json_get(req->result, "workspaces"); parsed = json_is_array(rows);
        if (parsed) {
            json_free(s->workspaces); s->workspaces = json_clone(rows); s->loaded = true;
            if (s->workspace) {
                bool found = false;
                for (size_t i = 0; i < json_count(rows); i++) {
                    char *id = slack_workspace_id(json_at(rows, i)); found |= str_eq(id, s->workspace); free(id);
                }
                if (!found) { private_clear(s); s->loaded = true; json_free(s->workspaces); s->workspaces = json_clone(rows); }
            }
        }
        break;
    }
    case TAG_CONVERSATIONS: parsed = directory_page(s, req, false); break;
    case TAG_PEOPLE: parsed = directory_page(s, req, true); break;
    case TAG_DETAIL: {
        const Json *row = json_get(req->result, "conversation");
        parsed = json_is_object(row) && str_eq(json_str(json_get(row, "id")), s->channel);
        if (parsed) { json_free(s->detail); s->detail = json_clone(row); }
        break;
    }
    case TAG_HISTORY:
        parsed = slack_page_merge(&s->state, &s->page, s->workspace, s->channel, !str_empty(s->thread), req->result);
        if (parsed) {
            s->history_loaded = true;
            if (s->page.stalled) set_string(&s->error, "Slack history pagination made no progress. Refresh before reading more.");
            s->empty_pages &= ~(1u << TAG_HISTORY);
            if (!json_count(json_get(req->result, "messages")) && !str_empty(s->page.cursor) && s->page.more) {
                s->empty_pages |= 1u << TAG_HISTORY;
                if (s->shown) SetTimer(pane_hwnd(s->base.pane), TIMER_EMPTY, 100, NULL);
            }
        }
        break;
    case TAG_DM: {
        const Json *row = json_get(req->result, "conversation"); const char *id = json_str_nonempty(json_get(row, "id"));
        parsed = id != NULL;
        if (parsed) { Json *rows = json_array(); json_array_push(rows, json_clone(row)); slack_rows_merge(s->conversations, rows); json_free(rows); navigate(s, s->workspace, id, NULL); }
        break;
    }
    case TAG_READ:
        parsed = json_bool_is(json_get(req->result, "ok"), true);
        if (parsed) slack_read_confirm(slack_read(&s->state, s->workspace, s->channel), json_str(json_get(req->args, "ts")));
        break;
    }
    if (!parsed) { error_from(s, req); if (req->tag == TAG_READ) s->read_failed = true; }
    changed(s);
}
static void call(SlackScreen *s, int tag, const char *operation, Json *args) {
    uint64_t generation = s->state.generation;
    if (s->requests[tag] || !ready(s, operation) || !slack_state_current(&s->state, generation)) { json_free(args); return; }
    Request *req = store_call(operation, args, 0, s, slack_inbox_done, tag, &s->requests[tag]);
    req->arg = (intptr_t)s->state.generation;
}
static void load(SlackScreen *s, int tag) {
    if (!s->shown) return;
    Json *args = arguments(s); const char *operation = NULL;
    switch (tag) {
    case TAG_WORKSPACES: operation = "slack_workspaces"; break;
    case TAG_CONVERSATIONS:
        if (!s->workspace) break;
        operation = "slack_conversations"; json_set_str(args, "types", "public_channel,private_channel,im,mpim");
        if (!str_empty(s->conv_cursor)) json_set_str(args, "cursor", s->conv_cursor);
        break;
    case TAG_PEOPLE:
        if (!s->workspace) break;
        operation = "slack_people"; if (!str_empty(s->people_cursor)) json_set_str(args, "cursor", s->people_cursor); break;
    case TAG_DETAIL: if (s->channel) operation = "slack_conversation"; break;
    case TAG_HISTORY:
        if (!s->channel) break;
        operation = str_empty(s->thread) ? "slack_history" : "slack_thread";
        if (!str_empty(s->thread)) json_set_str(args, "ts", s->thread);
        if (!str_empty(s->page.cursor)) json_set_str(args, "cursor", s->page.cursor);
        if (!str_empty(s->page.oldest)) json_set_str(args, "oldest", s->page.oldest);
        if (!str_empty(s->page.latest)) json_set_str(args, "latest", s->page.latest);
        break;
    }
    if (operation) call(s, tag, operation, args); else json_free(args);
}
static const Json *conversation(SlackScreen *s) {
    if (s->detail) return s->detail;
    for (size_t i = 0; i < json_count(s->conversations); i++) {
        const Json *row = json_at(s->conversations, i);
        if (str_eq(json_str(json_get(row, "id")), s->channel)) return row;
    }
    return NULL;
}
static char *destination_name(SlackScreen *s) {
    char *name = slack_conversation_name(conversation(s), s->people);
    const char *label = "Slack";
    for (size_t i = 0; i < json_count(s->workspaces); i++) {
        const Json *row = json_at(s->workspaces, i); char *id = slack_workspace_id(row);
        bool selected = str_eq(id, s->workspace); free(id);
        if (selected) { label = json_str_or(json_get(row, "label"), "Slack"); break; }
    }
    char *title = xstrfmt("%s [%s] / %s [%s]%s%s", label, s->workspace ? s->workspace : "", name,
        s->channel ? s->channel : "", str_empty(s->thread) ? "" : " / thread ", s->thread ? s->thread : "");
    free(name); return title;
}
static void layout(Screen *base, Doc *doc) {
    SlackScreen *s = (SlackScreen *)base; view_clear(s);
    int x = px(12), w = doc->width - px(24); doc_space(doc, px(12));
    if (!sync_access(s)) {
        doc_empty_state(doc, x, w, 0xE8F2, "Slack inbox unavailable", "An Admin token and a server with the Slack inbox routes are required."); return;
    }
    if (s->error) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); }
    if (!s->workspace) {
        doc_section(doc, x, w, "Workspaces");
        for (size_t i = 0; i < json_count(s->workspaces); i++) {
            const Json *row = json_at(s->workspaces, i); char *id = slack_workspace_id(row);
            char *title = xstrfmt("%s · %s", json_str_or(json_get(row, "label"), "Workspace"), json_str_or(json_get(row, "team"), id ? id : ""));
            doc_button(doc, x, w, title, BUTTON_BORDERED, ACT_WORKSPACE, (intptr_t)i, id != NULL && slack_inbox_supports("slack_conversations"));
            free(title); free(id); doc_space(doc, px(6));
        }
        if (s->requests[TAG_WORKSPACES]) doc_loading(doc, x, w, "Loading workspaces…");
        else if (s->loaded && !json_count(s->workspaces)) doc_empty_state(doc, x, w, 0xE8F2, "No Slack workspaces", "Connect a workspace under Settings → Slack workspaces; no project assignment is needed.");
        return;
    }
    if (!s->channel || s->directory) {
        ButtonSpec buttons[] = {
            { 0, "Workspaces", BUTTON_PLAIN, ACT_BACK, 0, true },
            { 0, s->directory ? "Conversations" : "People / Open DM", BUTTON_BORDERED, ACT_PEOPLE, 0, slack_inbox_supports("slack_people") },
        };
        doc_button_row(doc, x, w, buttons, 2); doc_space(doc, px(10));
        if (s->directory) {
            doc_button(doc, x, w, s->search ? s->search : "Find people…", BUTTON_BORDERED, ACT_PERSON, -1, true);
            for (size_t i = 0; i < json_count(s->people); i++) {
                const Json *row = json_at(s->people, i); const char *id = json_str_nonempty(json_get(row, "id"));
                if (!id || json_bool_is(json_get(row, "deleted"), true) || json_bool_is(json_get(row, "is_bot"), true)) continue;
                char *name = slack_person_name(s->people, id);
                if (str_empty(s->search) || str_icontains(name, s->search) || str_icontains(id, s->search)) {
                    char *title = xstrfmt("%s (%s) · Open DM", name, id);
                    doc_button(doc, x, w, title, BUTTON_PLAIN, ACT_PERSON, (intptr_t)i, slack_inbox_supports("slack_open_dm") && !s->requests[TAG_DM]); free(title);
                }
                free(name);
            }
            if (!str_empty(s->people_cursor)) doc_button(doc, x, w, "More people", BUTTON_BORDERED, ACT_PEOPLE_MORE, 0, !s->requests[TAG_PEOPLE]);
            if (s->requests[TAG_PEOPLE]) doc_loading(doc, x, w, "Loading people…");
        } else {
            static const char *const groups[] = { "Public channels", "Private channels", "Direct messages", "Group DMs" };
            for (int group = 0; group < 4; group++) {
                doc_section(doc, x, w, groups[group]);
                for (size_t i = 0; i < json_count(s->conversations); i++) {
                    const Json *row = json_at(s->conversations, i);
                    int kind = json_bool_is(json_get(row, "is_mpim"), true) ? 3 : json_bool_is(json_get(row, "is_im"), true) ? 2 : json_bool_is(json_get(row, "is_private"), true) ? 1 : 0;
                    if (kind != group) continue;
                    char *name = slack_conversation_name(row, s->people);
                    doc_button(doc, x, w, name, BUTTON_PLAIN, ACT_CHANNEL, (intptr_t)i, slack_inbox_supports("slack_history")); free(name);
                }
            }
            if (!str_empty(s->conv_cursor)) doc_button(doc, x, w, "More conversations", BUTTON_BORDERED, ACT_CONV_MORE, 0, !s->requests[TAG_CONVERSATIONS]);
            if (s->requests[TAG_CONVERSATIONS]) doc_loading(doc, x, w, "Loading conversations…");
        }
        return;
    }
    doc_button(doc, x, 0, str_empty(s->thread) ? "← Conversations" : "← Conversation", BUTTON_PLAIN, ACT_BACK, 0, true);
    char *dest = destination_name(s); doc_text(doc, x, w, dest, FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK); free(dest);
    if (s->detail) {
        const char *topic = json_str(json_get(json_get(s->detail, "topic"), "value"));
        const char *purpose = json_str(json_get(json_get(s->detail, "purpose"), "value"));
        if (!str_empty(topic)) doc_text(doc, x, w, topic, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        if (!str_empty(purpose)) doc_text(doc, x, w, purpose, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    }
    doc_space(doc, px(10));
    if (s->page.more && str_empty(s->thread)) doc_button(doc, x, 0, "Older messages", BUTTON_BORDERED, ACT_OLDER, 0, !s->requests[TAG_HISTORY]);
    for (size_t i = 0; i < s->state.message_count; i++) {
        const SlackMessage *m = &s->state.messages[i];
        if (!slack_message_in_thread(m, s->workspace, s->channel, s->thread)) continue;
        int top = doc->y; int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
        char *author = slack_person_name(s->people, json_str(json_get(m->raw, "user")));
        const char *bot = json_str_nonempty(json_get(json_get(m->raw, "bot_profile"), "name"));
        if (bot) set_string(&author, bot);
        char *heading = xstrfmt("%s · %s%s", author, m->ts, json_is_object(json_get(m->raw, "edited")) ? " · edited" : "");
        doc_text(doc, x + px(10), w - px(20), heading, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); free(author); free(heading);
        char *text = slack_message_text(m->raw, s->people); doc_markdown(doc, x + px(10), w - px(20), text, FONT_BODY); free(text);
        if (str_empty(s->thread)) {
            char *label = xstrfmt("Thread / reply · %d replies", json_int_or(json_get(m->raw, "reply_count"), 0));
            doc_button(doc, x + px(10), 0, label, BUTTON_PLAIN, ACT_THREAD, (intptr_t)i, slack_inbox_supports("slack_thread")); free(label);
        }
        doc_box_end(doc, box, px(10));
        s->view = xrealloc(s->view, (s->view_count + 1) * sizeof *s->view);
        s->view[s->view_count].rc = (RECT){x, top, x + w, doc->y}; s->view[s->view_count++].ts = xstrdup(m->ts);
        doc_space(doc, px(8));
    }
    if (s->page.more && !str_empty(s->thread)) doc_button(doc, x, 0, "More replies", BUTTON_BORDERED, ACT_OLDER, 0, !s->requests[TAG_HISTORY]);
    if (s->requests[TAG_HISTORY]) doc_loading(doc, x, w, "Loading messages…");
    else if (s->history_loaded && !s->view_count) doc_text(doc, x, w, "No messages on this page.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    SlackDraft *d = draft(s);
    if (d && d->uncertain) {
        doc_notice_box(doc, x, w, "The send may have reached Slack. Your draft is kept. Refresh and inspect this destination's history before choosing to resend; the app will never retry it automatically.");
        doc_button(doc, x, 0, "I inspected history — allow another send", BUTTON_BORDERED, ACT_RECOVER, 0, true);
    }
}
static void header(Screen *base, HeaderInfo *info) {
    SlackScreen *s = (SlackScreen *)base;
    snprintf(info->title, sizeof info->title, "Slack inbox");
    snprintf(info->subtitle, sizeof info->subtitle, "Admin · Human replies · Refresh for updates");
    info->buttons[info->button_count++] = (HeaderButton){ 0xE72C, ACT_REFRESH, slack_inbox_offered(), "Refresh Slack inbox", "Refresh" };
    info->buttons[info->button_count++] = (HeaderButton){ 0xE774, ACT_WEB, true, "Open Slack in your browser", "Slack web" };
    if (s->workspace) snprintf(info->subtitle, sizeof info->subtitle, "Workspace %s%s", s->workspace, s->directory ? " · People" : "");
}
static void send_message(SlackScreen *s) {
    if (!ready(s, "slack_send") || !s->channel || s->directory) return;
    SlackDraft *d = draft(s);
    if (!d || !slack_draft_begin(d)) { set_string(&s->error, "Write 1–8000 characters. An uncertain send must be checked in history before another send."); changed(s); return; }
    Json *args = arguments(s); json_set_str(args, "text", d->sent_text);
    if (!str_empty(d->thread)) json_set_str(args, "threadTs", d->thread);
    call(s, TAG_SEND, "slack_send", args); composer_set(s); changed(s);
}
void slack_inbox_recover_confirmed(SlackScreen *s, const Json *destination, uint64_t generation) {
    // A modal confirmation dispatches completions which can clear drafts or destroy the screen.
    SlackScreen *live = screens; while (live && live != s) live = live->next;
    if (!live) return;
    if (!ready(s, "slack_send") || !slack_state_current(&s->state, generation)
        || !str_eq(s->workspace, json_str(json_get(destination, "id")))
        || !str_eq(s->channel, json_str(json_get(destination, "channel")))
        || !str_eq(s->thread, json_str(json_get(destination, "threadTs")))) { changed(s); return; }
    // Look up an existing draft; never create a replacement after account/access invalidation.
    // Drafts store a channel's missing thread as "", as slack_draft does.
    const char *thread = s->thread ? s->thread : "";
    for (size_t i = 0; i < s->state.draft_count; i++) {
        SlackDraft *d = &s->state.drafts[i];
        if (str_eq(d->workspace, s->workspace) && str_eq(d->channel, s->channel) && str_eq(d->thread, thread)) {
            if (d->uncertain && !d->sending) { d->uncertain = false; set_string(&s->error, NULL); changed(s); }
            return;
        }
    }
}
void slack_inbox_find_confirmed(SlackScreen *s, const char *workspace, uint64_t generation, const char *query) {
    // The text dialog's modal loop can invalidate access or destroy the inbox.
    SlackScreen *live = screens; while (live && live != s) live = live->next;
    if (!live || !query) return;
    if (!sync_access(s) || !slack_state_current(&s->state, generation)
        || !str_eq(s->workspace, workspace) || !s->directory) { changed(s); return; }
    set_string(&s->search, query); changed(s);
}
static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    SlackScreen *s = (SlackScreen *)base;
    if (act == ACT_WEB) { open_web_url("https://app.slack.com/client"); return; }
    if (!sync_access(s)) { changed(s); return; }
    if (GetTickCount64() < s->cooldown) { changed(s); return; }
    switch (act) {
    case ACT_WORKSPACE: {
        char *id = slack_workspace_id(json_at(s->workspaces, (size_t)arg));
        if (id && slack_inbox_supports("slack_conversations")) navigate(s, id, NULL, NULL);
        free(id); break;
    }
    case ACT_CHANNEL: {
        const char *id = json_str_nonempty(json_get(json_at(s->conversations, (size_t)arg), "id"));
        if (id && slack_inbox_supports("slack_history")) navigate(s, s->workspace, id, NULL);
        break;
    }
    case ACT_THREAD:
        if (arg >= 0 && (size_t)arg < s->state.message_count && slack_inbox_supports("slack_thread")) navigate(s, s->workspace, s->channel, s->state.messages[arg].ts);
        break;
    case ACT_BACK:
        if (!str_empty(s->thread)) navigate(s, s->workspace, s->channel, NULL);
        else navigate(s, s->channel || s->directory ? s->workspace : NULL, NULL, NULL);
        break;
    case ACT_PEOPLE:
        s->directory = !s->directory; composer_set(s);
        if (s->directory && !json_count(s->people)) load(s, TAG_PEOPLE);
        changed(s); break;
    case ACT_PERSON:
        if (arg == -1) {
            char *workspace = xstrdup(s->workspace);
            uint64_t generation = s->state.generation;
            char *query = dialog_text(pane_hwnd(base->pane), "Find people", "Name or Slack user ID (searches loaded pages)", "Find", s->search);
            slack_inbox_find_confirmed(s, workspace, generation, query);
            free(query); free(workspace);
        } else if (slack_inbox_supports("slack_open_dm")) {
            const char *id = json_str_nonempty(json_get(json_at(s->people, (size_t)arg), "id"));
            if (id) { Json *args = arguments(s); json_set_str(args, "userId", id); call(s, TAG_DM, "slack_open_dm", args); }
        }
        break;
    case ACT_CONV_MORE: if (!str_empty(s->conv_cursor)) load(s, TAG_CONVERSATIONS); break;
    case ACT_PEOPLE_MORE: if (!str_empty(s->people_cursor)) load(s, TAG_PEOPLE); break;
    case ACT_OLDER: if (s->page.more) load(s, TAG_HISTORY); break;
    case ACT_REFRESH: refresh(base); break;
    case ACT_SEND: send_message(s); break;
    case ACT_RECOVER: {
        if (!s->workspace || !s->channel || !draft(s)->uncertain) break;
        Json *destination = arguments(s); if (s->thread) json_set_str(destination, "threadTs", s->thread);
        uint64_t generation = s->state.generation;
        if (app_confirm("Allow another Slack send?", "Check Slack history at this destination first. Sending the retained draft again can create a duplicate.", "Allow send", false))
            slack_inbox_recover_confirmed(s, destination, generation);
        json_free(destination);
        break;
    }
    }
}
static void refresh(Screen *base) {
    SlackScreen *s = (SlackScreen *)base;
    if (!sync_access(s) || GetTickCount64() < s->cooldown) { changed(s); return; }
    // Keep a pending send attached to its destination; a history refresh must not lose its receipt.
    if (s->requests[TAG_SEND]) return;
    cancel(s); slack_page_clear(&s->page); set_string(&s->error, NULL); s->read_failed = false;
    for (size_t i = s->state.message_count; i > 0; i--) {
        SlackMessage *m = &s->state.messages[i - 1];
        if (str_eq(m->workspace, s->workspace) && str_eq(m->channel, s->channel))
            slack_message_delete(&s->state, s->workspace, s->channel, m->ts);
    }
    set_string(&s->conv_cursor, NULL); set_string(&s->people_cursor, NULL);
    if (!s->workspace) load(s, TAG_WORKSPACES);
    else if (s->channel && !s->directory) { load(s, TAG_DETAIL); load(s, TAG_HISTORY); }
    else { load(s, TAG_CONVERSATIONS); load(s, TAG_PEOPLE); }
    changed(s);
}
static void observed(SlackScreen *s, int scroll, int height) {
    if (!s->shown || s->read_failed || !g_store.active || s->directory || !s->channel || !slack_inbox_supports("slack_read")) return;
    SlackRead *r = slack_read(&s->state, s->workspace, s->channel);
    for (size_t i = 0; i < s->view_count; i++) {
        // Only a message whose bottom has entered the viewport is considered viewed.
        if (s->view[i].rc.bottom > scroll && s->view[i].rc.bottom <= scroll + height)
            slack_read_viewed(r, s->view[i].ts, GetTickCount64());
    }
    if (s->base.pane && slack_read_due(r, GetTickCount64() + 1000)) SetTimer(pane_hwnd(s->base.pane), TIMER_READ, 1000, NULL);
}
static void place(Screen *base, const RECT *content, int scroll) {
    SlackScreen *s = (SlackScreen *)base;
    observed(s, scroll, content->bottom - content->top);
}
static void timer(Screen *base, UINT id) {
    SlackScreen *s = (SlackScreen *)base;
    KillTimer(pane_hwnd(base->pane), id);
    if (!s->shown || !sync_access(s) || GetTickCount64() < s->cooldown) return;
    if (id == TIMER_EMPTY) {
        unsigned pending = s->empty_pages; s->empty_pages = 0;
        if (pending & (1u << TAG_CONVERSATIONS)) load(s, TAG_CONVERSATIONS);
        if (pending & (1u << TAG_PEOPLE)) load(s, TAG_PEOPLE);
        if (pending & (1u << TAG_HISTORY)) load(s, TAG_HISTORY);
    } else if (id == TIMER_READ && !s->read_failed && g_store.active && s->channel && !s->directory && !s->requests[TAG_READ]) {
        const char *ts = slack_read_due(slack_read(&s->state, s->workspace, s->channel), GetTickCount64());
        if (ts) { Json *args = arguments(s); json_set_str(args, "ts", ts); call(s, TAG_READ, "slack_read", args); }
        else observed(s, pane_scroll_y(base->pane), pane_content_rect(base->pane).bottom - pane_content_rect(base->pane).top);
    }
}
static int footer_height(Screen *base, int width) {
    SlackScreen *s = (SlackScreen *)base;
    return s->channel && !s->directory && slack_inbox_supports("slack_send") ? px(152) : 0;
}
static void footer_layout(Screen *base, const RECT *rc) {
    SlackScreen *s = (SlackScreen *)base;
    if (!s->composer) return;
    if (!s->shown || !s->channel || s->directory || !slack_inbox_supports("slack_send")) { ShowWindow(s->composer, SW_HIDE); return; }
    if (GetParent(s->composer) != pane_hwnd(base->pane)) SetParent(s->composer, pane_hwnd(base->pane));
    s->edit_rc = (RECT){rc->left + px(14), rc->top + px(34), rc->right - px(14), rc->bottom - px(42)};
    s->send_rc = (RECT){rc->right - px(100), rc->bottom - px(36), rc->right - px(14), rc->bottom - px(6)};
    pane_place_control(s->composer, &s->edit_rc); ShowWindow(s->composer, SW_SHOW);
}
static void footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    SlackScreen *s = (SlackScreen *)base;
    if (!s->channel || s->directory || !slack_inbox_supports("slack_send")) return;
    char *dest = destination_name(s); RECT title = {rc->left + px(14), rc->top + px(4), rc->right - px(14), rc->top + px(30)};
    char *note = xstrfmt("Send as you → %s", dest); free(dest);
    draw_text(cv, note, &title, FONT_FOOTNOTE, theme.ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS); free(note);
    SlackDraft *d = draft(s); RECT help = {rc->left + px(14), rc->bottom - px(36), s->send_rc.left - px(6), rc->bottom - px(6)};
    draw_text(cv, d && d->uncertain ? "Check history before resending" : "Ctrl+Enter sends · Limit 8000", &help, FONT_FOOTNOTE, theme.muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    bool enabled = d && !d->sending && !d->uncertain && slack_text_valid(d->text) && GetTickCount64() >= s->cooldown;
    fill_round_rect(cv, &s->send_rc, px(6), enabled ? theme.accent : theme.raise, theme.line);
    draw_text(cv, d && d->sending ? "Sending…" : "Send", &s->send_rc, FONT_BODY, enabled ? theme.on_accent : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void footer_click(Screen *base, POINT pt) { SlackScreen *s = (SlackScreen *)base; if (PtInRect(&s->send_rc, pt)) send_message(s); }
static void command(Screen *base, int id, int code, HWND control) {
    SlackScreen *s = (SlackScreen *)base;
    if (id != ID_COMPOSER || code != EN_CHANGE || s->filling || !sync_access(s)) return;
    SlackDraft *d = draft(s); if (!d) return;
    int n = GetWindowTextLengthW(control); wchar_t *w = xcalloc((size_t)n + 1, sizeof *w); GetWindowTextW(control, w, n + 1);
    char *text = wide_to_utf8(w); free(w); set_string(&d->text, text); free(text); pane_repaint(base->pane);
}
static LRESULT CALLBACK composer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SlackScreen *s = (SlackScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) { send_message(s); return 0; }
    if (msg == WM_CHAR && wp == '\n' && (GetKeyState(VK_CONTROL) & 0x8000)) return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, composer_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void visible(Screen *base, bool shown) {
    SlackScreen *s = (SlackScreen *)base; s->shown = shown;
    if (!shown) { cancel(s); ShowWindow(s->composer, SW_HIDE); return; }
    if (!s->composer) {
        s->composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, pane_hwnd(base->pane), (HMENU)(INT_PTR)ID_COMPOSER, GetModuleHandleW(NULL), NULL);
        SendMessageW(s->composer, WM_SETFONT, (WPARAM)font(FONT_BODY), FALSE);
        SendMessageW(s->composer, EM_SETLIMITTEXT, 32000, 0); // Allow an over-limit draft; validation blocks sending it.
        SetWindowSubclass(s->composer, composer_proc, 1, (DWORD_PTR)s);
    }
    if (!sync_access(s)) return;
    composer_set(s); refresh(base);
}
static void activated(Screen *base, bool active) {
    SlackScreen *s = (SlackScreen *)base;
    if (!active) KillTimer(pane_hwnd(base->pane), TIMER_READ);
    else if (s->shown) { RECT rc = pane_content_rect(base->pane); observed(s, pane_scroll_y(base->pane), rc.bottom - rc.top); }
}
static bool can_leave(Screen *base) {
    SlackScreen *s = (SlackScreen *)base;
    for (size_t i = 0; i < s->state.draft_count; i++) if (!str_empty(s->state.drafts[i].text) || s->state.drafts[i].sending)
        return app_confirm("Leave Slack inbox?", "Scoped drafts are kept while navigating Slack. Leaving this screen discards them; an in-flight send may still reach Slack.", "Leave", false);
    return true;
}
static void destroy(Screen *base) {
    SlackScreen *s = (SlackScreen *)base;
    SlackScreen **slot = &screens; while (*slot && *slot != s) slot = &(*slot)->next; if (*slot) *slot = s->next;
    private_clear(s); if (s->composer) DestroyWindow(s->composer);
    json_free(s->workspaces); json_free(s->conversations); json_free(s->people);
    api_client_release(s->account); free(s->device); free(s->error); screen_release(base);
}
static const ScreenVTable vt = {
    .destroy = destroy, .layout = layout, .header = header, .action = action, .timer = timer, .place = place,
    .footer_height = footer_height, .footer_layout = footer_layout, .footer_paint = footer_paint, .footer_click = footer_click,
    .command = command, .visible = visible, .refresh = refresh, .activated = activated, .can_leave = can_leave, .detachable = true,
};
Screen *slack_screen_new(void) {
    SlackScreen *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("slack-inbox");
    s->workspaces = json_array(); s->conversations = json_array(); s->people = json_array();
    s->account = api_client_retain(g_store.client); s->device = xstrdup(g_store.has_device ? g_store.device.id : NULL); s->access = access_mask();
    s->next = screens; screens = s; return &s->base;
}
