#include "meet.h"
#include "meet_tools.h"
#include "browser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *id, *label, *path; } MODELS[MEET_MODEL_COUNT] = {
    [MEET_LIVE] = { "gpt-live-1", "GPT-Live 1", "/v1/live/sessions" },
    [MEET_REALTIME] = { "gpt-realtime-2.1-mini", "GPT-Realtime 2.1 mini", "/v1/realtime?model=gpt-realtime-2.1-mini" },
};
const char *meet_model_id(MeetModel model) { return MODELS[model].id; }
const char *meet_model_label(MeetModel model) { return MODELS[model].label; }
const char *meet_ws_path(MeetModel model) { return MODELS[model].path; }

static const char *name_of(const MeetPersona *p) { return p->name && *p->name ? p->name : "the user"; }
static const char *voice_of(const MeetPersona *p) { return p->voice && *p->voice ? p->voice : "marin"; }
bool meet_text_out(const MeetPersona *p) { return p->eleven_voice && *p->eleven_voice; }

// MARK: - Setup

char *meet_instructions(const MeetPersona *p) {
    const char *name = name_of(p), *project = p->project && *p->project ? p->project : "this project";
    bool own_voice = meet_text_out(p);
    Str s; str_init(&s);
    if (own_voice)
        str_appendf(&s, "You speak for %s in a live meeting, in %s's own voice, through their microphone. Speak as %s, in the "
                        "first person. You hear everyone else in the meeting. ", name, name, name);
    else
        str_appendf(&s, "You are %s's AI voice assistant, taking part in a live meeting for them through their microphone. "
                        "You hear everyone else in the meeting. ", name);
    if (p->independent)
        str_appendf(&s, "Act independently on %s's behalf: join the discussion when you have something useful to add, answer "
                        "questions put to %s and give updates as %s would. Do not talk over people, and keep quiet when the "
                        "discussion does not need you. ", name, name, name);
    else
        str_appendf(&s, "Speak only when someone addresses you or %s with a question or request you can help with; otherwise "
                        "stay silent and keep listening. ", name);
    str_appendz(&s, "Keep answers short and conversational. If someone sincerely asks whether they are talking to an AI, do not deny it. ");
    if (own_voice)
        str_appendz(&s, "Your text is read aloud as written: write plain spoken English, with no Markdown, lists, emoji or stage "
                        "directions. ");
    str_appendf(&s, "The meeting is about the project %s: use the tools for anything about its conversations, agents, pull "
                    "requests, issues or findings, say briefly that you are checking, and say only what they confirm. Never "
                    "invent project details.\n\n", project);
    char *tools = meet_tools_instructions(project);
    str_appendz(&s, tools);
    free(tools);
    return str_detach(&s);
}

static Json *pcm_format(void) {
    Json *f = json_object(); json_set_str(f, "type", "audio/pcm"); json_set_num(f, "rate", MEET_SAMPLE_RATE);
    return f;
}
static char *serialize_free(Json *event) { char *text = json_serialize(event, false); json_free(event); return text; }

char *meet_setup_event(const MeetPersona *p) {
    Json *event = json_object(), *session = json_object(), *audio = json_object();
    char *instructions = meet_instructions(p);
    json_set_str(event, "type", "session.update");
    json_set_str(session, "type", "realtime");
    json_set_str(session, "model", MEET_MODEL);
    // With an ElevenLabs voice the answer is text, which ElevenLabs says.
    bool text_out = meet_text_out(p);
    Json *modalities = json_array(); json_array_push(modalities, json_string(text_out ? "text" : "audio"));
    json_object_set(session, "output_modalities", modalities);
    json_set_str(session, "instructions", instructions);
    Json *input = json_object(), *transcription = json_object(), *turns = json_object();
    json_object_set(input, "format", pcm_format());
    json_set_str(transcription, "model", "gpt-transcribe");
    json_object_set(input, "transcription", transcription);
    // An addressed assistant answers only when the app asks, on a wake word; an independent one whenever it judges.
    json_set_str(turns, "type", p->independent ? "semantic_vad" : "server_vad");
    json_set_bool(turns, "create_response", p->independent);
    json_set_bool(turns, "interrupt_response", true);
    json_object_set(input, "turn_detection", turns);
    json_object_set(audio, "input", input);
    if (!text_out) {
        Json *output = json_object();
        json_set_str(output, "voice", voice_of(p));
        json_object_set(output, "format", pcm_format());
        json_object_set(audio, "output", output);
    }
    json_object_set(session, "audio", audio);
    json_object_set(session, "tools", meet_tools_json());
    json_set_str(session, "tool_choice", "auto");
    free(instructions);
    json_object_set(event, "session", session);
    return serialize_free(event);
}

