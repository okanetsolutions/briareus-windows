// The meeting assistant's words with an ElevenLabs agent: the agent's settings (the user's voice, the prompt, the
// project's read-only tools from core/meet_tools.c), the events its conversation WebSocket sends and takes, the
// meeting's running transcript, and the record kept of each meeting.
// No sockets and no audio here: app/meeting.c carries the events, app/meet_audio.c the sound.
#ifndef BRIAREUS_MEET_H
#define BRIAREUS_MEET_H
#include "json.h"
#include "str.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Meetings are joined with an ElevenLabs agent; GPT-Live 1 and GPT-Realtime 2.1 mini are kept only to read and price
/// the meetings recorded with them.
typedef enum { MEET_LIVE, MEET_REALTIME, MEET_AGENT, MEET_MODEL_COUNT } MeetModel;
const char *meet_model_id(MeetModel model);
const char *meet_model_label(MeetModel model);

/// Both directions are mono 16-bit PCM at this rate.
#define MEET_SAMPLE_RATE 24000
#define ELEVEN_HOST "api.elevenlabs.io"
/// The agent the app keeps in the user's ElevenLabs workspace, brought up to date as each meeting starts.
#define MEET_AGENT_NAME "Briareus meeting assistant"
/// Fast, and good at choosing tools.
#define MEET_AGENT_LLM "claude-haiku-4-5"
/// ElevenLabs' most natural model for real time.
#define MEET_TTS_MODEL "eleven_v4_turbo"
/// US dollars per minute of an agent's call, ElevenAgents' rate on every plan (2026-10); the LLM is billed on top.
#define MEET_AGENT_MINUTE_COST 0.08
/// A meeting runs at most this long before ElevenLabs ends it: the most it allows.
#define MEET_MAX_SECONDS (2 * 60 * 60)

/// Who the assistant speaks for and how, in a meeting about `project`, with ElevenLabs voice `voice`. An `independent`
/// assistant takes part on its own: it speaks when it judges it useful. Otherwise it speaks only when addressed: when a
/// turn says one of the comma-separated `wake_words`, and stays silent otherwise.
typedef struct { const char *name, *wake_words, *project, *voice; bool introduce, independent; } MeetPersona;

char *meet_instructions(const MeetPersona *p);
/// What the assistant says as it joins, when `introduce` is set; "" otherwise.
char *meet_first_message(const MeetPersona *p);
/// The agent's settings, the body that creates it (POST /v1/convai/agents/create) and brings it up to date (PATCH
/// /v1/convai/agents/{id}): its name, prompt, LLM, the project's tools by id, skip_turn, the voice and PCM both ways.
char *meet_agent_body(const MeetPersona *p, char *const *tool_ids, size_t tool_count);
/// The conversation WebSocket's path on api.elevenlabs.io for the agent, opened with the API key in a header.
char *meet_agent_ws_path(const char *agent_id);
/// The path, with its signature, of the `signed_url` ElevenLabs answered for a private agent's conversation; NULL when it
/// is not a wss URL on ELEVEN_HOST. New string.
char *meet_signed_ws_path(const Json *answer);

/// The first message on the socket.
char *meet_start_event(void);
char *meet_audio_event(const int16_t *pcm, size_t samples);
/// The answer to the server's ping `event_id`.
char *meet_pong_event(int event_id);
/// A tool's answer back to the agent.
char *meet_tool_result_event(const char *call_id, const char *result, bool is_error);
/// Asks for an answer to what was just said, whether or not the assistant was addressed.
char *meet_answer_now_event(void);

typedef enum {
    MEET_EV_OTHER,
    MEET_EV_READY,           // the conversation started
    MEET_EV_AUDIO,           // speech to play: `audio`
    MEET_EV_HEARD_TURN,      // a whole turn the meeting said: `text`
    MEET_EV_SAID,            // what the assistant said: `text`
    MEET_EV_INTERRUPTED,     // someone talked over the assistant: unplayed speech is dropped
    MEET_EV_TOOL,            // the agent calls a tool: `id` (its call id), `text` (its name), `request` (its parameters, JSON)
    MEET_EV_PING,            // to answer with a pong: `event_id`
    MEET_EV_ERROR,           // `text`
} MeetEventKind;
typedef struct { MeetEventKind kind; unsigned char *audio; size_t audio_len; char *text, *id, *request; int event_id; } MeetEvent;
/// Reads one server event. False for text that is not a JSON event.
bool meet_event_parse(const char *json, size_t len, MeetEvent *out);
void meet_event_free(MeetEvent *e);
/// The id in an answer that created an agent (`agent_id`) or a tool (`id`); NULL without one. New string.
char *meet_created_id(const Json *answer);

char *base64_encode(const void *data, size_t len);

/// The meeting so far as lines of "Meeting: …", "Assistant: …" and "Lookup: …", the oldest dropped past a limit.
typedef struct { Str text; int speaker; } MeetLog;
enum { MEET_SPEAKER_MEETING = 1, MEET_SPEAKER_ASSISTANT = 2, MEET_SPEAKER_LOOKUP = 3 };
void meet_log_init(MeetLog *log);
void meet_log_free(MeetLog *log);
/// Adds a piece of speech; a new speaker starts a new line.
void meet_log_add(MeetLog *log, int speaker, const char *text);
/// Adds a whole line: a turn, an answer or a lookup, on its own line even after the same speaker.
void meet_log_line(MeetLog *log, int speaker, const char *text);
/// The last `max` bytes or fewer, starting at a line. New string.
char *meet_log_tail(const MeetLog *log, size_t max);

/// Text made fit to be said: no Markdown or code, whitespace collapsed, cut at a sentence within `max` bytes.
char *meet_spoken(const char *text, size_t max);

/// What a meeting used: the agent's minutes, or for the meetings before it, GPT-Live's voice seconds, Realtime's tokens
/// by kind, the seconds its input transcription heard and the characters ElevenLabs said.
typedef struct {
    double seconds;
    double text_in, text_cached, text_out, audio_in, audio_cached, audio_out;
    double transcribed_seconds;
    double spoken_chars;
} MeetUsage;
/// In US dollars, at the listed rates (2026-10).
double meet_usage_cost(MeetModel model, const MeetUsage *usage);

/// One meeting, kept on this computer: how long it ran, what the voice cost, and how many project lookups it made and how
/// fast they came back. Meetings recorded before the tools held what the conversation's agent cost and answered.
typedef struct {
    MeetModel model;
    double started;          // Unix seconds
    double seconds;          // from joining to leaving
    MeetUsage usage;
    double agent_cost;       // the conversation agent's cost over the meeting, for meetings before the tools; 0 since
    int requests, answers;   // tool calls made, and answered
    double answer_seconds;   // their waits, added up
} MeetRecord;
Json *meet_record_json(const MeetRecord *r);
bool meet_record_parse(const Json *value, MeetRecord *out);

typedef struct { int meetings, requests, answers; double seconds, voice_cost, agent_cost, answer_seconds; } MeetTotals;
/// Adds the records (an array of meet_record_json objects) up by model.
void meet_totals(const Json *records, MeetTotals out[MEET_MODEL_COUNT]);

#endif
