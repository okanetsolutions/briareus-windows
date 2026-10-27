#include "webhook.h"
#include "str.h"
#include <errno.h>
#include <stdlib.h>

const WebhookFieldDef webhook_fields[WEBHOOK_FIELD_COUNT] = {
    [WEBHOOK_PER_HOUR] = { "perHour", "Deliveries an hour", "Past it a sender is answered 429 with Retry-After. From 1 to 600.", 1, 600, 30 },
    [WEBHOOK_MAX_TURNS] = { "maxTurns", "Turns in a row", "Turns deliveries may start with no word from you; past it the webhook pauses until you say anything here (or save this form). From 0 to 1000; 0 means no limit.", 0, 1000, 10 },
};

char *webhook_field_text(const Json *hook, WebhookField field) {
    const WebhookFieldDef *def = &webhook_fields[field];
    return xstrfmt("%d", json_int_or(json_get(hook, def->key), def->fallback));
}

Json *webhook_save(bool armed, bool instructions, bool ssh_unattended,
                   const char *const caps[WEBHOOK_FIELD_COUNT], WebhookField *invalid) {
    Json *body = json_object();
    json_set_bool(body, "armed", armed);
    json_set_bool(body, "instructions", instructions);
    json_set_bool(body, "sshUnattended", ssh_unattended);
    for (int f = 0; f < WEBHOOK_FIELD_COUNT; f++) {
        const WebhookFieldDef *def = &webhook_fields[f];
        char *text = str_trim(caps[f] ? caps[f] : ""), *end = NULL;
        errno = 0;
        long n = strtol(text, &end, 10);
        bool ok = *text && !*end && errno != ERANGE && n >= def->min && n <= def->max;
        free(text);
        if (!ok) {
            *invalid = (WebhookField)f;
            json_free(body);
            return NULL;
        }
        json_set_num(body, def->key, (double)n);
    }
    return body;
}
