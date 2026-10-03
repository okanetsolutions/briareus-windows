// The meeting assistant's tools: what the ElevenLabs agent may look up about the one project the meeting is about, as
// briareus-swift's voice does, read-only. Each tool is a few /api/v1 calls the app makes with its own token, the
// project's repo put in every one, and their answers cut down to a short JSON summary for the model to say.
// Nothing here changes anything on the server: in a meeting anyone can speak, so the assistant only reads.
#ifndef BRIAREUS_MEET_TOOLS_H
#define BRIAREUS_MEET_TOOLS_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MEET_TOOL_LIST_CONVERSATIONS,
    MEET_TOOL_READ_CONVERSATION,
    MEET_TOOL_LIST_PULL_REQUESTS,
    MEET_TOOL_READ_PULL_REQUEST,
    MEET_TOOL_LIST_FINDINGS,
    MEET_TOOL_LIST_ISSUES,
    MEET_TOOL_READ_ISSUE,
    MEET_TOOL_COUNT
} MeetTool;

const char *meet_tool_name(MeetTool tool);
bool meet_tool_find(const char *name, MeetTool *out);
/// The body that creates the tool in the ElevenLabs workspace (POST /v1/convai/tools) or brings it up to date (PATCH
/// /v1/convai/tools/{id}): a client tool, which the app runs and answers.
char *meet_tool_body(MeetTool tool);
/// What the model is told about the project and the tools.
char *meet_tools_instructions(const char *project);

/// One API call a tool makes: the client operation and its arguments.
#define MEET_TOOL_MAX_CALLS 3
typedef struct { const char *op; Json *args; } MeetCall;
/// The calls `tool` makes with the model's `args` on `repo`, in order, into `out`; 0 with `*refusal` (a JSON output for
/// the model) when the arguments are wrong.
size_t meet_tool_calls(MeetTool tool, const Json *args, const char *repo, MeetCall out[MEET_TOOL_MAX_CALLS], char **refusal);
void meet_calls_free(MeetCall *calls, size_t count);
/// The output for the model, as JSON text, from the calls' answers in order (NULL for one that failed: only the first
/// is needed). `repo` keeps out conversations of other projects.
char *meet_tool_summary(MeetTool tool, const Json *args, const char *repo, const Json *const *answers, size_t count);
/// The output when the calls failed: `why` in an error object.
char *meet_tool_error(const char *why);

#endif
