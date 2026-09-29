// What the mobile API answers with, read into the shapes the screens use. Unknown fields ride along in `raw`.
#ifndef BRIAREUS_MODELS_H
#define BRIAREUS_MODELS_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// MARK: - Connection

typedef struct {
    char *id, *label, *permission;
    char **repos; size_t repo_count;
    double expires_at;   // milliseconds since 1970
} Device;
bool device_parse(const Json *value, Device *out);
Json *device_json(const Device *device);
void device_free(Device *device);
void device_copy(Device *into, const Device *from);
bool device_can_manage(const Device *device);
time_t device_expiry(const Device *device);

/// -1 when the server left transcription out (a server from before voice notes), else 0 or 1.
typedef struct { int version; Device device; int transcribe; } Discovery;
bool discovery_parse(const Json *value, Discovery *out);
void discovery_free(Discovery *discovery);
/// What the server's owner has to do before voice notes work, or NULL when they do.
const char *discovery_voice_notes_off(int transcribe);

typedef struct { char *name; bool read_only; } Operation;
bool operations_parse(const Json *value, Operation **out, size_t *count);
Json *operations_json(const Operation *ops, size_t count);
void operations_free(Operation *ops, size_t count);
Operation *operations_copy(const Operation *ops, size_t count);

/// What pairing learned about this device, saved so the next launch opens without asking again first.
typedef struct { Device device; Operation *operations; size_t operation_count; int transcribe; } Connection;
bool connection_parse(const Json *value, Connection *out);
Json *connection_json(const Connection *connection);
void connection_free(Connection *connection);

// MARK: - Projects and sessions

typedef struct { char *repo; char *label; } Project;
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
/// from the number otherwise (a conversation that moved on to another pull request still holds this round). New string.
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
/// What `start_session` is asked to run on. The server may still fall back to a default model or effort.
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
/// Used when the project has no default runtime and `start_session` therefore needs a provider.
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
