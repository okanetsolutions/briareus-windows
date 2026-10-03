#include "meet.h"
#include "meet_tools.h"
#include "browser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *id, *label; } MODELS[MEET_MODEL_COUNT] = {
    [MEET_LIVE] = { "gpt-live-1", "GPT-Live 1" },
    [MEET_REALTIME] = { "gpt-realtime-2.1-mini", "GPT-Realtime 2.1 mini" },
    [MEET_AGENT] = { "elevenlabs-agent", "ElevenLabs agent" },
};
const char *meet_model_id(MeetModel model) { return MODELS[model].id; }
const char *meet_model_label(MeetModel model) { return MODELS[model].label; }

static const char *name_of(const MeetPersona *p) { return p->name && *p->name ? p->name : "the user"; }
static char *serialize_free(Json *j) { char *text = json_serialize(j, false); json_free(j); return text; }

// MARK: - The agent

char *meet_instructions(const MeetPersona *p) {
    const char *name = name_of(p), *project = p->project && *p->project ? p->project : "this project";
    Str s; str_init(&s);
    str_appendf(&s, "You speak for %s in a live meeting, in %s's own voice, through their microphone. Speak as %s, in the "
                    "first person. You hear everyone else in the meeting. ", name, name, name);
    if (p->independent)
        str_appendf(&s, "Act independently on %s's behalf: join the discussion when you have something useful to add, answer "
                        "questions put to %s and give updates as %s would. Do not talk over people. When the discussion does "
                        "not need you, call skip_turn and stay silent. ", name, name, name);
    else {
        const char *words = p->wake_words && *p->wake_words ? p->wake_words : name;
        str_appendf(&s, "Speak only when someone addresses you by one of these names: %s, or asks you to answer. Everything "
                        "else said in the meeting is not for you: call skip_turn and stay silent, without a word. ", words);
    }
    str_appendz(&s, "Keep answers short and conversational, in plain spoken English. If someone sincerely asks whether they are "
                    "talking to an AI, do not deny it. ");
    str_appendf(&s, "The meeting is about the project %s: use the tools for anything about its conversations, agents, pull "
                    "requests, issues or findings, say briefly that you are checking, and say only what they confirm. Never "
                    "invent project details.\n\n", project);
    char *tools = meet_tools_instructions(project);
    str_appendz(&s, tools);
    free(tools);
    return str_detach(&s);
}

char *meet_first_message(const MeetPersona *p) {
    if (!p->introduce) return xstrdup("");
    return xstrfmt("Hi everyone, I'm %s's AI assistant, joining for them. Ask me anything about the project.", name_of(p));
}

char *meet_agent_body(const MeetPersona *p, char *const *tool_ids, size_t tool_count) {
    Json *body = json_object(), *config = json_object();
    json_set_str(body, "name", MEET_AGENT_NAME);
    // Turns: a meeting talks among itself, so the agent waits for a turn to end before it judges whether to speak.
    Json *asr = json_object(), *turn = json_object(), *tts = json_object(), *conversation = json_object();
    json_set_str(asr, "user_input_audio_format", "pcm_24000");
    json_set_str(turn, "turn_eagerness", "patient");
    json_set_str(tts, "model_id", MEET_TTS_MODEL);
    json_set_str(tts, "voice_id", p->voice ? p->voice : "");
    json_set_str(tts, "agent_output_audio_format", "pcm_24000");
    json_set_num(conversation, "max_duration_seconds", MEET_MAX_SECONDS);
    json_object_set(config, "asr", asr);
    json_object_set(config, "turn", turn);
    json_object_set(config, "tts", tts);
    json_object_set(config, "conversation", conversation);
    Json *agent = json_object(), *prompt = json_object(), *ids = json_array(), *builtin = json_object(), *skip = json_object(), *params = json_object();
    char *instructions = meet_instructions(p), *first = meet_first_message(p);
    json_set_str(agent, "first_message", first);
    json_set_str(agent, "language", "en");
    json_set_str(prompt, "prompt", instructions);
    json_set_str(prompt, "llm", MEET_AGENT_LLM);
    for (size_t i = 0; i < tool_count; i++) json_array_push(ids, json_string(tool_ids[i]));
    json_object_set(prompt, "tool_ids", ids);
    // skip_turn lets it stay silent through everything not for it.
    json_set_str(skip, "type", "system");
    json_set_str(skip, "name", "skip_turn");
    json_set_str(params, "system_tool_type", "skip_turn");
    json_object_set(skip, "params", params);
    json_object_set(builtin, "skip_turn", skip);
    json_object_set(prompt, "built_in_tools", builtin);
    json_object_set(agent, "prompt", prompt);
    json_object_set(config, "agent", agent);
    json_object_set(body, "conversation_config", config);
    free(instructions); free(first);
    return serialize_free(body);
}

