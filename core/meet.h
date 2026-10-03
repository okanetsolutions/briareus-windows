// The meeting assistant's words with OpenAI's Realtime API: the events a GPT-Realtime 2.1 mini session sends and takes,
// with the project's read-only tools (core/meet_tools.c), and the meeting's running transcript. With an ElevenLabs
// voice, Realtime answers in text and ElevenLabs' Text to Dialogue WebSocket says it in that voice.
// No sockets and no audio here: app/meeting.c carries the events, app/meet_audio.c the sound.
#ifndef BRIAREUS_MEET_H
#define BRIAREUS_MEET_H
#include "json.h"
#include "str.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Meetings are joined with GPT-Realtime; GPT-Live 1 is kept only to read and price the meetings recorded with it.
typedef enum { MEET_LIVE, MEET_REALTIME, MEET_MODEL_COUNT } MeetModel;
#define MEET_MODEL "gpt-realtime-2.1-mini"
/// Both directions are mono 16-bit PCM at this rate.
#define MEET_SAMPLE_RATE 24000

const char *meet_model_id(MeetModel model);
const char *meet_model_label(MeetModel model);
/// The WebSocket's path on api.openai.com, with its query.
const char *meet_ws_path(MeetModel model);

/// Who the assistant speaks for and how, in a meeting about `project` (its repository). An `independent` assistant takes
/// part on its own: it speaks when it judges it useful. Otherwise it speaks only when addressed: when a turn says one
/// of the comma-separated `wake_words`. With an `eleven_voice` (an ElevenLabs voice ID) it answers in text for ElevenLabs
/// to say; otherwise it speaks with the OpenAI `voice`.
typedef struct { const char *name, *voice, *wake_words, *project; bool introduce, independent; const char *eleven_voice; } MeetPersona;
/// Whether the model answers in text, for ElevenLabs to say.
bool meet_text_out(const MeetPersona *p);

char *meet_instructions(const MeetPersona *p);
/// The first event: `session.update`, with the project's tools.
char *meet_setup_event(const MeetPersona *p);
/// What the assistant says as it joins, when `introduce` is set; NULL otherwise.
char *meet_greeting_event(const MeetPersona *p);
char *meet_audio_event(const int16_t *pcm, size_t samples);
/// Asks for an answer to what was just said, when the assistant was not addressed by a wake word.
char *meet_answer_now_event(void);
/// A tool's `output` back to the model, then `response.create` so it says it. Fills `out`; returns 2.
size_t meet_tool_output_events(const char *call_id, const char *output, char *out[2]);

/// What a session used, as OpenAI reports it: GPT-Live's voice seconds, Realtime's tokens by kind, and the seconds its
/// input transcription heard.
typedef struct {
    double seconds;
    double text_in, text_cached, text_out, audio_in, audio_cached, audio_out;
    double transcribed_seconds;
    double spoken_chars;     // what ElevenLabs said, in characters
} MeetUsage;
/// In US dollars, at the rates the model pages list (2026-10-03), ElevenLabs' at Eleven v4 Turbo's list price.
double meet_usage_cost(MeetModel model, const MeetUsage *usage);
/// Takes a reported usage in: GPT-Live's seconds are a running total, Realtime's tokens add up.
void meet_usage_merge(MeetUsage *into, const MeetUsage *reported, bool snapshot);

typedef enum {
    MEET_EV_OTHER,
    MEET_EV_READY,           // the session took its setup
    MEET_EV_AUDIO,           // speech to play: `audio`
    MEET_EV_HEARD,           // part of what the meeting said: `text`
    MEET_EV_HEARD_TURN,      // a whole turn the meeting said (Realtime): `text`
    MEET_EV_SAID,            // part of what the assistant said: `text`
    MEET_EV_SAID_DONE,       // the assistant's text answer is whole (Realtime answering in text)
    MEET_EV_RESPONSE,        // Realtime started an answer
    MEET_EV_SPEECH_STARTED,  // someone started talking: unplayed speech is dropped
    MEET_EV_TOOL,            // the model calls a tool: `id` (its call id), `text` (its name), `request` (its arguments, JSON)
    MEET_EV_ERROR,           // `text`
    MEET_EV_CLOSED,
} MeetEventKind;
/// Any event may also report usage: `has_usage`, a running total when `usage_snapshot`.
typedef struct {
    MeetEventKind kind; unsigned char *audio; size_t audio_len; char *text, *id, *request;
    bool has_usage, usage_snapshot; MeetUsage usage;
} MeetEvent;
/// Reads one server event. False for text that is not a JSON event.
bool meet_event_parse(const char *json, size_t len, MeetEvent *out);
void meet_event_free(MeetEvent *e);

char *base64_encode(const void *data, size_t len);

// MARK: ElevenLabs

/// Eleven v4 Turbo: ElevenLabs' most natural model for real time, about 150 ms to first speech.
#define ELEVEN_MODEL "eleven_v4_turbo"
#define ELEVEN_HOST "api.elevenlabs.io"
/// US dollars per character said, Eleven v4 Turbo's list price ($0.04 per 1,000).
#define ELEVEN_CHAR_COST 0.00004
/// The Text to Dialogue WebSocket's path, PCM at MEET_SAMPLE_RATE.
const char *eleven_ws_path(void);
/// The first message: the one voice the session speaks with.
char *eleven_open_event(const char *voice);
/// Text to say; `new_turn` starts a new answer.
char *eleven_text_event(const char *voice, const char *text, bool new_turn);
/// Says what is buffered now: the answer is whole.
char *eleven_flush_event(void);
/// Keeps the socket open through silence (it closes after 20 s without a message).
char *eleven_keep_alive_event(void);
/// Reads one server message: MEET_EV_AUDIO, MEET_EV_ERROR, or MEET_EV_OTHER. False for text that is not JSON.
bool eleven_event_parse(const char *json, size_t len, MeetEvent *out);
/// Characters in UTF-8 text, as ElevenLabs bills them.
size_t meet_char_count(const char *text);

/// The meeting so far as lines of "Meeting: …" and "Assistant: …", the oldest dropped past a limit.
typedef struct { Str text; int speaker; } MeetLog;
enum { MEET_SPEAKER_MEETING = 1, MEET_SPEAKER_ASSISTANT = 2 };
void meet_log_init(MeetLog *log);
void meet_log_free(MeetLog *log);
/// Adds a piece of speech; a new speaker starts a new line.
void meet_log_add(MeetLog *log, int speaker, const char *text);
/// The last `max` bytes or fewer, starting at a line. New string.
char *meet_log_tail(const MeetLog *log, size_t max);

/// Whether a heard turn should get a `response.create` from the app: Realtime's turn detection answers on its own for an
/// independent assistant, so only an addressed one is asked, and only when the turn says a wake word.
bool meet_should_answer(const MeetPersona *p, const char *heard);
/// Whether a heard turn says one of the comma-separated wake words, as a whole word, ignoring case.
bool meet_wake_word(const char *heard, const char *wake_words);
/// Text made fit to be said: no Markdown or code, whitespace collapsed, cut at a sentence within `max` bytes.
char *meet_spoken(const char *text, size_t max);

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
