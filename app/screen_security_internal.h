#ifndef BRIAREUS_SCREEN_SECURITY_INTERNAL_H
#define BRIAREUS_SCREEN_SECURITY_INTERNAL_H
#include "screens.h"
#include "review_outcome_card.h"
#include "review_security.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>

// The Security screen, opened from Status: every open pull request of the token's projects with its security pass, worst
// first. The server lists no security results across pull requests, so the screen reads the projects, each project's
// open pull requests, then each pull request's rounds and outcome, one request at a time and at most
// SECURITY_PULL_LIMIT pull requests. Hiding the screen pauses the reads where they stand; showing it again resumes them.
enum { SECURITY_REFRESH = 1900, SECURITY_OPEN };
enum { SECURITY_PULL_LIMIT = 100 };
typedef struct {
    Project project; int number; char *title;
    bool read;                 // its outcome was asked for and answered (or refused, with `error`)
    ReviewOutcome outcome;     // valid when `has_outcome`
    bool has_outcome;
    char *error;
} SecurityPull;
typedef struct {
    Screen base;
    ApiClient *client; Json *device;   // the connection and token the reads belong to
    Project *projects; size_t project_count, next_project;
    SecurityPull *pulls; size_t pull_count, next_pull;
    Request *read;
    // `error` stops the scan (the project list could not be read); `skipped` names projects whose pull requests could not.
    char *error, *skipped;
    bool shown, started, capped;
    unsigned generation;   // changes whenever the list is replaced, so a click laid out before names the list it saw
} SecurityScreen;
bool security_current(SecurityScreen *s);
// How many pull requests have their outcome read, and whether the scan has ended.
size_t security_read_count(const SecurityScreen *s);
bool security_done(const SecurityScreen *s);
void security_layout(Screen *base, Doc *doc);
#endif
