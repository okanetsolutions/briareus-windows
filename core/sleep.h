#ifndef BRIAREUS_SLEEP_H
#define BRIAREUS_SLEEP_H
#include "models.h"
#include <stdint.h>

enum { SLEEP_PANES = 4, SLEEP_TICKER = 30, SLEEP_STEPS = 40, SLEEP_TEXT = 32768 };
// Worker-side bounded backlog, with no synchronization of its own. A dropped session is repaired by /sessions.
enum { SLEEP_BACKLOG = 128 };
typedef struct { Json *events[SLEEP_BACKLOG]; size_t count; bool reconcile; } SleepInbox;
// Consumes data. False means ordered live/deletion backlog overflow; session overflow never closes a stream.
bool sleep_inbox_push(SleepInbox *q, const char *type, Json *data);
Json *sleep_inbox_take(SleepInbox *q, bool *reconcile); // owned FIFO batch; resets the backlog
void sleep_inbox_free(SleepInbox *q);
extern const int sleep_choices[7];
int sleep_minutes(int value); // invalid persisted values use the five minute default
// Monotonic milliseconds; input is filtered before native accelerator/character dispatch.
typedef struct { uint64_t input, swallow; bool asleep; uint32_t held[9], blocked[9]; } SleepIdle;
bool sleep_due(SleepIdle *s, uint64_t now, int minutes, bool eligible);
bool sleep_input(SleepIdle *s, uint64_t now, unsigned input, bool down, bool up);

// Native windows can synthesize moves when a child appears under a parked cursor.
typedef struct { int x, y; bool known; } SleepPointer;
void sleep_pointer_reset(SleepPointer *p, int x, int y);
bool sleep_pointer_moved(SleepPointer *p, int x, int y);

typedef enum { SLEEP_NONE, SLEEP_FILE, SLEEP_COMMAND } SleepKind;
typedef struct { SleepKind kind; bool writes; char *text; } SleepStep;
// Owned normalized repository-relative path, or NULL for unsafe/outside paths.
char *sleep_path(const char *path, const char *repo);
SleepStep sleep_step(const Json *event, const char *repo);
void sleep_step_free(SleepStep *step);
// Bounded normalized history. One step per beat, round robin across nonempty session queues.
typedef struct { SleepStep steps[SLEEP_PANES][SLEEP_STEPS]; size_t count[SLEEP_PANES], position[SLEEP_PANES], turn; } SleepReplay;
void sleep_replay_clear(SleepReplay *r, int pane);
void sleep_replay_load(SleepReplay *r, int pane, const Json *events, const char *repo);
// Borrowed step until the replay is cleared; *pane receives its assignment slot.
const SleepStep *sleep_replay_next(SleepReplay *r, int *pane);

typedef struct {
    Session session;
    char *path, *pending, *command, *content;
    bool writes, pending_writes, replay, loaded;
    uint64_t touched, since, revision, identity;
    size_t typed;
    int cursor, file_cursor, command_cursor;
    bool seeded;
} SleepPane;
typedef struct { char *project, *text; bool replay; } SleepCommand;
typedef struct {
    SleepPane panes[SLEEP_PANES];
    SleepCommand ticker[SLEEP_TICKER]; size_t commands;
    uint64_t serial, last_live;
} SleepWall;
void sleep_wall_free(SleepWall *w);
// Keeps existing assignments; a fifth session replaces the least recently touched pane.
int sleep_assign(SleepWall *w, const Session *s, uint64_t now, bool replay);
void sleep_remove(SleepWall *w, const char *id);
bool sleep_replay_due(const SleepWall *w, uint64_t now);
void sleep_apply(SleepWall *w, int pane, const SleepStep *step, uint64_t now, bool replay);
void sleep_tick(SleepWall *w, uint64_t now);
// Seed latest file/command independently of newer non-display live events; never rewinds displayed steps.
void sleep_seed(SleepWall *w, int pane, const Json *events, uint64_t now);
// Owned repo_file arguments; branch omitted on fallback. Revision guards file/branch/assignment changes.
Json *sleep_file_args(const SleepPane *p, bool fallback);
bool sleep_loaded(SleepPane *p, uint64_t revision, const Json *answer);
// Newest recent sessions first, stable for ties; includes closed sessions when replaying and excludes sessions without repositories.
size_t sleep_select(const Session *items, size_t count, size_t out[SLEEP_PANES], bool working);
#endif
