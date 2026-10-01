// What the client API answers with, read into the shapes the screens use. Unknown fields ride along in `raw`.
#ifndef BRIAREUS_MODELS_H
#define BRIAREUS_MODELS_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/// Defines `T *name(const Json *array, size_t *count)`: reads each element with `bool parse(const Json *, T *)` and keeps
/// the ones that read. Prefix with `static` for a private one.
#define DEFINE_LIST_PARSE(T, name, parse)     T *name(const Json *array, size_t *count) {         size_t n = json_count(array), m = 0;         T *out = xcalloc(n, sizeof *out);         for (size_t i = 0; i < n; i++) if (parse(json_at(array, i), &out[m])) m++;         *count = m;         return out;     }
/// Defines `void name(T *items, size_t count)`: frees each item with `free_one(T *)`, then the array. NULL is fine.
#define DEFINE_LIST_FREE(T, name, free_one)     void name(T *items, size_t count) { if (!items) return; for (size_t i = 0; i < count; i++) free_one(&items[i]); free(items); }

// MARK: - Connection

/// The token's own record (`client` in what `GET /` answers): this device's token.
typedef struct {
    char *id, *label, *permission;   // permission: read, manage or admin
    char **repos; size_t repo_count;  // empty for an admin token, which is held to no project list
    double expires_at;   // milliseconds since 1970
} Device;
bool device_parse(const Json *value, Device *out);
Json *device_json(const Device *device);
void device_free(Device *device);
void device_copy(Device *into, const Device *from);
/// A manage or admin token may write.
bool device_can_manage(const Device *device);
time_t device_expiry(const Device *device);
/// read 0, manage 1, admin 2; -1 for a permission this app does not know, which may do nothing.
int permission_rank(const char *permission);

/// -1 when the server left transcription out (a server from before voice notes), else 0 or 1.
typedef struct { int version; Device device; int transcribe; } Discovery;
bool discovery_parse(const Json *value, Discovery *out);
void discovery_free(Discovery *discovery);
/// What the server's owner has to do before voice notes work, or NULL when they do.
const char *discovery_voice_notes_off(int transcribe);

/// One route the server lists: its method, its path with `{...}` for each parameter, and the least permission that may call it.
typedef struct { char *method, *path, *access; } Route;
/// Reads the server's OpenAPI document (`paths`, with `x-briareus-access` on each operation) or a saved list of routes.
bool routes_parse(const Json *openapi_or_saved, Route **out, size_t *count);
Json *routes_json(const Route *routes, size_t count);
void routes_free(Route *routes, size_t count);
Route *routes_copy(const Route *routes, size_t count);
/// Whether the server has `method path` (a `{...}` segment on either side matches any) and `permission` ranks high enough.
bool routes_allow(const Route *routes, size_t count, const char *method, const char *path, const char *permission);

/// What pairing learned about this device, saved so the next launch opens without asking again first.
typedef struct { Device device; Route *routes; size_t route_count; int transcribe; } Connection;
bool connection_parse(const Json *value, Connection *out);
Json *connection_json(const Connection *connection);
void connection_free(Connection *connection);

// MARK: - Projects and sessions

typedef struct { char *repo; char *label; bool has_local; } Project;   // has_local: a local checkout a session can work in
bool project_parse(const Json *value, Project *out);
Json *project_json(const Project *project);
void project_free(Project *project);
void project_copy(Project *into, const Project *from);
/// The label when it has one, else the repository name.
const char *project_title(const Project *project);
bool projects_parse(const Json *list_or_array, Project **out, size_t *count);
Json *projects_json(const Project *projects, size_t count);
void projects_free(Project *projects, size_t count);

/// A conversation as the server sent it. The object is kept whole, so saved lists round-trip.
typedef struct { Json *raw; } Session;
bool session_parse(const Json *value, Session *out);
void session_free(Session *session);
void session_copy(Session *into, const Session *from);
const char *session_id(const Session *s);
const char *session_repo(const Session *s);
const char *session_status(const Session *s);
const char *session_model(const Session *s);
const char *session_provider(const Session *s);
const char *session_display_title(const Session *s);
bool session_is_active(const Session *s);
bool session_live_input(const Session *s);
const Json *session_queued(const Session *s);
bool session_review_loop_on(const Session *s);
/// The server arms the review loop only on sessions started from scratch on a task.
bool session_can_review_loop(const Session *s);
/// A review round waiting for verdicts: a loop's round or a hand-started review. NULL without one.
const Json *session_held_triage(const Session *s);
/// The pull request this conversation works on, once it has one; 0 without.
int session_pull_number(const Session *s);
/// Whether it was started on issue `number`: such a session is named by its prompt's first line, `Issue #N: title`.
bool session_on_issue(const Session *s, int number);
bool sessions_parse(const Json *list_or_array, Session **out, size_t *count);
Json *sessions_json(const Session *sessions, size_t count);
void sessions_free(Session *sessions, size_t count);

// MARK: - Findings

/// The review round a conversation holds for a decision, as the dashboard's Findings screen queues them: a loop's round
/// or a hand-started review, even one whose every finding was deleted. NULL without one.
const Json *session_held_round(const Session *s);
typedef struct { size_t index; const Json *held; } HeldRound;
/// The conversations holding a round, the oldest hold first, as the dashboard lists them.
HeldRound *sessions_held_rounds(const Session *sessions, size_t count, size_t *out_count);
/// The pull request a round is about, as a link: the conversation's own when that is the same pull request, and built
/// from the number otherwise (a conversation that moved on to another pull request still holds this round). A round without
/// a number has the conversation's link, or NULL. New string.
char *held_round_pr_url(const Session *s, const Json *held);
/// A hand-started review says whose pull request it is; a loop round is always the user's, and takes verdicts.
bool held_round_is_mine(const Json *held);
/// The pull request number a round was left on; 0 without one.
int held_round_pr_number(const Json *held);
/// What `complete_findings` answered, in the dashboard's words; `*danger` is set when the verdicts led nowhere.
char *triage_outcome_text(const Json *outcome, bool *danger);
/// The Findings screen's subtitle: "nothing is waiting", or how many reviews and pull requests wait for a decision.
char *findings_subtitle(size_t rounds, size_t pull_requests);

