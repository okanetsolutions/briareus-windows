// Fixtures match nadinyamaui/briareus mail merge 4eba29400a1bd0a7072a9991395332fbd710f6bf.
#include "mail.h"
#include "mail_group.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Json *message(int account, const char *id, double date) {
    Json *j = json_parsez("{\"from\":{\"name\":\"Sender\",\"address\":\"sender@example.com\"},\"to\":[{\"address\":\"recipient@example.com\"}],\"cc\":[{\"name\":\"Copy\",\"address\":\"cc@example.com\"}],\"replyTo\":[{\"address\":\"reply@example.com\"}],\"subject\":\"Subject\",\"snippet\":\"Preview\",\"threadId\":\"thread/+=\",\"labels\":[\"INBOX\",\"My folder\"],\"isRead\":false,\"inInbox\":true,\"isStarred\":true,\"attachments\":[{\"id\":\"a\",\"name\":\"report.pdf\",\"mimeType\":\"application/pdf\",\"size\":2048}],\"webUrl\":\"https://outlook.office.com/mail/id/abc\",\"body\":{\"text\":\"HTML-only mail rendered as text <script>literal</script>\\n[link](https://evil.example)\",\"html\":\"<script>alert(1)</script><img src='https://remote'>\",\"truncated\":true}}");
    json_set_num(j, "accountId", account); json_set_str(j, "id", id); json_set_num(j, "receivedAt", date); return j;
}
static Json *page(int account, const char *id, double date, const char *cursor) {
    Json *j = json_object(), *rows = json_array(); json_array_push(rows, message(account, id, date));
    json_object_set(j, "messages", rows); json_object_set(j, "nextCursor", json_string_or_null(cursor)); return j;
}
static void bodies(void) {
    Json *j = message(7, "a/+=", 123456); MailMessage m;
    CHECK(mail_message_parse(j, false, &m)); CHECK(m.text == NULL); CHECK(!m.truncated);
    CHECK_INT(m.account_id, 7); CHECK_STR(m.id, "a/+="); CHECK_STR(m.sender, "Sender <sender@example.com>");
    CHECK_STR(m.to, "recipient@example.com"); CHECK_STR(m.cc, "Copy <cc@example.com>"); CHECK_STR(m.reply_to, "reply@example.com");
    CHECK_STR(m.thread_id, "thread/+="); CHECK_STR(m.subject, "Subject"); CHECK_STR(m.snippet, "Preview");
    CHECK(m.in_inbox && !m.is_read && m.is_starred); CHECK_INT(m.label_count, 2); CHECK_STR(m.labels[1], "My folder");
    CHECK(m.received_at == 123456); CHECK_STR(m.web_url, "https://outlook.office.com/mail/id/abc");
    CHECK_INT(m.attachment_count, 1); CHECK_STR(m.attachments[0].name, "report.pdf"); CHECK(m.attachments[0].size == 2048);
    CHECK_STR(m.attachments[0].id, "a"); CHECK_STR(m.attachments[0].mime_type, "application/pdf"); mail_message_free(&m);
    CHECK(mail_message_parse(j, true, &m)); CHECK(m.truncated); CHECK(strstr(m.text, "<script>literal</script>") != NULL);
    CHECK(strstr(m.text, "alert(1)") == NULL); CHECK(!m.is_read); mail_message_free(&m);
    Json *b = json_object(); json_set_str(b, "html", "<img src='https://remote'>"); json_object_set(j, "body", b);
    CHECK(mail_message_parse(j, true, &m)); CHECK(m.text == NULL); mail_message_free(&m); // No client HTML fallback.
    json_object_remove(j, "body"); CHECK(!mail_message_parse(j, true, &m));
    json_set_str(j, "webUrl", "javascript:alert(1)"); CHECK(mail_message_parse(j, false, &m)); CHECK(m.web_url == NULL); mail_message_free(&m);
    json_set_str(j, "id", ""); CHECK(!mail_message_parse(j, false, &m)); json_set_str(j, "id", "a");
    json_set_num(j, "accountId", -1); CHECK(!mail_message_parse(j, false, &m));
    json_set_num(j, "accountId", 7.5); CHECK(!mail_message_parse(j, false, &m)); json_free(j);
    const char *bad[] = { NULL, "", "https://", "http://gmail.com", "javascript:alert(1)", "data:text/html,hello", "https://user@host/", "https://host\\evil/", "https://host/\nfile", "https://host/ a", "https://evil%40host/", "https://%65vil.example/" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!mail_message_web_url_safe(bad[i]));
    CHECK(mail_message_web_url_safe("https://mail.google.com/mail/u/0/#inbox/id"));
    CHECK(mail_message_web_url_safe("https://AZaz09-host.example:8443/message"));
    CHECK(mail_message_web_url_safe("https://[::1]/message"));
    CHECK(!mail_message_web_url_safe("https://host_name/message"));
    CHECK(!mail_message_web_url_safe("https://host\177/message"));
}
static void pagination(void) {
    MailMessages all = {0}, p;
    Json *j = page(7, "same", 200, "opaque/+="); CHECK(mail_messages_parse(j, &p)); json_free(j);
    mail_messages_append(&all, &p); CHECK_INT(all.count, 1); CHECK_STR(all.next_cursor, "opaque/+="); CHECK(p.messages == NULL);
    j = page(8, "same", 300, "second"); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_append(&all, &p);
    CHECK_INT(all.count, 2); CHECK_INT(all.messages[0].account_id, 8);
    j = page(7, "same", 200, "third"); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_append(&all, &p); CHECK_INT(all.count, 2);
    j = page(7, "older", 100, NULL); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_append(&all, &p);
    CHECK_INT(all.count, 3); CHECK(all.next_cursor == NULL); CHECK_STR(all.messages[2].id, "older");
    CHECK(mail_message_find(&all, 8, "same") != NULL); CHECK(mail_message_find(&all, 7, "same") != NULL);
    CHECK(mail_message_find(&all, 9, "same") == NULL); CHECK(mail_message_find(&all, 7, "absent") == NULL);
    j = page(7, "same", 200, ""); CHECK(!mail_messages_parse(j, &p)); json_free(j);
    j = json_parsez("{\"messages\":[{\"id\":\"bad\"}],\"nextCursor\":null}"); CHECK(!mail_messages_parse(j, &p)); CHECK(p.messages == NULL); json_free(j);
    j = json_parsez("{\"messages\":[],\"nextCursor\":4}"); CHECK(!mail_messages_parse(j, &p)); json_free(j);
    j = json_object(); CHECK(!mail_messages_parse(j, &p)); json_free(j);
    j = json_parsez("{\"messages\":[],\"nextCursor\":null}"); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_free(&p);
    mail_messages_free(&all); CHECK(all.messages == NULL && all.count == 0);
}
static void privacy(void) {
    MailMessages all = {0}, p; Json *j = page(7, "one", 100, "next"); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_append(&all, &p);
    j = page(8, "two", 200, "next"); CHECK(mail_messages_parse(j, &p)); json_free(j); mail_messages_append(&all, &p);
    j = json_parsez("{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"status\":\"connected\"}],\"providers\":[\"gmail\"]}");
    MailAccounts a; CHECK(mail_accounts_parse(j, &a)); json_free(j);
    CHECK(mail_account_readable(&a, 7)); CHECK(!mail_account_readable(&a, 8)); CHECK(mail_messages_prune(&all, &a));
    CHECK_INT(all.count, 1); CHECK_INT(all.messages[0].account_id, 7); CHECK(all.next_cursor == NULL); CHECK(!mail_messages_prune(&all, &a));
    free(a.accounts[0].status); a.accounts[0].status = xstrdup("reauth"); CHECK(!mail_account_readable(&a, 7));
    CHECK(mail_messages_prune(&all, &a)); CHECK_INT(all.count, 0); mail_messages_free(&all); mail_accounts_free(&a);
}
static void filters(void) {
    MailFilter f; mail_filter_init(&f); Json *j = mail_filter_args(&f, NULL);
    CHECK_INT(json_count(j), 1); CHECK_INT(json_int_or(json_get(j, "limit"), 0), 50); json_free(j);
    f.account = 8; f.q = xstrdup("query &="); f.label = xstrdup("My /folder"); f.thread = xstrdup("thread/+=");
    for (int flag = 0; flag <= 1; flag++) {
        f.unread = f.inbox = f.starred = flag; j = mail_filter_args(&f, "cursor/+=");
        CHECK_INT(json_int_or(json_get(j, "account"), 0), 8); CHECK_STR(json_str(json_get(j, "q")), "query &=");
        CHECK_STR(json_str(json_get(j, "label")), "My /folder"); CHECK_STR(json_str(json_get(j, "thread")), "thread/+=");
        CHECK_STR(json_str(json_get(j, "cursor")), "cursor/+=");
        CHECK_INT(json_int_or(json_get(j, "unread"), -1), flag); CHECK_INT(json_int_or(json_get(j, "inbox"), -1), flag);
        CHECK_INT(json_int_or(json_get(j, "starred"), -1), flag); json_free(j);
    }
    mail_filter_free(&f); CHECK(f.q == NULL && f.account == 0 && f.unread == -1);
    j = mail_filter_args(&f, ""); CHECK_INT(json_count(j), 1); json_free(j);
    const int statuses[] = { 0, 400, 401, 403, 404, 409, 429, 500, 503 };
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) CHECK(!str_empty(mail_read_error(statuses[i], false)));
    CHECK(strstr(mail_read_error(404, true), "removed") != NULL);
}
static void conversations(void) {
    MailMessage rows[] = {
        { .account_id = 7, .id = "latest", .thread_id = "shared", .sender = "Person <person@example.com>", .received_at = 400, .is_read = true },
        { .account_id = 8, .id = "other-account", .thread_id = "shared", .sender = "notify@example.com", .received_at = 300, .is_read = true },
        { .account_id = 7, .id = "middle", .thread_id = "shared", .sender = "no-reply@example.com", .received_at = 200, .is_starred = true },
        { .account_id = 7, .id = "oldest", .thread_id = "shared", .received_at = 100, .is_read = true },
    };
    MailMessages messages = { .messages = rows, .count = sizeof rows / sizeof *rows };
    MailGroups groups = mail_groups_build(&messages, true);
    CHECK_INT(groups.count, 2);
    CHECK_INT(groups.groups[0].primary, 0); CHECK_INT(groups.groups[0].count, 3);
    CHECK_INT(groups.groups[0].members[0], 3); CHECK_INT(groups.groups[0].members[1], 2); CHECK_INT(groups.groups[0].members[2], 0);
    CHECK(groups.groups[0].unread && groups.groups[0].starred);
    CHECK(!groups.groups[0].notification); // The primary controls classification, even when an older member is automated.
    CHECK_INT(groups.groups[1].primary, 1); CHECK_INT(groups.groups[1].members[0], 1);
    CHECK(!groups.groups[1].unread && !groups.groups[1].starred && groups.groups[1].notification);
    CHECK_STR(rows[0].id, "latest"); CHECK(rows[0].is_read); CHECK(!rows[2].is_read);
    mail_groups_free(&groups); CHECK(groups.groups == NULL && groups.count == 0);
    mail_groups_free(&groups);
}
static void separate_messages(void) {
    MailMessage rows[] = {
        { .account_id = 7, .thread_id = NULL, .is_read = true },
        { .account_id = 7, .thread_id = "", .is_read = true },
        { .account_id = 7, .thread_id = NULL, .is_starred = true },
        { .account_id = 7, .thread_id = "thread", .is_read = true },
        { .account_id = 7, .thread_id = "thread", .is_read = true },
    };
    MailMessages messages = { .messages = rows, .count = sizeof rows / sizeof *rows };
    MailGroups groups = mail_groups_build(&messages, true);
    CHECK_INT(groups.count, 4);
    for (size_t i = 0; i < 3; i++) {
        CHECK_INT(groups.groups[i].primary, i); CHECK_INT(groups.groups[i].count, 1); CHECK_INT(groups.groups[i].members[0], i);
    }
    CHECK_INT(groups.groups[3].count, 2); CHECK_INT(groups.groups[3].members[0], 4); CHECK_INT(groups.groups[3].members[1], 3);
    mail_groups_free(&groups);
    groups = mail_groups_build(&messages, false); CHECK_INT(groups.count, 5);
    for (size_t i = 0; i < groups.count; i++) {
        CHECK_INT(groups.groups[i].primary, i); CHECK_INT(groups.groups[i].count, 1); CHECK_INT(groups.groups[i].members[0], i);
    }
    CHECK(!groups.groups[1].unread && !groups.groups[1].starred);
    CHECK(groups.groups[2].unread && groups.groups[2].starred);
    mail_groups_free(&groups);
    messages = (MailMessages){0}; groups = mail_groups_build(&messages, true);
    CHECK(groups.groups == NULL && groups.count == 0); mail_groups_free(&groups);
}
static void large_conversations(void) {
    MailMessage rows[16] = {0};
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        rows[i].account_id = 7; rows[i].thread_id = xstrfmt("thread-%d", i < 6 ? 0 : (int)i);
        rows[i].is_read = true;
    }
    MailMessages messages = { .messages = rows, .count = sizeof rows / sizeof *rows };
    MailGroups groups = mail_groups_build(&messages, true);
    CHECK_INT(groups.count, 11); CHECK_INT(groups.groups[0].count, 6);
    CHECK_INT(groups.groups[0].primary, 0); CHECK_INT(groups.groups[0].members[0], 5); CHECK_INT(groups.groups[0].members[5], 0);
    CHECK_INT(groups.groups[10].primary, 15); CHECK_INT(groups.groups[10].members[0], 15);
    mail_groups_free(&groups);
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) free(rows[i].thread_id);
}
static void notification_classification(void) {
    MailMessage m = {0}; CHECK(!mail_message_is_notification(&m));
    char *labels[] = { NULL, "INBOX", "CATEGORY_PROMOTIONS" };
    m.labels = labels; m.label_count = sizeof labels / sizeof *labels;
    const char *categories[] = { "CATEGORY_PROMOTIONS", "CATEGORY_UPDATES", "CATEGORY_SOCIAL", "CATEGORY_FORUMS" };
    for (size_t i = 0; i < sizeof categories / sizeof *categories; i++) {
        labels[2] = (char *)categories[i]; CHECK(mail_message_is_notification(&m));
    }
    labels[2] = "category_updates"; CHECK(!mail_message_is_notification(&m));
    const char *senders[] = { "NoReply@example.com", "Sender <NO-REPLY@example.com>", "DONOTREPLY@example.com",
        "NOTIFICATIONS@example.com", "NOTIFY@example.com", "MAILER-DAEMON@example.com" };
    for (size_t i = 0; i < sizeof senders / sizeof *senders; i++) {
        m.sender = (char *)senders[i]; CHECK(mail_message_is_notification(&m));
    }
    m.sender = "Person <person@example.com>"; CHECK(!mail_message_is_notification(&m));
    m.sender = ""; CHECK(!mail_message_is_notification(&m));
}
void mail_inbox_tests(void) {
    test_run("mail conversations isolate accounts and aggregate member flags without changing messages", conversations);
    test_run("mail conversation modes keep absent thread IDs and ungrouped messages separate", separate_messages);
    test_run("mail conversation ownership supports many groups and a long thread", large_conversations);
    test_run("mail notification classification recognizes category labels and automated senders", notification_classification);
    test_run("mail models retain only literal text with attachment metadata and safe provider URLs", bodies);
    test_run("mail cursor pages deduplicate account+id and preserve newest-first order", pagination);
    test_run("mail account removal and reauth purge private rows and old pagination", privacy);
    test_run("mail filters preserve all tri-state flags and opaque cursor arguments", filters);
}
