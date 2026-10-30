#ifndef BRIAREUS_SCREEN_CI_INTERNAL_H
#define BRIAREUS_SCREEN_CI_INTERNAL_H
#include "screens.h"
#include "ci.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
enum { CI_POLL = 1, CI_REFRESH = 1800, CI_DAYS, CI_KIND, CI_REPO, CI_BRANCH, CI_MORE, CI_OPEN };
typedef struct {
    Screen base;
    ApiClient *client;
    Json *device, *window, *stats, *runs;
    Request *reads[2];
    char *errors[2], *repo, *branch;
    double retry[2]; int failures[2];
    int days, offset, requested_offset;
    bool statuses, shown, truncated, capped;
    intptr_t action_first, action_next;
    Poller poller;
    time_t (*clock)(void); // NULL uses the wall clock; tests advance deadlines without sleeping.
} CIScreen;
double ci_now(const CIScreen *s);
bool ci_current(CIScreen *s);
void ci_load(CIScreen *s, bool reset);
void ci_layout(Screen *base, Doc *doc);
#endif
