// Operator inbox state, pinned to core PR #121 at 54ba5c987e7fae8685766b38459632879ff577fd.
#ifndef BRIAREUS_SLACK_H
#define BRIAREUS_SLACK_H
#include "json.h"
#include <stdint.h>

#define SLACK_TEXT_LIMIT 8000

typedef struct { char *workspace, *channel, *ts; Json *raw; } SlackMessage;
typedef struct {
    char *workspace, *channel, *thread, *text, *sent_text;
    bool sending, uncertain;
} SlackDraft;
typedef struct { char *workspace, *channel, *viewed, *marked; uint64_t due; } SlackRead;
typedef struct {
    SlackMessage *messages; size_t message_count;
    SlackDraft *drafts; size_t draft_count;
    SlackRead *reads; size_t read_count;
    Json *deleted; // Event tombstones keep late history/send receipts from resurrecting a deletion.
    uint64_t generation;
} SlackState;
typedef struct { char *cursor, *oldest, *latest; bool more, stalled; } SlackPage;
typedef enum { SLACK_SEND_REFUSED, SLACK_SEND_AMBIGUOUS, SLACK_SEND_CONFIRMED, SLACK_SEND_WORKSPACE_CHANGED } SlackSendResult;

void slack_state_clear(SlackState *s);
/// Reject async work from a previous navigation/account generation.
bool slack_state_current(const SlackState *s, uint64_t generation);
void slack_state_advance(SlackState *s);
bool slack_ts_valid(const char *ts);
int slack_ts_compare(const char *a, const char *b);
/// Strictly exclusive bounds, with no floating point timestamp conversions.
bool slack_ts_between(const char *ts, const char *oldest, const char *latest);
/// Merge partial updates by (workspace, channel, ts); usable by history, sends and future SSE.
bool slack_message_merge(SlackState *s, const char *workspace, const char *channel, const Json *message);
/// A receipt fills a missing identity only; an earlier own event/edit remains authoritative.
bool slack_message_receipt(SlackState *s, const char *workspace, const char *channel, const Json *message);
void slack_message_delete(SlackState *s, const char *workspace, const char *channel, const char *ts);
bool slack_message_in_thread(const SlackMessage *m, const char *workspace, const char *channel, const char *thread);
void slack_page_clear(SlackPage *p);
/// Merge history/replies and advance cursor even on empty pages; refuse non-progressing pagination.
bool slack_page_merge(SlackState *s, SlackPage *p, const char *workspace, const char *channel, bool thread, const Json *answer);
void slack_rows_merge(Json *rows, const Json *incoming);
/// Slack workspace IDs are numbers, unlike message timestamps; return their exact integral decimal form.
char *slack_workspace_id(const Json *row);
char *slack_person_name(const Json *people, const char *id);
char *slack_conversation_name(const Json *row, const Json *people);
/// Converts Slack links, mentions and emphasis to readable Markdown, with block/file metadata fallbacks.
char *slack_message_text(const Json *message, const Json *people);
SlackDraft *slack_draft(SlackState *s, const char *workspace, const char *channel, const char *thread);
bool slack_text_valid(const char *text);
bool slack_draft_begin(SlackDraft *d);
/// A successful receipt clears only the submitted draft; failures never start another send.
SlackSendResult slack_draft_finish(SlackDraft *d, bool ok, bool refusal, const Json *receipt);
/// Cancellation can leave a write in flight; it becomes uncertain, never resendable automatically.
void slack_draft_cancel(SlackDraft *d);
SlackRead *slack_read(SlackState *s, const char *workspace, const char *channel);
void slack_read_viewed(SlackRead *r, const char *ts, uint64_t now);
const char *slack_read_due(const SlackRead *r, uint64_t now);
void slack_read_confirm(SlackRead *r, const char *ts);
// Both the transport mailbox and ready/snapshot buffer use these hard limits.
#define SLACK_EVENTS_MAX 256u
#define SLACK_EVENTS_BYTES (2u * 1024 * 1024)
typedef struct { char *name, *data; size_t length; } SlackEvent;
typedef struct { SlackEvent *items; size_t count, bytes; bool overflow; } SlackEvents;
void slack_events_clear(SlackEvents *q);
/// False means reconciliation must restart; no subsequent event is accepted until clear.
bool slack_events_push(SlackEvents *q, const char *name, const char *data, size_t length);
typedef enum { SLACK_EVENT_IGNORED, SLACK_EVENT_APPLIED, SLACK_EVENT_READY,
               SLACK_EVENT_CHANGED, SLACK_EVENT_REMOVED } SlackEventResult;
SlackEventResult slack_event_apply(SlackState *s, const char *workspace, const char *name, const Json *data);
/// Removes cached messages at this destination before authoritative snapshot replacement.
void slack_state_prune(SlackState *s, const char *workspace, const Json *conversations);
void slack_messages_clear(SlackState *s, const char *workspace, const char *channel);
/// Exponential reconnect delay; reset failures only after snapshots finish, not merely ready.
uint64_t slack_reconnect_delay(unsigned failures, double retry_after);
#endif
