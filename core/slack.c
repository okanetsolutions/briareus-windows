#include "slack.h"
#include "str.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void replace(char **slot, const char *text) { char *copy = xstrdup(text); free(*slot); *slot = copy; }
static bool destination(const char *w, const char *c, const char *workspace, const char *channel) {
    return str_eq(w, workspace) && str_eq(c, channel);
}
void slack_state_advance(SlackState *s) { s->generation++; }
bool slack_state_current(const SlackState *s, uint64_t generation) { return s->generation == generation; }
void slack_state_clear(SlackState *s) {
    for (size_t i = 0; i < s->message_count; i++) {
        SlackMessage *m = &s->messages[i]; free(m->workspace); free(m->channel); free(m->ts); json_free(m->raw);
    }
    for (size_t i = 0; i < s->draft_count; i++) {
        SlackDraft *d = &s->drafts[i]; free(d->workspace); free(d->channel); free(d->thread); free(d->text); free(d->sent_text);
    }
    for (size_t i = 0; i < s->read_count; i++) {
        SlackRead *r = &s->reads[i]; free(r->workspace); free(r->channel); free(r->viewed); free(r->marked);
    }
    free(s->messages); free(s->drafts); free(s->reads); json_free(s->deleted);
    uint64_t generation = s->generation + 1;
    memset(s, 0, sizeof *s); s->generation = generation;
}
bool slack_ts_valid(const char *ts) {
    if (!ts) return false;
    size_t a = 0, b = 0;
    while (isdigit((unsigned char)*ts)) { a++; ts++; }
    if (*ts++ != '.' || !a || a > 12) return false;
    while (isdigit((unsigned char)*ts)) { b++; ts++; }
    return !*ts && b && b <= 9;
}
int slack_ts_compare(const char *a, const char *b) {
    // Valid decimal strings: compare the integer's significant length, then fraction padded with zeroes.
    while (*a == '0' && a[1] != '.') a++;
    while (*b == '0' && b[1] != '.') b++;
    size_t an = strcspn(a, "."), bn = strcspn(b, ".");
    if (an != bn) return an < bn ? -1 : 1;
    int cmp = strncmp(a, b, an);
    if (cmp) return cmp < 0 ? -1 : 1;
    a += an + 1; b += bn + 1;
    while (*a || *b) {
        char ac = *a ? *a++ : '0', bc = *b ? *b++ : '0';
        if (ac != bc) return ac < bc ? -1 : 1;
    }
    return 0;
}
bool slack_ts_between(const char *ts, const char *oldest, const char *latest) {
    return slack_ts_valid(ts) && (str_empty(oldest) || (slack_ts_valid(oldest) && slack_ts_compare(ts, oldest) > 0))
        && (str_empty(latest) || (slack_ts_valid(latest) && slack_ts_compare(ts, latest) < 0));
}
static int message_compare(const void *a, const void *b) {
    const SlackMessage *ma = a, *mb = b;
    return slack_ts_compare(ma->ts, mb->ts);
}
bool slack_message_merge(SlackState *s, const char *workspace, const char *channel, const Json *message) {
    const char *ts = json_str(json_get(message, "ts"));
    if (str_empty(workspace) || str_empty(channel) || !slack_ts_valid(ts)) return false;
    for (size_t i = 0; i < json_count(s->deleted); i++) {
        const Json *row = json_at(s->deleted, i);
        if (str_eq(workspace, json_str(json_get(row, "workspace"))) && str_eq(channel, json_str(json_get(row, "channel"))) &&
            str_eq(ts, json_str(json_get(row, "ts")))) return false;
    }
    for (size_t i = 0; i < s->message_count; i++) {
        SlackMessage *m = &s->messages[i];
        if (destination(m->workspace, m->channel, workspace, channel) && str_eq(m->ts, ts)) {
            json_object_merge(m->raw, message); return true;
        }
    }
    s->messages = xrealloc(s->messages, (s->message_count + 1) * sizeof *s->messages);
    SlackMessage *m = &s->messages[s->message_count++];
    m->workspace = xstrdup(workspace); m->channel = xstrdup(channel); m->ts = xstrdup(ts); m->raw = json_clone(message);
    qsort(s->messages, s->message_count, sizeof *s->messages, message_compare);
    return true;
}
bool slack_message_receipt(SlackState *s, const char *workspace, const char *channel, const Json *message) {
    const char *ts = json_str(json_get(message, "ts"));
    for (size_t i = 0; i < s->message_count; i++) {
        const SlackMessage *m = &s->messages[i];
        if (destination(m->workspace, m->channel, workspace, channel) && str_eq(m->ts, ts)) return true;
    }
    return slack_message_merge(s, workspace, channel, message);
}
void slack_message_delete(SlackState *s, const char *workspace, const char *channel, const char *ts) {
    for (size_t i = 0; i < s->message_count; i++) {
        SlackMessage *m = &s->messages[i];
        if (!destination(m->workspace, m->channel, workspace, channel) || !str_eq(m->ts, ts)) continue;
        free(m->workspace); free(m->channel); free(m->ts); json_free(m->raw);
        memmove(m, m + 1, (s->message_count - i - 1) * sizeof *m); s->message_count--; return;
    }
}
bool slack_message_in_thread(const SlackMessage *m, const char *workspace, const char *channel, const char *thread) {
    if (!destination(m->workspace, m->channel, workspace, channel)) return false;
    const char *parent = json_str(json_get(m->raw, "thread_ts"));
    return str_empty(thread) ? str_empty(parent) || str_eq(parent, m->ts) || str_eq(json_str(json_get(m->raw, "subtype")), "thread_broadcast") : str_eq(m->ts, thread) || str_eq(parent, thread);
}
void slack_page_clear(SlackPage *p) { free(p->cursor); free(p->oldest); free(p->latest); memset(p, 0, sizeof *p); }
bool slack_page_merge(SlackState *s, SlackPage *p, const char *workspace, const char *channel, bool thread, const Json *answer) {
    const Json *messages = json_get(answer, "messages");
    const char *cursor = json_str(json_get(answer, "nextCursor"));
    if (!json_is_array(messages) || !cursor || json_bool_tristate(json_get(answer, "hasMore")) < 0) return false;
    char *edge = NULL;
    for (size_t i = 0; i < json_count(messages); i++) {
        const Json *m = json_at(messages, i); const char *ts = json_str(json_get(m, "ts"));
        if (!slack_ts_between(ts, p->oldest, p->latest)) continue;
        slack_message_merge(s, workspace, channel, m);
        if (!edge || (thread ? slack_ts_compare(ts, edge) > 0 : slack_ts_compare(ts, edge) < 0)) replace(&edge, ts);
    }
    p->more = *cursor || json_bool_is(json_get(answer, "hasMore"), true);
    p->stalled = (*cursor && str_eq(cursor, p->cursor)) || (p->more && !*cursor && !edge);
    replace(&p->cursor, cursor);
    if (!*cursor && edge && p->more) replace(thread ? &p->oldest : &p->latest, edge);
    free(edge);
    if (p->stalled) p->more = false;
    return true;
}
void slack_rows_merge(Json *rows, const Json *incoming) {
    if (!json_is_array(rows) || !json_is_array(incoming)) return;
    for (size_t i = 0; i < json_count(incoming); i++) {
        const Json *row = json_at(incoming, i); const char *id = json_str_nonempty(json_get(row, "id"));
        if (!id) continue;
        size_t k;
        for (k = 0; k < json_count(rows); k++) if (str_eq(id, json_str(json_get(json_at(rows, k), "id")))) break;
        if (k == json_count(rows)) json_array_push(rows, json_clone(row));
        else json_object_merge(rows->a.items[k], row);
    }
}
char *slack_workspace_id(const Json *row) {
    double id;
    if (!json_num(json_get(row, "id"), &id) || id < 1 || id > 9007199254740991.0 || floor(id) != id) return NULL;
    return xstrfmt("%.0f", id);
}
char *slack_person_name(const Json *people, const char *id) {
    for (size_t i = 0; i < json_count(people); i++) {
        const Json *p = json_at(people, i);
        if (!str_eq(id, json_str(json_get(p, "id")))) continue;
        const char *name = json_str_nonempty(json_get(json_get(p, "profile"), "display_name"));
        if (!name) name = json_str_nonempty(json_get(p, "real_name"));
        if (!name) name = json_str_nonempty(json_get(p, "name"));
        if (name) return xstrdup(name);
    }
    return xstrdup(str_empty(id) ? "Unknown author" : id);
}
char *slack_conversation_name(const Json *row, const Json *people) {
    if (json_bool_is(json_get(row, "is_im"), true)) return slack_person_name(people, json_str(json_get(row, "user")));
    const char *name = json_str_nonempty(json_get(row, "name"));
    return xstrdup(name ? name : json_str_or(json_get(row, "id"), "Conversation"));
}
static void literal(Str *s, const char *text) {
    for (const char *p = text; *p; p++) {
        if (strchr("\\[]*_~`", *p)) str_appendc(s, '\\');
        str_appendc(s, *p);
    }
}
static char *mrkdwn(const char *source, const Json *people) {
    Str s; str_init(&s); bool code = false, fence = false;
    for (const char *p = source ? source : ""; *p; p++) {
        if (*p == '`') {
            if (strncmp(p, "```", 3) == 0) { fence = !fence; str_appendz(&s, "```"); p += 2; }
            else { code = !code; str_appendc(&s, '`'); }
        } else if (code || fence) str_appendc(&s, *p);
        else if (strncmp(p, "&amp;", 5) == 0) { str_appendc(&s, '&'); p += 4; }
        else if (strncmp(p, "&lt;", 4) == 0) { str_appendc(&s, '<'); p += 3; }
        else if (strncmp(p, "&gt;", 4) == 0) { str_appendc(&s, '>'); p += 3; }
        else if (*p == '<' && strchr(p, '>')) {
            const char *end = strchr(p, '>'); char *token = xstrndup(p + 1, (size_t)(end - p - 1));
            char *label = strchr(token, '|'); if (label) *label++ = 0;
            if (token[0] == '@') { char *name = slack_person_name(people, token + 1); str_appendc(&s, '@'); literal(&s, name); free(name); }
            else if (token[0] == '#') { str_appendc(&s, '#'); literal(&s, label ? label : token + 1); }
            else if (token[0] == '!') literal(&s, label ? label : token + 1);
            else if (str_has_prefix(token, "https://") && !strpbrk(token, " ()\\\r\n\t")) {
                str_appendc(&s, '['); literal(&s, label ? label : token); str_appendf(&s, "](%s)", token);
            } else literal(&s, label ? label : token);
            free(token); p = end;
        } else if (*p == '*' || *p == '~') { str_appendc(&s, *p); str_appendc(&s, *p); }
        else if (*p == '[' || *p == ']') { str_appendc(&s, '\\'); str_appendc(&s, *p); }
        else str_appendc(&s, *p);
    }
    return str_detach(&s);
}
static void block_text(Str *out, const Json *value, const Json *people) {
    if (json_is_array(value)) {
        for (size_t i = 0; i < json_count(value); i++) block_text(out, json_at(value, i), people);
    } else if (json_is_object(value)) {
        const Json *text = json_get(value, "text");
        if (json_str_nonempty(text)) {
            char *rendered = str_eq(json_str(json_get(value, "type")), "plain_text") ? xstrdup(json_str(text)) : mrkdwn(json_str(text), people);
            str_appendf(out, "%s\n", rendered); free(rendered);
        } else block_text(out, text, people);
        block_text(out, json_get(value, "elements"), people);
        block_text(out, json_get(value, "fields"), people);
    }
}
char *slack_message_text(const Json *message, const Json *people) {
    Str s; str_init(&s);
    const char *text = json_str_nonempty(json_get(message, "text"));
    if (text) { char *rendered = mrkdwn(text, people); str_appendz(&s, rendered); free(rendered); }
    else {
        block_text(&s, json_get(message, "blocks"), people);
        const Json *attachments = json_get(message, "attachments");
        for (size_t i = 0; i < json_count(attachments); i++) {
            const Json *a = json_at(attachments, i);
            const char *fallback = json_str_nonempty(json_get(a, "fallback"));
            if (!fallback) fallback = json_str_nonempty(json_get(a, "text"));
            if (fallback) { char *rendered = mrkdwn(fallback, people); str_appendf(&s, "\n%s", rendered); free(rendered); }
        }
        if (!s.len && json_count(json_get(message, "blocks"))) str_appendz(&s, "[Slack block content]");
    }
    const Json *files = json_get(message, "files");
    for (size_t i = 0; i < json_count(files); i++) {
        const Json *f = json_at(files, i);
        const char *name = json_str_nonempty(json_get(f, "title"));
        if (!name) name = json_str_or(json_get(f, "name"), "File");
        str_appendz(&s, "\n\nFile: "); literal(&s, name);
        str_appendf(&s, " (%s, %.0f bytes)", json_str_or(json_get(f, "mimetype"), "unknown type"), json_num_or(json_get(f, "size"), 0));
        const char *link = json_str(json_get(f, "permalink"));
        if (link && str_has_prefix(link, "https://") && !strpbrk(link, " ()\\\r\n\t")) str_appendf(&s, " · [Open in browser](%s)", link);
        else str_appendz(&s, " · Open in Slack to access this file");
    }
    if (!s.len) str_appendz(&s, "[Message without text]");
    return str_detach(&s);
}
SlackDraft *slack_draft(SlackState *s, const char *workspace, const char *channel, const char *thread) {
    if (!thread) thread = "";
    for (size_t i = 0; i < s->draft_count; i++) {
        SlackDraft *d = &s->drafts[i];
        if (destination(d->workspace, d->channel, workspace, channel) && str_eq(d->thread, thread)) return d;
    }
    s->drafts = xrealloc(s->drafts, (s->draft_count + 1) * sizeof *s->drafts);
    SlackDraft *d = &s->drafts[s->draft_count++]; memset(d, 0, sizeof *d);
    d->workspace = xstrdup(workspace); d->channel = xstrdup(channel); d->thread = xstrdup(thread); d->text = xstrdup("");
    return d;
}
bool slack_text_valid(const char *text) {
    // JS string.length counts UTF-16 units; match it even for astral emoji in UTF-8.
    size_t units = 0; bool nonspace = false;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        nonspace |= !isspace(*p);
        if ((*p & 0xc0) != 0x80) units += *p >= 0xf0 ? 2 : 1;
        if (units > SLACK_TEXT_LIMIT) return false;
    }
    return nonspace;
}
bool slack_draft_begin(SlackDraft *d) {
    if (d->sending || d->uncertain || !slack_text_valid(d->text)) return false;
    replace(&d->sent_text, d->text); d->sending = true; return true;
}
void slack_draft_cancel(SlackDraft *d) { if (d->sending) { d->sending = false; d->uncertain = true; } }
SlackSendResult slack_draft_finish(SlackDraft *d, bool ok, bool refusal, const Json *receipt) {
    d->sending = false;
    const char *ts = json_str(json_get(receipt, "ts")), *channel = json_str(json_get(receipt, "channel"));
    if (ok && str_eq(channel, d->channel) && slack_ts_valid(ts)) {
        if (str_eq(d->text, d->sent_text)) replace(&d->text, "");
        replace(&d->sent_text, NULL); d->uncertain = false;
        return json_bool_is(json_get(receipt, "workspaceChanged"), true) ? SLACK_SEND_WORKSPACE_CHANGED : SLACK_SEND_CONFIRMED;
    }
    d->uncertain = !refusal || ok;
    return d->uncertain ? SLACK_SEND_AMBIGUOUS : SLACK_SEND_REFUSED;
}
SlackRead *slack_read(SlackState *s, const char *workspace, const char *channel) {
    for (size_t i = 0; i < s->read_count; i++) if (destination(s->reads[i].workspace, s->reads[i].channel, workspace, channel)) return &s->reads[i];
    s->reads = xrealloc(s->reads, (s->read_count + 1) * sizeof *s->reads);
    SlackRead *r = &s->reads[s->read_count++]; memset(r, 0, sizeof *r);
    r->workspace = xstrdup(workspace); r->channel = xstrdup(channel); return r;
}
void slack_read_viewed(SlackRead *r, const char *ts, uint64_t now) {
    if (!slack_ts_valid(ts) || (r->viewed && slack_ts_compare(ts, r->viewed) <= 0)) return;
    replace(&r->viewed, ts); r->due = now + 1000;
}
const char *slack_read_due(const SlackRead *r, uint64_t now) {
    return r->viewed && now >= r->due && (!r->marked || slack_ts_compare(r->viewed, r->marked) > 0) ? r->viewed : NULL;
}
void slack_read_confirm(SlackRead *r, const char *ts) {
    if (slack_ts_valid(ts) && (!r->marked || slack_ts_compare(ts, r->marked) > 0)) replace(&r->marked, ts);
}