char *meet_greeting_event(const MeetPersona *p) {
    if (!p->introduce) return NULL;
    char *words = xstrfmt("Introduce yourself now in one short sentence: you are %s's AI assistant, joining the meeting for them, "
                          "and people can ask you about the project. Then listen.", name_of(p));
    Json *event = json_object(), *response = json_object();
    json_set_str(response, "instructions", words);
    json_set_str(event, "type", "response.create");
    json_object_set(event, "response", response);
    free(words);
    return serialize_free(event);
}

// MARK: - Audio

char *base64_encode(const void *data, size_t len) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *b = data;
    char *out = xmalloc((len + 2) / 3 * 4 + 1), *o = out;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (unsigned)b[i] << 16 | (unsigned)b[i + 1] << 8 | b[i + 2];
        *o++ = digits[v >> 18]; *o++ = digits[v >> 12 & 63]; *o++ = digits[v >> 6 & 63]; *o++ = digits[v & 63];
    }
    if (i < len) {
        unsigned v = (unsigned)b[i] << 16 | (i + 1 < len ? (unsigned)b[i + 1] << 8 : 0);
        *o++ = digits[v >> 18]; *o++ = digits[v >> 12 & 63];
        *o++ = i + 1 < len ? digits[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    return out;
}

char *meet_audio_event(const int16_t *pcm, size_t samples) {
    // Little-endian samples, as Windows holds them.
    char *audio = base64_encode(pcm, samples * sizeof *pcm);
    char *event = xstrfmt("{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", audio);
    free(audio);
    return event;
}

char *meet_answer_now_event(void) { return xstrdup("{\"type\":\"response.create\"}"); }

size_t meet_tool_output_events(const char *call_id, const char *output, char *out[2]) {
    Json *event = json_object(), *item = json_object();
    json_set_str(item, "type", "function_call_output");
    json_set_str(item, "call_id", call_id);
    json_set_str(item, "output", output);
    json_set_str(event, "type", "conversation.item.create");
    json_object_set(event, "item", item);
    out[0] = serialize_free(event);
    out[1] = meet_answer_now_event();
    return 2;
}

// MARK: - Usage and cost

double meet_usage_cost(MeetModel model, const MeetUsage *u) {
    double spoken = u->spoken_chars * ELEVEN_CHAR_COST;
    if (model == MEET_LIVE) return u->seconds / 60 * 0.05 + spoken;
    double tokens = (u->text_in * 0.6 + u->text_cached * 0.06 + u->text_out * 2.4 + u->audio_in * 10 + u->audio_cached * 0.3 + u->audio_out * 20) / 1e6;
    return tokens + u->transcribed_seconds / 60 * 0.0045 + spoken;
}
void meet_usage_merge(MeetUsage *into, const MeetUsage *reported, bool snapshot) {
    if (snapshot) { if (reported->seconds > into->seconds) into->seconds = reported->seconds; return; }
    into->seconds += reported->seconds;
    into->text_in += reported->text_in; into->text_cached += reported->text_cached; into->text_out += reported->text_out;
    into->audio_in += reported->audio_in; into->audio_cached += reported->audio_cached; into->audio_out += reported->audio_out;
    into->transcribed_seconds += reported->transcribed_seconds;
    into->spoken_chars += reported->spoken_chars;
}

static void read_usage(const char *type, const Json *e, MeetEvent *out) {
    MeetUsage *u = &out->usage;
    if (str_eq(type, "response.done")) {
        const Json *usage = json_get(json_get(e, "response"), "usage");
        if (!json_is_object(usage)) return;
        const Json *in = json_get(usage, "input_token_details"), *cached = json_get(in, "cached_tokens_details"), *outd = json_get(usage, "output_token_details");
        double cached_text = json_num_or(json_get(cached, "text_tokens"), 0), cached_audio = json_num_or(json_get(cached, "audio_tokens"), 0);
        u->text_in = json_num_or(json_get(in, "text_tokens"), 0) - cached_text;
        u->audio_in = json_num_or(json_get(in, "audio_tokens"), 0) - cached_audio;
        if (u->text_in < 0) u->text_in = 0;
        if (u->audio_in < 0) u->audio_in = 0;
        u->text_cached = cached_text; u->audio_cached = cached_audio;
        u->text_out = json_num_or(json_get(outd, "text_tokens"), 0);
        u->audio_out = json_num_or(json_get(outd, "audio_tokens"), 0);
        out->has_usage = true;
    } else if (str_eq(type, "conversation.item.input_audio_transcription.completed")) {
        // gpt-transcribe is billed by the minute heard.
        const Json *usage = json_get(e, "usage");
        double seconds;
        if (str_eq(json_str(json_get(usage, "type")), "duration") && json_num(json_get(usage, "seconds"), &seconds)) {
            u->transcribed_seconds = seconds; out->has_usage = true;
        }
    }
}

// MARK: - Server events

static void take_text(MeetEvent *out, MeetEventKind kind, const Json *value) {
    out->kind = kind;
    out->text = json_dup_str(value);
    if (!out->text) out->text = xstrdup("");
}

bool meet_event_parse(const char *json, size_t len, MeetEvent *out) {
    memset(out, 0, sizeof *out);
    Json *e = json_parse(json, len);
    const char *type = json_str(json_get(e, "type"));
    if (!type) { json_free(e); return false; }
    out->kind = MEET_EV_OTHER;
    const char *audio = NULL;
    if (str_eq(type, "error")) {
        const Json *err = json_get(e, "error");
        const char *message = json_str_nonempty(json_get(err, "message"));
        out->kind = MEET_EV_ERROR;
        out->text = xstrdup(message ? message : json_str_nonempty(err) ? json_str(err) : "OpenAI reported an error.");
    }
    else if (str_eq(type, "session.updated")) out->kind = MEET_EV_READY;
    else if (str_eq(type, "response.output_audio.delta")) audio = json_str(json_get(e, "delta"));
    else if (str_eq(type, "conversation.item.input_audio_transcription.completed")) take_text(out, MEET_EV_HEARD_TURN, json_get(e, "transcript"));
    else if (str_eq(type, "response.output_audio_transcript.delta")) take_text(out, MEET_EV_SAID, json_get(e, "delta"));
    else if (str_eq(type, "response.output_text.delta")) take_text(out, MEET_EV_SAID, json_get(e, "delta"));
    // An answer in audio has its transcript done too, which ElevenLabs says should OpenAI answer in audio after all.
    else if (str_eq(type, "response.output_text.done") || str_eq(type, "response.output_audio_transcript.done")) out->kind = MEET_EV_SAID_DONE;
    else if (str_eq(type, "response.created")) out->kind = MEET_EV_RESPONSE;
    else if (str_eq(type, "input_audio_buffer.speech_started")) out->kind = MEET_EV_SPEECH_STARTED;
    else if (str_eq(type, "response.output_item.done")) {
        const Json *item = json_get(e, "item");
        const char *call = json_str_nonempty(json_get(item, "call_id")), *name = json_str_nonempty(json_get(item, "name"));
        if (str_eq(json_str(json_get(item, "type")), "function_call") && call && name) {
            out->kind = MEET_EV_TOOL;
            out->id = xstrdup(call);
            out->text = xstrdup(name);
            out->request = xstrdup(json_str_or(json_get(item, "arguments"), "{}"));
        }
    }
    if (audio) {
        out->audio = base64_decode(audio, strlen(audio), &out->audio_len);
        out->kind = out->audio ? MEET_EV_AUDIO : MEET_EV_OTHER;
    }
    read_usage(type, e, out);
    json_free(e);
    return true;
}

// MARK: - ElevenLabs

const char *eleven_ws_path(void) { return "/v1/text-to-dialogue/stream-input?model_id=" ELEVEN_MODEL "&output_format=pcm_24000"; }

char *eleven_open_event(const char *voice) {
    Json *event = json_object(), *voices = json_array();
    json_array_push(voices, json_string(voice));
    json_object_set(event, "voices", voices);
    return serialize_free(event);
}
char *eleven_text_event(const char *voice, const char *text, bool new_turn) {
    Json *event = json_object(), *inputs = json_array(), *input = json_object();
    json_set_str(input, "text", text);
    json_set_str(input, "voice_id", voice);
    if (new_turn) json_set_bool(input, "new_turn", true);
    json_array_push(inputs, input);
    json_object_set(event, "inputs", inputs);
    return serialize_free(event);
}
char *eleven_flush_event(void) { return xstrdup("{\"flush\":true}"); }
char *eleven_keep_alive_event(void) { return xstrdup("{\"keep_alive\":true}"); }

bool eleven_event_parse(const char *json, size_t len, MeetEvent *out) {
    memset(out, 0, sizeof *out);
    Json *e = json_parse(json, len);
    if (!json_is_object(e)) { json_free(e); return false; }
    out->kind = MEET_EV_OTHER;
    const char *audio = json_str_nonempty(json_get(e, "audio"));
    const char *message = json_str_nonempty(json_get(e, "message"));
    if (audio) {
        out->audio = base64_decode(audio, strlen(audio), &out->audio_len);
        if (out->audio) out->kind = MEET_EV_AUDIO;
    } else if (message || json_str_nonempty(json_get(e, "error"))) {
        out->kind = MEET_EV_ERROR;
        out->text = xstrfmt("ElevenLabs: %s", message ? message : json_str(json_get(e, "error")));
    }
    json_free(e);
    return true;
}

size_t meet_char_count(const char *text) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)(text ? text : ""); *c; c++) if ((*c & 0xC0) != 0x80) n++;
    return n;
}