char *meet_agent_ws_path(const char *agent_id) {
    // Agent ids are letters, digits and underscores; anything else is left out rather than escaped.
    Str s; str_init(&s);
    str_appendz(&s, "/v1/convai/conversation?agent_id=");
    for (const char *c = agent_id ? agent_id : ""; *c; c++) if (isalnum((unsigned char)*c) || *c == '_' || *c == '-') str_appendc(&s, *c);
    return str_detach(&s);
}

char *meet_signed_ws_path(const Json *answer) {
    static const char prefix[] = "wss://" ELEVEN_HOST "/";
    const char *url = json_str(json_get(answer, "signed_url"));
    if (!url || strncmp(url, prefix, sizeof prefix - 1)) return NULL;
    return xstrdup(url + sizeof prefix - 2);
}

char *meet_created_id(const Json *answer) {
    const char *id = json_str_nonempty(json_get(answer, "agent_id"));
    if (!id) id = json_str_nonempty(json_get(answer, "id"));
    return id ? xstrdup(id) : NULL;
}

// MARK: - Events

char *meet_start_event(void) { return xstrdup("{\"type\":\"conversation_initiation_client_data\"}"); }

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
    char *event = xstrfmt("{\"user_audio_chunk\":\"%s\"}", audio);
    free(audio);
    return event;
}
char *meet_pong_event(int event_id) { return xstrfmt("{\"type\":\"pong\",\"event_id\":%d}", event_id); }
char *meet_tool_result_event(const char *call_id, const char *result, bool is_error) {
    Json *e = json_object();
    json_set_str(e, "type", "client_tool_result");
    json_set_str(e, "tool_call_id", call_id);
    json_set_str(e, "result", result);
    json_set_bool(e, "is_error", is_error);
    return serialize_free(e);
}
char *meet_answer_now_event(void) {
    return xstrdup("{\"type\":\"user_message\",\"text\":\"Answer the latest question or request in the meeting now, briefly.\"}");
}

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
    if (str_eq(type, "conversation_initiation_metadata")) out->kind = MEET_EV_READY;
    else if (str_eq(type, "audio")) {
        const char *audio = json_str(json_get(json_get(e, "audio_event"), "audio_base_64"));
        if (audio) out->audio = base64_decode(audio, strlen(audio), &out->audio_len);
        if (out->audio) out->kind = MEET_EV_AUDIO;
    }
    else if (str_eq(type, "user_transcript")) take_text(out, MEET_EV_HEARD_TURN, json_get(json_get(e, "user_transcription_event"), "user_transcript"));
    else if (str_eq(type, "agent_response")) take_text(out, MEET_EV_SAID, json_get(json_get(e, "agent_response_event"), "agent_response"));
    else if (str_eq(type, "interruption")) out->kind = MEET_EV_INTERRUPTED;
    else if (str_eq(type, "ping")) { out->kind = MEET_EV_PING; out->event_id = json_int_or(json_get(json_get(e, "ping_event"), "event_id"), 0); }
    else if (str_eq(type, "client_tool_call")) {
        const Json *call = json_get(e, "client_tool_call");
        const char *id = json_str_nonempty(json_get(call, "tool_call_id")), *name = json_str_nonempty(json_get(call, "tool_name"));
        if (id && name) {
            out->kind = MEET_EV_TOOL;
            out->id = xstrdup(id);
            out->text = xstrdup(name);
            out->request = json_is_object(json_get(call, "parameters")) ? json_serialize(json_get(call, "parameters"), false) : xstrdup("{}");
        }
    }
    else if (str_eq(type, "client_error") || str_eq(type, "error")) {
        const Json *err = json_get(e, "error_event");
        const char *message = json_str_nonempty(json_get(err, "message"));
        if (!message) message = json_str_nonempty(json_get(err, "error_name"));
        if (!message) message = json_str_nonempty(json_get(e, "message"));
        out->kind = MEET_EV_ERROR;
        out->text = xstrfmt("ElevenLabs: %s", message ? message : "the agent reported an error.");
    }
    json_free(e);
    return true;
}
void meet_event_free(MeetEvent *e) { free(e->audio); free(e->text); free(e->id); free(e->request); memset(e, 0, sizeof *e); }

// MARK: - Cost

double meet_usage_cost(MeetModel model, const MeetUsage *u) {
    double spoken = u->spoken_chars * 0.00004;
    if (model == MEET_AGENT) return u->seconds / 60 * MEET_AGENT_MINUTE_COST;
    if (model == MEET_LIVE) return u->seconds / 60 * 0.05 + spoken;
    double tokens = (u->text_in * 0.6 + u->text_cached * 0.06 + u->text_out * 2.4 + u->audio_in * 10 + u->audio_cached * 0.3 + u->audio_out * 20) / 1e6;
    return tokens + u->transcribed_seconds / 60 * 0.0045 + spoken;
}

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