void slack_messages_clear(SlackState *s, const char *workspace, const char *channel) {
    for (size_t i = s->message_count; i > 0; i--) {
        SlackMessage *m = &s->messages[i - 1];
        if (destination(m->workspace, m->channel, workspace, channel)) slack_message_delete(s, workspace, channel, m->ts);
    }
}
void slack_events_clear(SlackEvents *q) {
    for (size_t i = 0; i < q->count; i++) { free(q->items[i].name); free(q->items[i].data); }
    free(q->items); memset(q, 0, sizeof *q);
}
bool slack_events_push(SlackEvents *q, const char *name, const char *data, size_t length) {
    if (q->overflow) return false;
    size_t name_length = strlen(name);
    if (q->count >= SLACK_EVENTS_MAX || name_length > SLACK_EVENTS_BYTES - q->bytes || length > SLACK_EVENTS_BYTES - q->bytes - name_length) {
        slack_events_clear(q); q->overflow = true; return false;
    }
    q->items = xrealloc(q->items, (q->count + 1) * sizeof *q->items);
    q->items[q->count++] = (SlackEvent){xstrdup(name), xstrndup(data, length), length};
    q->bytes += length + name_length; return true;
}
SlackEventResult slack_event_apply(SlackState *s, const char *workspace, const char *name, const Json *data) {
    char *id = NULL;
    double number;
    if (json_num(json_get(data, "workspaceId"), &number) && number >= 1 && number <= 9007199254740991.0 && floor(number) == number)
        id = xstrfmt("%.0f", number);
    bool matches = str_eq(id, workspace); free(id);
    if (!matches) return SLACK_EVENT_IGNORED;
    if (str_eq(name, "ready")) return SLACK_EVENT_READY;
    if (str_eq(name, "workspace.changed")) return SLACK_EVENT_CHANGED;
    if (str_eq(name, "workspace.removed")) return SLACK_EVENT_REMOVED;
    if (str_eq(name, "conversation.read")) {
        const char *channel = json_str_nonempty(json_get(data, "channel")), *ts = json_str(json_get(data, "ts"));
        if (!channel || !slack_ts_valid(ts)) return SLACK_EVENT_IGNORED;
        slack_read_confirm(slack_read(s, workspace, channel), ts); return SLACK_EVENT_APPLIED;
    }
    const Json *event = json_get(data, "event");
    const char *channel = json_str_nonempty(json_get(event, "channel"));
    if (!channel) return SLACK_EVENT_IGNORED;
    if (str_eq(name, "message.deleted")) {
        const char *ts = json_str(json_get(event, "deleted_ts"));
        if (!slack_ts_valid(ts)) return SLACK_EVENT_IGNORED;
        if (!s->deleted) s->deleted = json_array();
        Json *row = json_object(); json_set_str(row, "workspace", workspace); json_set_str(row, "channel", channel); json_set_str(row, "ts", ts);
        bool found = false;
        for (size_t i = 0; i < json_count(s->deleted); i++) {
            const Json *old = json_at(s->deleted, i);
            if (str_eq(workspace, json_str(json_get(old, "workspace"))) && str_eq(channel, json_str(json_get(old, "channel"))) &&
                str_eq(ts, json_str(json_get(old, "ts")))) { found = true; break; }
        }
        if (!found) json_array_push(s->deleted, row); else json_free(row);
        slack_message_delete(s, workspace, channel, ts); return SLACK_EVENT_APPLIED;
    }
    if (str_eq(name, "message") || str_eq(name, "message.changed")) {
        const Json *message = str_eq(name, "message.changed") ? json_get(event, "message") : event;
        if (str_eq(name, "message.changed")) {
            const char *ts = json_str(json_get(message, "ts")); bool loaded = false;
            for (size_t i = 0; i < s->message_count; i++) {
                const SlackMessage *m = &s->messages[i];
                if (destination(m->workspace, m->channel, workspace, channel) && str_eq(m->ts, ts)) { loaded = true; break; }
            }
            if (!loaded) return SLACK_EVENT_IGNORED;
        }
        return slack_message_merge(s, workspace, channel, message) ? SLACK_EVENT_APPLIED : SLACK_EVENT_IGNORED;
    }
    return SLACK_EVENT_IGNORED;
}
uint64_t slack_reconnect_delay(unsigned failures, double retry_after) {
    uint64_t delay = 1000;
    for (unsigned i = 1; i < failures && delay < 60000; i++) delay *= 2;
    if (delay > 60000) delay = 60000;
    if (retry_after > 0) {
        if (retry_after > 86400 * 30) retry_after = 86400 * 30;
        uint64_t retry = (uint64_t)(retry_after * 1000);
        if (retry > delay) delay = retry;
    }
    return delay;
}

