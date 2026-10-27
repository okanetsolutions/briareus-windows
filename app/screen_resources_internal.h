#ifndef BRIAREUS_SCREEN_RESOURCES_INTERNAL_H
#define BRIAREUS_SCREEN_RESOURCES_INTERNAL_H
#include "screens.h"
#include "server_stats.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

enum { RESOURCES_POLL = 1, RESOURCES_REFRESH = 1000, RESOURCES_OPEN };
typedef enum { RESOURCE_SESSION, RESOURCE_SERVER, RESOURCE_LIST } ResourceKind;
typedef struct ResourcesScreen {
    Screen base;
    struct ResourcesScreen *next;
    ResourceKind kind;
    struct ResourcesScreen *list; // server dashboard's independent sessions read channel
    Session session;
    ServerStats server;
    OpenStats sessions;
    struct ResourcesScreen *dashboard; // borrowed parent, invalidates rendered row actions
    intptr_t action_first, action_next; size_t action_count;
    ApiClient *client;
    Json *device;
    char *retry_key, *error;
    SessionStats reading;
    Request *read;
    Poller poller;
    bool shown, loaded, fresh;
    double received_at, reading_epoch;
} ResourcesScreen;
bool resources_current(ResourcesScreen *s);
double resources_retry_until(ResourcesScreen *s);
void resources_discard_revoked(ResourcesScreen *s);
const char *resources_operation(ResourcesScreen *s);
void resources_clear_figures(ResourcesScreen *s);
bool resources_adopt(ResourcesScreen *s, const Json *answer);
void server_resources_layout(Screen *base, Doc *doc);
void resources_layout(Screen *base, Doc *doc);
#endif
