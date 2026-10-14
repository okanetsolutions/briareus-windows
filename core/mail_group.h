// Account-scoped conversations over a newest-first mail snapshot.
#ifndef BRIAREUS_MAIL_GROUP_H
#define BRIAREUS_MAIL_GROUP_H
#include "mail.h"

typedef struct {
    int primary, *members;
    size_t count;
    bool unread, starred, notification;
} MailGroup;
typedef struct { MailGroup *groups; size_t count; } MailGroups;

bool mail_message_is_notification(const MailMessage *message);
/// Owns groups and member indices, freed with mail_groups_free; never retains or changes messages.
/// Input is newest first as produced by mail_messages_append. Primary is newest; members are oldest first.
/// With threads false every message is separate; empty thread IDs never join other messages.
MailGroups mail_groups_build(const MailMessages *messages, bool threads);
void mail_groups_free(MailGroups *groups);
#endif
