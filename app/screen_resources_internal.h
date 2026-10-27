#ifndef BRIAREUS_SCREEN_RESOURCES_INTERNAL_H
#define BRIAREUS_SCREEN_RESOURCES_INTERNAL_H
#include "screens.h"
#include "stats.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

enum { RESOURCES_POLL = 1, RESOURCES_REFRESH = 1000 };
typedef struct {
    Screen base;
    Session session;
    ApiClient *client;
    Json *device;
    char *retry_key, *error;
    SessionStats reading;
    Request *read;
    Poller poller;
    bool shown, loaded, fresh;
    double received_at;
} ResourcesScreen;
bool resources_current(ResourcesScreen *s);
double resources_retry_until(ResourcesScreen *s);
void resources_layout(Screen *base, Doc *doc);
#endif
