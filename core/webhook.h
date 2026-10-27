#ifndef BRIAREUS_WEBHOOK_H
#define BRIAREUS_WEBHOOK_H
#include "json.h"

typedef enum { WEBHOOK_PER_HOUR, WEBHOOK_MAX_TURNS, WEBHOOK_FIELD_COUNT } WebhookField;
typedef struct { const char *key, *label, *hint; int min, max, fallback; } WebhookFieldDef;
extern const WebhookFieldDef webhook_fields[WEBHOOK_FIELD_COUNT];

// Owned text for a record's cap, using the server default only when it lacks an integer.
char *webhook_field_text(const Json *hook, WebhookField field);
// Owned PUT body, or NULL with the first invalid field in *invalid. Explicit zero is unlimited turns.
Json *webhook_save(bool armed, bool instructions, bool ssh_unattended,
                   const char *const caps[WEBHOOK_FIELD_COUNT], WebhookField *invalid);
#endif