void meet_event_free(MeetEvent *e) { free(e->audio); free(e->text); free(e->id); free(e->request); memset(e, 0, sizeof *e); }

// MARK: - The meeting's transcript

#define MEET_LOG_LIMIT 24000
void meet_log_init(MeetLog *log) { str_init(&log->text); log->speaker = 0; }
void meet_log_free(MeetLog *log) { str_free(&log->text); log->speaker = 0; }
void meet_log_add(MeetLog *log, int speaker, const char *text) {
    if (!text || !*text) return;
    if (speaker != log->speaker) {
        if (log->text.len) str_appendc(&log->text, '\n');
        str_appendz(&log->text, speaker == MEET_SPEAKER_ASSISTANT ? "Assistant: " : "Meeting: ");
        log->speaker = speaker;
        while (*text == ' ') text++;
    }
    str_appendz(&log->text, text);
    if (log->text.len > MEET_LOG_LIMIT) {
        // The oldest lines go, whole, once the record is well past the limit.
        const char *cut = strchr(log->text.data + log->text.len - MEET_LOG_LIMIT / 2, '\n');
        if (cut) {
            size_t drop = (size_t)(cut + 1 - log->text.data);
            memmove(log->text.data, cut + 1, log->text.len - drop + 1);
            log->text.len -= drop;
        }
    }
}
char *meet_log_tail(const MeetLog *log, size_t max) {
    if (!log->text.len) return xstrdup("");
    if (log->text.len <= max) return xstrdup(log->text.data);
    const char *from = log->text.data + log->text.len - max;
    const char *line = strchr(from, '\n');
    return xstrdup(line ? line + 1 : from);
}

