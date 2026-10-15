#include "mail_group.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

bool mail_message_is_notification(const MailMessage *m) {
    static const char *const labels[] = { "CATEGORY_PROMOTIONS", "CATEGORY_UPDATES", "CATEGORY_SOCIAL", "CATEGORY_FORUMS" };
    for (size_t i = 0; i < m->label_count; i++)
        for (size_t k = 0; k < sizeof labels / sizeof *labels; k++)
            if (str_eq(m->labels[i], labels[k])) return true;
    static const char *const senders[] = { "noreply", "no-reply", "donotreply", "notifications@", "notify@", "mailer-daemon" };
    for (size_t i = 0; i < sizeof senders / sizeof *senders; i++)
        if (str_icontains(m->sender, senders[i])) return true;
    return false;
}
static bool same_thread(const MailMessage *a, const MailMessage *b) {
    return a->account_id == b->account_id && !str_empty(a->thread_id) && str_eq(a->thread_id, b->thread_id);
}
MailGroups mail_groups_build(const MailMessages *messages, bool threads) {
    MailGroups result = {0};
    if (!messages->count) return result;
    bool *used = xcalloc(messages->count, sizeof *used);
    size_t cap = 0;
    for (size_t i = 0; i < messages->count; i++) {
        if (used[i]) continue;
        if (result.count == cap) { cap = cap ? cap * 2 : 8; result.groups = xrealloc(result.groups, cap * sizeof *result.groups); }
        MailGroup *c = &result.groups[result.count++];
        memset(c, 0, sizeof *c);
        c->primary = (int)i;
        size_t mcap = 4;
        c->members = xcalloc(mcap, sizeof *c->members);
        for (size_t k = i; k < messages->count; k++) {
            if (k != i && (!threads || !same_thread(&messages->messages[i], &messages->messages[k]))) continue;
            used[k] = true;
            if (c->count == mcap) { mcap *= 2; c->members = xrealloc(c->members, mcap * sizeof *c->members); }
            c->members[c->count++] = (int)k;
            if (!messages->messages[k].is_read) c->unread = true;
            if (messages->messages[k].is_starred) c->starred = true;
        }
        for (size_t a = 0, b = c->count; a + 1 < b; a++) {
            b--; int tmp = c->members[a]; c->members[a] = c->members[b]; c->members[b] = tmp;
        }
        c->notification = mail_message_is_notification(&messages->messages[c->primary]);
    }
    free(used);
    return result;
}
void mail_groups_free(MailGroups *groups) {
    for (size_t i = 0; i < groups->count; i++) free(groups->groups[i].members);
    free(groups->groups);
    memset(groups, 0, sizeof *groups);
}