// MARK: - Transcript

typedef struct {
    Json *raw;
    int seq;
    const char *kind, *t, *text, *name, *summary, *question;
    const Json *options, *attachments;
    bool has_cost, has_duration; double cost_usd, duration_ms;
    int is_error;   // -1 unset
} Event;
bool event_parse(const Json *value, Event *out);
void event_free(Event *event);
/// Tool events carry their detail in `summary`; other kinds use `text`.
const char *event_detail(const Event *event);
/// Status and workspace setup output are dashboard plumbing, not part of the conversation.
bool event_visible(const Event *event);
/// When the server logged it; false without a readable time.
bool event_time(const Event *event, time_t *out);

// Cursor and transcript have one lifetime: saved events restore both, and an empty transcript starts at zero.
typedef struct { Event *events; size_t count, cap; int cursor; } Transcript;
void transcript_init(Transcript *t);
void transcript_free(Transcript *t);
/// Appends every event of a JSON array not already held, sorted by sequence, and advances the cursor.
void transcript_append(Transcript *t, const Json *events);
Json *transcript_json(const Transcript *t);

// MARK: - Runtimes

typedef struct { char *id, *label; char **efforts; size_t effort_count; char *default_effort; } RuntimeModel;
typedef struct { int id; char *label; int available; RuntimeModel *models; size_t model_count; char *default_model; } RuntimeProvider;
/// What a start is asked to run on (`provider`, `model`, `effort`). The server may still fall back to a default model or effort.
typedef struct { int provider_id; char *model; char *effort; } RuntimeChoice;
typedef struct { bool has_default; RuntimeChoice def; RuntimeProvider *providers; size_t provider_count; } RuntimeCatalog;

bool runtime_catalog_parse(const Json *value, RuntimeCatalog *out);
Json *runtime_catalog_json(const RuntimeCatalog *catalog);
void runtime_catalog_free(RuntimeCatalog *catalog);
void runtime_choice_free(RuntimeChoice *choice);
void runtime_choice_copy(RuntimeChoice *into, const RuntimeChoice *from);
bool runtime_choice_equal(const RuntimeChoice *a, const RuntimeChoice *b);
Json *runtime_choice_arguments(const RuntimeChoice *choice);
const char *runtime_model_title(const RuntimeModel *model);
/// Only an explicit false greys a provider out; older servers omit the field.
bool runtime_provider_available(const RuntimeProvider *provider);
const RuntimeProvider *runtime_catalog_provider(const RuntimeCatalog *catalog, int id);
const RuntimeModel *runtime_catalog_model(const RuntimeCatalog *catalog, const RuntimeChoice *choice);
/// The efforts the chosen model offers; `*count` is 0 without any.
const char *const *runtime_catalog_efforts(const RuntimeCatalog *catalog, const RuntimeChoice *choice, size_t *count);
/// A provider's model with that model's own default effort, since efforts differ between models. False for an unknown provider.
bool runtime_catalog_choice(const RuntimeCatalog *catalog, int provider_id, const char *model, RuntimeChoice *out);
/// Used when the project has no default runtime and a start therefore needs a provider.
bool runtime_catalog_first_available(const RuntimeCatalog *catalog, RuntimeChoice *out);
/// "Provider · Model", as a new string.
char *runtime_catalog_label(const RuntimeCatalog *catalog, const RuntimeChoice *choice);

// MARK: - Pull request files

typedef struct {
    char *filename, *previous_filename, *status, *patch, *url;
    int additions, deletions;   // -1 when the server sent none
} PullFile;
bool pull_file_parse(const Json *value, PullFile *out);
Json *pull_file_json(const PullFile *file);
void pull_file_free(PullFile *file);
void pull_file_copy(PullFile *into, const PullFile *from);
/// The last path component.
const char *pull_file_name(const PullFile *file);
/// The path without its last component, as a new string; "" at the root.
char *pull_file_directory(const PullFile *file);

typedef struct { Json *pr; PullFile *files; size_t file_count; int next_page; bool truncated; } PullFilesPage;   // next_page 0 = none
bool pull_files_page_parse(const Json *value, PullFilesPage *out);
void pull_files_page_free(PullFilesPage *page);

/// Pages of one pull request revision. Later pages are pinned to page 1's commits.
typedef struct { Json *pr; PullFile *files; size_t file_count; int next_page; bool truncated; } PullFileList;
void pull_file_list_init(PullFileList *list);
void pull_file_list_free(PullFileList *list);
bool pull_file_list_parse(const Json *value, PullFileList *out);
Json *pull_file_list_json(const PullFileList *list);
/// The arguments for the next page, or NULL once every page was read.
Json *pull_file_list_arguments(const PullFileList *list, const char *repo, int number);
void pull_file_list_append(PullFileList *list, const PullFilesPage *page);
/// A saved list of the revision this first page belongs to keeps its files and takes the page's details.
bool pull_file_list_confirm(PullFileList *list, const PullFilesPage *page);

/// GitHub's timestamps come with and without fractional seconds. UTC.
bool board_date_parse(const char *value, time_t *out);

#endif