// MARK: - Asking the agent

static bool word_char(unsigned char c) { return isalnum(c) || c >= 0x80; }
bool meet_wake_word(const char *heard, const char *wake_words) {
    if (!heard || !wake_words) return false;
    char *text = str_fold(heard);
    size_t count = 0;
    char **words = str_split(wake_words, ',', &count);
    bool found = false;
    for (size_t i = 0; i < count && !found; i++) {
        char *trimmed = str_trim(words[i]), *word = str_fold(trimmed);
        size_t n = strlen(word);
        for (const char *at = n ? strstr(text, word) : NULL; at && !found; at = strstr(at + 1, word))
            found = (at == text || !word_char((unsigned char)at[-1])) && !word_char((unsigned char)at[n]);
        free(trimmed); free(word);
    }
    str_array_free(words, count);
    free(text);
    return found;
}
bool meet_should_answer(const MeetPersona *p, const char *heard) { return !p->independent && meet_wake_word(heard, p->wake_words); }

char *meet_spoken(const char *text, size_t max) {
    Str s; str_init(&s);
    bool fence = false, space = false;
    for (const char *line = text ? text : ""; *line; ) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        while (p < line + n && (*p == ' ' || *p == '\t')) p++;
        if (!strncmp(p, "```", 3)) fence = !fence;
        else if (!fence) {
            // A heading's hashes, a quote's mark and a list's bullet are not said.
            while (p < line + n && (*p == '#' || *p == '>')) p++;
            if (p + 1 < line + n && (*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') p += 2;
            for (; p < line + n; p++) {
                char c = *p;
                if (c == '*' || c == '`' || c == '_' || c == '|') continue;
                if (c == ' ' || c == '\t' || c == '\r') { space = s.len > 0; continue; }
                if (space) { str_appendc(&s, ' '); space = false; }
                str_appendc(&s, c);
            }
            space = s.len > 0;
        }
        line = end ? end + 1 : line + n;
    }
    char *all = str_detach(&s);
    size_t len = strlen(all);
    if (len <= max) return all;
    // Cut after the last sentence that fits, else at the last space, never inside a UTF-8 sequence.
    size_t cut = 0;
    for (size_t i = 0; i < max; i++) if ((all[i] == '.' || all[i] == '!' || all[i] == '?') && (all[i + 1] == ' ' || !all[i + 1])) cut = i + 1;
    if (!cut) { for (size_t i = 0; i < max; i++) if (all[i] == ' ') cut = i; }
    if (!cut) { cut = max; while (cut && ((unsigned char)all[cut] & 0xC0) == 0x80) cut--; }
    all[cut] = 0;
    return all;
}

// MARK: - Meeting records

static const char *const USAGE_KEYS[] = { "seconds", "textIn", "textCached", "textOut", "audioIn", "audioCached", "audioOut", "transcribedSeconds", "spokenChars" };
static double *usage_field(MeetUsage *u, size_t i) {
    double *fields[] = { &u->seconds, &u->text_in, &u->text_cached, &u->text_out, &u->audio_in, &u->audio_cached, &u->audio_out, &u->transcribed_seconds, &u->spoken_chars };
    return fields[i];
}
Json *meet_record_json(const MeetRecord *r) {
    Json *o = json_object(), *usage = json_object();
    MeetUsage u = r->usage;
    json_set_str(o, "model", meet_model_id(r->model));
    json_set_num(o, "started", r->started);
    json_set_num(o, "seconds", r->seconds);
    for (size_t i = 0; i < sizeof USAGE_KEYS / sizeof *USAGE_KEYS; i++) json_set_num(usage, USAGE_KEYS[i], *usage_field(&u, i));
    json_object_set(o, "usage", usage);
    // The cost as it was charged then, so a later change of rates does not rewrite history.
    json_set_num(o, "voiceCost", meet_usage_cost(r->model, &r->usage));
    json_set_num(o, "agentCost", r->agent_cost);
    json_set_num(o, "requests", r->requests);
    json_set_num(o, "answers", r->answers);
    json_set_num(o, "answerSeconds", r->answer_seconds);
    return o;
}
bool meet_record_parse(const Json *value, MeetRecord *out) {
    memset(out, 0, sizeof *out);
    const char *model = json_str(json_get(value, "model"));
    int m = -1;
    for (int i = 0; i < MEET_MODEL_COUNT; i++) if (str_eq(model, meet_model_id((MeetModel)i))) m = i;
    if (m < 0) return false;
    out->model = (MeetModel)m;
    out->started = json_num_or(json_get(value, "started"), 0);
    out->seconds = json_num_or(json_get(value, "seconds"), 0);
    const Json *usage = json_get(value, "usage");
    for (size_t i = 0; i < sizeof USAGE_KEYS / sizeof *USAGE_KEYS; i++) *usage_field(&out->usage, i) = json_num_or(json_get(usage, USAGE_KEYS[i]), 0);
    out->agent_cost = json_num_or(json_get(value, "agentCost"), 0);
    out->requests = json_int_or(json_get(value, "requests"), 0);
    out->answers = json_int_or(json_get(value, "answers"), 0);
    out->answer_seconds = json_num_or(json_get(value, "answerSeconds"), 0);
    return true;
}
void meet_totals(const Json *records, MeetTotals out[MEET_MODEL_COUNT]) {
    memset(out, 0, sizeof *out * MEET_MODEL_COUNT);
    for (size_t i = 0; i < json_count(records); i++) {
        MeetRecord r;
        if (!meet_record_parse(json_at(records, i), &r)) continue;
        MeetTotals *t = &out[r.model];
        double charged;
        t->meetings++;
        t->seconds += r.seconds;
        t->voice_cost += json_num(json_get(json_at(records, i), "voiceCost"), &charged) ? charged : meet_usage_cost(r.model, &r.usage);
        t->agent_cost += r.agent_cost;
        t->requests += r.requests; t->answers += r.answers; t->answer_seconds += r.answer_seconds;
    }
}