static bool channel_present(const Json *rows, const char *channel) {
    for (size_t i = 0; i < json_count(rows); i++) if (str_eq(channel, json_str(json_get(json_at(rows, i), "id")))) return true;
    return false;
}
void slack_state_prune(SlackState *s, const char *workspace, const Json *conversations) {
    for (size_t i = s->message_count; i > 0; i--) {
        const SlackMessage *m = &s->messages[i - 1];
        if (str_eq(m->workspace, workspace) && !channel_present(conversations, m->channel)) slack_message_delete(s, m->workspace, m->channel, m->ts);
    }
    if (s->deleted) {
        Json *kept = json_array();
        for (size_t i = 0; i < json_count(s->deleted); i++) {
            const Json *row = json_at(s->deleted, i);
            if (!str_eq(workspace, json_str(json_get(row, "workspace"))) || channel_present(conversations, json_str(json_get(row, "channel"))))
                json_array_push(kept, json_clone(row));
        }
        json_free(s->deleted); s->deleted = kept;
    }
    for (size_t i = s->draft_count; i > 0; i--) {
        SlackDraft *d = &s->drafts[i - 1];
        if (!str_eq(d->workspace, workspace) || channel_present(conversations, d->channel)) continue;
        free(d->workspace); free(d->channel); free(d->thread); free(d->text); free(d->sent_text);
        memmove(d, d + 1, (s->draft_count - i) * sizeof *d); s->draft_count--;
    }
    for (size_t i = s->read_count; i > 0; i--) {
        SlackRead *r = &s->reads[i - 1];
        if (!str_eq(r->workspace, workspace) || channel_present(conversations, r->channel)) continue;
        free(r->workspace); free(r->channel); free(r->viewed); free(r->marked);
        memmove(r, r + 1, (s->read_count - i) * sizeof *r); s->read_count--;
    }
}
