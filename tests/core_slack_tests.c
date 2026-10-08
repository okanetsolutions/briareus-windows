#include "slack.h"
#include "slack_fixtures.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static bool merge(SlackState *s, const char *w, const char *c, const char *source) {
    Json *j = json_parsez(source); bool ok = slack_message_merge(s, w, c, j); json_free(j); return ok;
}
static bool page(SlackState *s, SlackPage *p, bool thread, const char *source) {
    Json *j = json_parsez(source); bool ok = slack_page_merge(s, p, "1", "C1", thread, j); json_free(j); return ok;
}
static void timestamps(void) {
    CHECK(slack_ts_valid("1712345678.000001")); CHECK(slack_ts_valid("000000000001.123456789"));
    const char *bad[] = { NULL, "", "1", ".1", "1.", "1234567890123.1", "1.1234567890", "1.2e3", "-1.1", "1.1x" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!slack_ts_valid(bad[i]));
    CHECK(slack_ts_compare("9.1", "10.01") < 0); CHECK(slack_ts_compare("10.01", "9.1") > 0);
    CHECK(slack_ts_compare("01.10", "1.1") == 0); CHECK(slack_ts_compare("1.101", "1.100999999") > 0);
    CHECK(slack_ts_compare("1.123456781", "1.123456782") < 0);
    CHECK(slack_ts_between("1.2", "1.1", "1.3")); CHECK(slack_ts_between("1.2", NULL, NULL));
    CHECK(!slack_ts_between("1.2", "1.2", "1.3")); CHECK(!slack_ts_between("1.2", "1.1", "1.2"));
    CHECK(!slack_ts_between("bad", NULL, NULL)); CHECK(!slack_ts_between("1.2", "bad", NULL)); CHECK(!slack_ts_between("1.2", NULL, "bad"));
}
static void identity(void) {
    SlackState s = {0}; uint64_t generation = s.generation;
    CHECK(merge(&s, "1", "C1", "{\"ts\":\"1.000000001\",\"text\":\"old\",\"user\":\"U1\"}"));
    CHECK(merge(&s, "1", "C1", "{\"ts\":\"1.000000001\",\"text\":\"edited\",\"reply_count\":2}"));
    CHECK_INT(s.message_count, 1); CHECK_STR(json_str(json_get(s.messages[0].raw, "user")), "U1");
    CHECK_STR(json_str(json_get(s.messages[0].raw, "text")), "edited");
    CHECK(merge(&s, "2", "C1", "{\"ts\":\"1.000000001\",\"text\":\"other workspace\"}"));
    CHECK(merge(&s, "1", "D1", "{\"ts\":\"1.000000001\",\"text\":\"DM\"}"));
    CHECK(merge(&s, "1", "C1", "{\"ts\":\"0.9\",\"text\":\"older\"}"));
    CHECK_INT(s.message_count, 4); CHECK_STR(s.messages[0].ts, "0.9");
    CHECK(!merge(&s, "", "C1", "{\"ts\":\"1.1\"}")); CHECK(!merge(&s, "1", "", "{\"ts\":\"1.1\"}"));
    CHECK(!merge(&s, "1", "C1", "{\"ts\":1712345678.1}"));
    slack_message_delete(&s, "1", "C1", "1.000000001"); CHECK_INT(s.message_count, 3);
    slack_message_delete(&s, "1", "C1", "missing"); CHECK_INT(s.message_count, 3);
    CHECK(slack_state_current(&s, generation)); slack_state_advance(&s); CHECK(!slack_state_current(&s, generation));
    slack_state_clear(&s); CHECK_INT(s.message_count, 0); CHECK(s.generation > generation); slack_state_clear(&s);
}
static void pagination(void) {
    SlackState s = {0}; SlackPage p = {0};
    CHECK(page(&s, &p, false, SLACK_EMPTY_PAGE)); CHECK(p.more && !p.stalled); CHECK_STR(p.cursor, "next"); CHECK_INT(s.message_count, 0);
    CHECK(page(&s, &p, false, SLACK_HISTORY)); CHECK(p.more); CHECK_STR(p.latest, "1712345678.000001"); CHECK_STR(p.cursor, "");
    CHECK_INT(s.message_count, 2); CHECK_STR(s.messages[0].ts, "1712345678.000001");
    CHECK(page(&s, &p, false, "{\"messages\":[{\"ts\":\"1712345678.000001\",\"text\":\"exclusive\"},{\"ts\":\"1712345677.999999\",\"text\":\"older\"}],\"nextCursor\":\"\",\"hasMore\":false}"));
    CHECK_INT(s.message_count, 3); CHECK(!p.more); CHECK_STR(json_str(json_get(s.messages[1].raw, "text")), "parent");
    slack_page_clear(&p); CHECK(page(&s, &p, true, SLACK_THREAD));
    CHECK(slack_message_in_thread(&s.messages[1], "1", "C1", "1712345678.000001"));
    CHECK(!slack_message_in_thread(&s.messages[1], "2", "C1", "1712345678.000001"));
    CHECK(slack_message_in_thread(&s.messages[1], "1", "C1", NULL));
    CHECK(slack_message_in_thread(&s.messages[3], "1", "C1", "1712345678.000001"));
    CHECK(!slack_message_in_thread(&s.messages[3], "1", "C1", NULL));
    CHECK(!slack_message_in_thread(&s.messages[2], "1", "C1", "1712345678.000001"));
    slack_page_clear(&p);
    CHECK(page(&s, &p, true, "{\"messages\":[{\"ts\":\"1.1\"},{\"ts\":\"1.2\"}],\"nextCursor\":\"\",\"hasMore\":true}"));
    CHECK_STR(p.oldest, "1.2"); CHECK(p.more); CHECK(!p.latest);
    CHECK(page(&s, &p, true, "{\"messages\":[],\"nextCursor\":\"a\",\"hasMore\":false}")); CHECK(p.more);
    CHECK(page(&s, &p, true, "{\"messages\":[],\"nextCursor\":\"a\",\"hasMore\":true}")); CHECK(p.stalled && !p.more);
    slack_page_clear(&p);
    CHECK(page(&s, &p, false, "{\"messages\":[],\"nextCursor\":\"\",\"hasMore\":true}")); CHECK(p.stalled);
    CHECK(!page(&s, &p, false, "{}")); CHECK(!page(&s, &p, false, "{\"messages\":[],\"nextCursor\":5,\"hasMore\":true}"));
    CHECK(!page(&s, &p, false, "{\"messages\":[],\"nextCursor\":\"\",\"hasMore\":null}"));
    slack_page_clear(&p); slack_state_clear(&s);
}
static void directory(void) {
    Json *people_response = json_parsez(SLACK_PEOPLE), *people = json_clone(json_get(people_response, "people"));
    CHECK_OWNED_STR(slack_person_name(people, "U1"), "Ana Ops"); CHECK_OWNED_STR(slack_person_name(people, "U2"), "U2");
    CHECK_OWNED_STR(slack_person_name(people, NULL), "Unknown author");
    Json *updates = json_parsez("[{\"id\":\"U1\",\"profile\":{},\"real_name\":\"Ana Updated\"},{\"id\":\"U2\",\"name\":\"bob\"},{\"name\":\"skip\"}]");
    slack_rows_merge(people, updates); CHECK_INT(json_count(people), 2); CHECK_OWNED_STR(slack_person_name(people, "U1"), "Ana Updated");
    json_free(updates); updates = json_parsez("[{\"id\":\"U1\",\"real_name\":\"\"}]"); slack_rows_merge(people, updates);
    CHECK_OWNED_STR(slack_person_name(people, "U1"), "ana"); slack_rows_merge(NULL, updates); slack_rows_merge(people, NULL);
    Json *workspaces = json_parsez(SLACK_WORKSPACES);
    CHECK_OWNED_STR(slack_workspace_id(json_at(json_get(workspaces, "workspaces"), 0)), "1727000000002");
    Json *bad = json_parsez("{\"id\":1.5}"); CHECK(slack_workspace_id(bad) == NULL); json_free(bad);
    CHECK(slack_workspace_id(NULL) == NULL);
    Json *conversations = json_parsez(SLACK_CONVERSATIONS); const Json *rows = json_get(conversations, "conversations");
    CHECK_OWNED_STR(slack_conversation_name(json_at(rows, 0), people), "general");
    CHECK_OWNED_STR(slack_conversation_name(json_at(rows, 2), people), "ana");
    CHECK_OWNED_STR(slack_conversation_name(json_at(rows, 3), people), "mpdm-team");
    CHECK_OWNED_STR(slack_conversation_name(NULL, people), "Conversation");
    Json *id = json_parsez("{\"id\":\"G9\"}"); CHECK_OWNED_STR(slack_conversation_name(id, people), "G9");
    json_free(id); json_free(conversations); json_free(workspaces); json_free(updates); json_free(people); json_free(people_response);
}
static void rendering(void) {
    Json *people = json_parsez("[{\"id\":\"U1\",\"profile\":{\"display_name\":\"Ana\"}}]");
    Json *m = json_parsez("{\"text\":\"Hello <@U1> <#C1|general> <!here> <!date^1|today> <https://example.com/a|open> <http://x|unsafe> *bold* _italic_ ~gone~ &amp; &lt; &gt; [text] `*code*`\\n```\\n*literal*\\n```\"}");
    CHECK_OWNED_STR(slack_message_text(m, people), "Hello @Ana #general here today [open](https://example.com/a) unsafe **bold** _italic_ ~~gone~~ & < > \\[text\\] `*code*`\n```\n*literal*\n```"); json_free(m);
    m = json_parsez("{\"blocks\":[{\"type\":\"section\",\"text\":{\"type\":\"mrkdwn\",\"text\":\"*Block*\"},\"fields\":[{\"type\":\"plain_text\",\"text\":\"metadata\"}]},{\"type\":\"rich_text\",\"elements\":[{\"type\":\"text\",\"text\":\"rich fallback\"}]}],\"attachments\":[{\"fallback\":\"attachment\"},{\"text\":\"second\"}],\"files\":[{\"title\":\"Report\",\"mimetype\":\"application/pdf\",\"size\":42,\"permalink\":\"https://slack.com/file\"},{\"name\":\"private\",\"url_private_download\":\"https://secret.example\"}]}");
    char *text = slack_message_text(m, people); CHECK(strstr(text, "**Block**\nmetadata\nrich fallback") != NULL);
    CHECK(strstr(text, "attachment\nsecond") != NULL); CHECK(strstr(text, "Report (application/pdf, 42 bytes)") != NULL);
    CHECK(strstr(text, "[Open in browser](https://slack.com/file)") != NULL); CHECK(strstr(text, "secret.example") == NULL);
    CHECK(strstr(text, "Open in Slack to access this file") != NULL); free(text); json_free(m);
    m = json_parsez("{\"blocks\":[{\"type\":\"image\"}]}" ); CHECK_OWNED_STR(slack_message_text(m, people), "[Slack block content]"); json_free(m);
    CHECK_OWNED_STR(slack_message_text(NULL, people), "[Message without text]"); json_free(people);
}
static void drafts(void) {
    SlackState s = {0}; SlackDraft *d = slack_draft(&s, "1", "C1", NULL); CHECK_STR(d->thread, "");
    CHECK(!slack_draft_begin(d)); free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d)); CHECK(!slack_draft_begin(d));
    CHECK(slack_draft_finish(d, false, false, NULL) == SLACK_SEND_AMBIGUOUS); CHECK(d->uncertain && !d->sending); CHECK_STR(d->text, "human reply"); CHECK(!slack_draft_begin(d));
    // Only an explicit user recovery clears uncertainty.
    d->uncertain = false; CHECK(slack_draft_begin(d)); CHECK(slack_draft_finish(d, false, true, NULL) == SLACK_SEND_REFUSED); CHECK(!d->uncertain);
    CHECK(slack_draft_begin(d)); Json *receipt = json_parsez(SLACK_RECEIPT);
    CHECK(slack_draft_finish(d, true, false, receipt) == SLACK_SEND_CONFIRMED); CHECK_STR(d->text, ""); CHECK(!d->sent_text);
    free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d));
    Json *changed = json_parsez(SLACK_CHANGED_RECEIPT); CHECK(slack_draft_finish(d, true, false, changed) == SLACK_SEND_WORKSPACE_CHANGED);
    CHECK_STR(d->text, ""); CHECK(!slack_draft_begin(d));
    free(d->text); d->text = xstrdup("original"); CHECK(slack_draft_begin(d)); free(d->text); d->text = xstrdup("edited while pending");
    CHECK(slack_draft_finish(d, true, false, receipt) == SLACK_SEND_CONFIRMED); CHECK_STR(d->text, "edited while pending");
    CHECK(slack_draft_begin(d)); slack_draft_cancel(d); CHECK(d->uncertain); CHECK_STR(d->text, "edited while pending"); slack_draft_cancel(d);
    d->uncertain = false; CHECK(slack_draft_begin(d)); CHECK(slack_draft_finish(d, true, false, NULL) == SLACK_SEND_AMBIGUOUS);
    d->uncertain = false; CHECK(slack_draft_begin(d)); json_set_str(receipt, "channel", "D1"); CHECK(slack_draft_finish(d, true, false, receipt) == SLACK_SEND_AMBIGUOUS);
    SlackDraft *thread = slack_draft(&s, "1", "C1", "1.1"); free(thread->text); thread->text = xstrdup("thread draft");
    SlackDraft *other = slack_draft(&s, "2", "C1", NULL); free(other->text); other->text = xstrdup("other workspace");
    CHECK_STR(slack_draft(&s, "1", "C1", "1.1")->text, "thread draft"); CHECK_STR(slack_draft(&s, "1", "C1", NULL)->text, "edited while pending"); CHECK_INT(s.draft_count, 3);
    json_free(changed); json_free(receipt); slack_state_clear(&s); CHECK_INT(s.draft_count, 0);
    CHECK(!slack_text_valid(NULL)); CHECK(!slack_text_valid(" \r\n\t")); CHECK(slack_text_valid("a"));
    char *long_text = xmalloc(8002); memset(long_text, 'a', 8001); long_text[8001] = 0; CHECK(!slack_text_valid(long_text)); long_text[8000] = 0; CHECK(slack_text_valid(long_text)); free(long_text);
    Str emoji; str_init(&emoji); for (size_t i = 0; i < 4000; i++) str_appendz(&emoji, "\xF0\x9F\x98\x80"); CHECK(slack_text_valid(emoji.data));
    str_appendz(&emoji, "a"); CHECK(!slack_text_valid(emoji.data)); str_free(&emoji);
}
static void reads(void) {
    SlackState s = {0}; SlackRead *r = slack_read(&s, "1", "C1"); CHECK(slack_read_due(r, 9999) == NULL);
    slack_read_viewed(r, "1.1", 100); CHECK(slack_read_due(r, 1099) == NULL); CHECK_STR(slack_read_due(r, 1100), "1.1");
    slack_read_viewed(r, "1.2", 900); CHECK(slack_read_due(r, 1100) == NULL); CHECK_STR(slack_read_due(r, 1900), "1.2");
    slack_read_viewed(r, "1.1", 2000); slack_read_viewed(r, "bad", 2000); CHECK_STR(slack_read_due(r, 2000), "1.2");
    slack_read_confirm(r, "1.1"); CHECK_STR(slack_read_due(r, 2000), "1.2"); slack_read_confirm(r, "1.2"); CHECK(slack_read_due(r, 2000) == NULL);
    slack_read_confirm(r, "1.1"); slack_read_confirm(r, "bad"); CHECK_STR(r->marked, "1.2");
    slack_read(&s, "2", "C1"); CHECK_INT(s.read_count, 2); CHECK(slack_read_due(slack_read(&s, "2", "C1"), 3000) == NULL);
    CHECK_STR(slack_read(&s, "1", "C1")->viewed, "1.2"); slack_state_clear(&s);
}
static SlackEventResult event(SlackState *s, const char *name, const char *source) {
    Json *data = json_parsez(source); SlackEventResult result = slack_event_apply(s, "1", name, data); json_free(data); return result;
}
static void live_identity(void) {
    SlackState s = {0};
    CHECK(event(&s, "ready", "{\"workspaceId\":1,\"userId\":\"U1\",\"refresh\":true}") == SLACK_EVENT_READY);
    CHECK(event(&s, "ready", "{\"workspaceId\":2}") == SLACK_EVENT_IGNORED);
    CHECK(event(&s, "message", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"ts\":\"1.1\",\"text\":\"original\",\"bot_id\":\"B1\"}}") == SLACK_EVENT_APPLIED);
    event(&s, "message", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"ts\":\"1.1\",\"text\":\"original\"}}");
    CHECK_INT(s.message_count, 1);
    event(&s, "message.changed", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"message\":{\"ts\":\"1.1\",\"text\":\"edited\",\"reply_count\":2}}}");
    Json *receipt = json_parsez("{\"ts\":\"1.1\",\"text\":\"original\"}");
    CHECK(slack_message_receipt(&s, "1", "C1", receipt));
    CHECK_STR(json_str(json_get(s.messages[0].raw, "text")), "edited"); CHECK_STR(json_str(json_get(s.messages[0].raw, "bot_id")), "B1");
    CHECK_INT(json_int_or(json_get(s.messages[0].raw, "reply_count"), -1), 2);
    event(&s, "message.deleted", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"1.1\"}}");
    CHECK_INT(s.message_count, 0); CHECK(!slack_message_receipt(&s, "1", "C1", receipt)); CHECK(!slack_message_merge(&s, "1", "C1", receipt));
    CHECK(slack_message_merge(&s, "2", "C1", receipt)); CHECK(slack_message_merge(&s, "1", "G1", receipt));
    event(&s, "message.deleted", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"1.1\"}}"); CHECK_INT(json_count(s.deleted), 1);
    event(&s, "conversation.read", "{\"workspaceId\":1,\"channel\":\"C1\",\"ts\":\"9.1\"}");
    SlackRead *r = slack_read(&s, "1", "C1"); slack_read_viewed(r, "8.1", 0); CHECK(slack_read_due(r, 2000) == NULL);
    event(&s, "conversation.read", "{\"workspaceId\":1,\"channel\":\"C1\",\"ts\":\"7.1\"}"); CHECK_STR(r->marked, "9.1");
    CHECK(event(&s, "workspace.changed", "{\"workspaceId\":1}") == SLACK_EVENT_CHANGED);
    CHECK(event(&s, "workspace.removed", "{\"workspaceId\":1}") == SLACK_EVENT_REMOVED);
    CHECK(event(&s, "message.changed", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\"}}") == SLACK_EVENT_IGNORED);
    CHECK(event(&s, "message.deleted", "{\"workspaceId\":1,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"bad\"}}") == SLACK_EVENT_IGNORED);
    CHECK(event(&s, "conversation.read", "{\"workspaceId\":1,\"channel\":\"C1\",\"ts\":7}") == SLACK_EVENT_IGNORED);
    CHECK(event(&s, "unknown", "{\"workspaceId\":1}") == SLACK_EVENT_IGNORED);
    json_free(receipt); slack_state_clear(&s);
}
static void bounded_events(void) {
    SlackEvents q = {0};
    CHECK(slack_events_push(&q, "message", "first", 5)); CHECK(slack_events_push(&q, "message.changed", "second", 6));
    CHECK_STR(q.items[0].data, "first"); CHECK_STR(q.items[1].data, "second");
    for (size_t i = 2; i < SLACK_EVENTS_MAX; i++) CHECK(slack_events_push(&q, "message", "{}", 2));
    CHECK(!slack_events_push(&q, "message.deleted", "{}", 2)); CHECK(q.overflow); CHECK_INT(q.count, 0);
    CHECK(!slack_events_push(&q, "ready", "{}", 2)); slack_events_clear(&q);
    CHECK(!slack_events_push(&q, "message", "", SLACK_EVENTS_BYTES)); CHECK(q.overflow); slack_events_clear(&q);
    CHECK(slack_events_push(&q, "ready", "{}", 2)); slack_events_clear(&q);
    CHECK_INT(slack_reconnect_delay(1, -1), 1000); CHECK_INT(slack_reconnect_delay(4, -1), 8000);
    CHECK_INT(slack_reconnect_delay(99, -1), 60000); CHECK_INT(slack_reconnect_delay(1, 120), 120000);
    CHECK_INT(slack_reconnect_delay(1, 1e99), 2592000000ULL);
}
static void prune_revoked_conversations(void) {
    SlackState s = {0}; Json *message = json_parsez("{\"ts\":\"1.1\"}");
    slack_message_merge(&s, "1", "G1", message); slack_message_merge(&s, "2", "G1", message); slack_message_merge(&s, "1", "C1", message);
    slack_draft(&s, "1", "G1", "1.1"); slack_draft(&s, "2", "G1", NULL); slack_draft(&s, "1", "C1", NULL);
    slack_read(&s, "1", "G1"); slack_read(&s, "2", "G1"); slack_read(&s, "1", "C1");
    Json *rows = json_parsez("[{\"id\":\"C1\"}]"); slack_state_prune(&s, "1", rows);
    CHECK_INT(s.message_count, 2); CHECK_INT(s.draft_count, 2); CHECK_INT(s.read_count, 2);
    CHECK_STR(s.drafts[0].workspace, "2"); CHECK_STR(s.drafts[1].channel, "C1");
    slack_messages_clear(&s, "1", "C1"); CHECK_INT(s.message_count, 1);
    json_free(message); json_free(rows); slack_state_clear(&s);
}
void slack_tests(void) {
    test_run("Slack live edits bots own receipt overlap deletes and read synchronization", live_identity);
    test_run("Slack event buffers restart on count or byte overflow and reconnect backoff", bounded_events);
    test_run("Slack revoked channel snapshots prune private messages drafts and reads", prune_revoked_conversations);
    test_run("Slack timestamp strings and exclusive bounds", timestamps);
    test_run("Slack message identity edits deletion and stale generations", identity);
    test_run("Slack empty cursors history and replies pagination", pagination);
    test_run("Slack workspace conversation and people shapes", directory);
    test_run("Slack mrkdwn blocks and metadata without attachment transport", rendering);
    test_run("Slack scoped drafts confirmed changed ambiguous refused and cancelled sends", drafts);
    test_run("Slack debounced viewed read positions remain scoped and monotonic", reads);
}
