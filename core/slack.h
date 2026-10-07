// Operator inbox state, pinned to core PR #121 at 9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89.
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
#endif
