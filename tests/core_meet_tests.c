// meet.c: the meeting assistant's events with GPT-Live and Realtime, its transcript, the agent's reply and the costs.
#include "meet.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static MeetPersona persona(bool independent) {
    MeetPersona p = { "Nadin", "marin", "Nadin, assistant", "Fix the login", true, independent };
    return p;
}
static Json *parsed(char *text) { Json *j = json_parsez(text); free(text); return j; }
static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_live_starts_with_a_client_delegation_and_pcm_at_24k(void) {
    MeetPersona p = persona(false);
    Json *e = parsed(meet_setup_event(MEET_LIVE, &p));
    CHECK_STR(json_str(json_get(e, "type")), "session.start");
    const Json *s = json_get(e, "session");
    CHECK_STR(json_str(json_get(s, "model")), "gpt-live-1");
    CHECK_STR(json_str(json_get(json_get(s, "delegation"), "type")), "client");
    CHECK_STR(json_str(json_get(json_get(json_get(s, "audio"), "format"), "type")), "audio/pcm");
    CHECK_INT(json_int_or(json_get(json_get(json_get(s, "audio"), "format"), "rate"), 0), 24000);
    CHECK_STR(json_str(json_get(json_get(json_get(s, "audio"), "output"), "voice")), "marin");
    const char *instructions = json_str(json_get(s, "instructions"));
    CHECK(strstr(instructions, "Speak only when someone addresses you or Nadin") != NULL);
    CHECK(strstr(instructions, "delegate them to the backend") != NULL);
    CHECK(strstr(instructions, "\"Fix the login\"") != NULL);
    json_free(e);
}
static void test_realtime_offers_ask_agent_and_answers_only_when_asked_unless_independent(void) {
    MeetPersona p = persona(false);
    Json *e = parsed(meet_setup_event(MEET_REALTIME, &p));
    CHECK_STR(json_str(json_get(e, "type")), "session.update");
    const Json *s = json_get(e, "session"), *turns = json_get(json_get(json_get(s, "audio"), "input"), "turn_detection");
    CHECK_STR(json_str(json_get(s, "model")), "gpt-realtime-2.1-mini");
    CHECK_STR(json_str(json_get(json_at(json_get(s, "tools"), 0), "name")), "ask_agent");
    CHECK(json_bool_is(json_get(turns, "create_response"), false));
    CHECK_STR(json_str(json_get(json_get(json_get(json_get(s, "audio"), "input"), "transcription"), "model")), "gpt-transcribe");
    CHECK(strstr(json_str(json_get(s, "instructions")), "call ask_agent") != NULL);
    json_free(e);
    p = persona(true);
    e = parsed(meet_setup_event(MEET_REALTIME, &p));
    turns = json_get(json_get(json_get(json_get(e, "session"), "audio"), "input"), "turn_detection");
    CHECK(json_bool_is(json_get(turns, "create_response"), true));
    CHECK_STR(json_str(json_get(turns, "type")), "semantic_vad");
    CHECK(strstr(json_str(json_get(json_get(e, "session"), "instructions")), "Act independently on Nadin's behalf") != NULL);
    json_free(e);
}
static void test_a_greeting_goes_only_when_asked_for(void) {
    MeetPersona p = persona(false);
    Json *live = parsed(meet_greeting_event(MEET_LIVE, &p)), *rt = parsed(meet_greeting_event(MEET_REALTIME, &p));
    CHECK_STR(json_str(json_get(live, "type")), "session.instructions.append");
    CHECK(json_is_null(json_get(live, "delegation_id")));
    CHECK_STR(json_str(json_get(rt, "type")), "response.create");
    CHECK(strstr(json_str(json_get(json_get(rt, "response"), "instructions")), "Nadin's AI assistant") != NULL);
    json_free(live); json_free(rt);
    p.introduce = false;
    CHECK(meet_greeting_event(MEET_LIVE, &p) == NULL);
}
static void test_base64_encodes_every_tail_length(void) {
    CHECK_OWNED_STR(base64_encode("", 0), "");
    CHECK_OWNED_STR(base64_encode("f", 1), "Zg==");
    CHECK_OWNED_STR(base64_encode("fo", 2), "Zm8=");
    CHECK_OWNED_STR(base64_encode("foo", 3), "Zm9v");
    CHECK_OWNED_STR(base64_encode("foobar", 6), "Zm9vYmFy");
}
static void test_audio_goes_out_as_each_api_names_it(void) {
    int16_t pcm[2] = { 1, -1 };
    Json *live = parsed(meet_audio_event(MEET_LIVE, pcm, 2)), *rt = parsed(meet_audio_event(MEET_REALTIME, pcm, 2));
    CHECK_STR(json_str(json_get(live, "type")), "session.input_audio.append");
    CHECK_STR(json_str(json_get(live, "audio")), "AQD//w==");
    CHECK_STR(json_str(json_get(rt, "type")), "input_audio_buffer.append");
    json_free(live); json_free(rt);
}
static void test_results_answer_a_delegation_or_a_function_call(void) {
    char *out[2] = { 0 };
    CHECK_INT(meet_result_events(MEET_LIVE, "item_1", "It ships today.", out), 1);
    Json *e = parsed(out[0]);
    CHECK_STR(json_str(json_get(e, "type")), "session.commentary.append");
    CHECK_STR(json_str(json_get(e, "delegation_id")), "item_1");
    CHECK_STR(json_str(json_get(e, "content")), "It ships today.");
    json_free(e);
    CHECK_INT(meet_result_events(MEET_REALTIME, "call_9", "Yes.", out), 2);
    e = parsed(out[0]);
    CHECK_STR(json_str(json_get(json_get(e, "item"), "type")), "function_call_output");
    CHECK_STR(json_str(json_get(json_get(e, "item"), "call_id")), "call_9");
    CHECK_STR(json_str(json_get(json_get(e, "item"), "output")), "{\"answer\":\"Yes.\"}");
    json_free(e);
    CHECK_OWNED_STR(out[1], "{\"type\":\"response.create\"}");
    char *progress = meet_progress_event(MEET_LIVE, "item_1", "Still checking.");
    CHECK(strstr(progress, "session.thinking.append") != NULL);
    free(progress);
    CHECK(meet_progress_event(MEET_REALTIME, "call_9", "x") == NULL);
    CHECK(meet_close_event(MEET_REALTIME) == NULL);
    CHECK_OWNED_STR(meet_close_event(MEET_LIVE), "{\"type\":\"session.close\"}");
    CHECK_OWNED_STR(meet_answer_now_event(MEET_REALTIME), "{\"type\":\"response.create\"}");
    char *now = meet_answer_now_event(MEET_LIVE);
    CHECK(strstr(now, "session.instructions.append") != NULL);
    free(now);
}

static MeetEvent parse(MeetModel m, const char *json) { MeetEvent e; CHECK(meet_event_parse(m, json, strlen(json), &e)); return e; }
static void test_live_events_are_read(void) {
    MeetEvent e = parse(MEET_LIVE, "{\"type\":\"session.started\",\"session\":{\"id\":\"s\"}}");
    CHECK_INT(e.kind, MEET_EV_READY); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.output_audio.delta\",\"delta\":\"AQD//w==\"}");
    CHECK_INT(e.kind, MEET_EV_AUDIO); CHECK_INT(e.audio_len, 4); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.input_transcript.delta\",\"delta\":\"What is\",\"start_ms\":1}");
    CHECK_INT(e.kind, MEET_EV_HEARD); CHECK_STR(e.text, "What is"); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.output_transcript.delta\",\"delta\":\"Let me check\"}");
    CHECK_INT(e.kind, MEET_EV_SAID); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.delegation.created\",\"offset_ms\":1000,\"delegation\":{\"id\":\"item_9\",\"type\":\"delegation\",\"target\":\"client\"}}");
    CHECK_INT(e.kind, MEET_EV_ASK); CHECK_STR(e.id, "item_9"); CHECK(e.request == NULL); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"error\",\"error\":{\"message\":\"Bad key\"}}");
    CHECK_INT(e.kind, MEET_EV_ERROR); CHECK_STR(e.text, "Bad key"); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.usage.updated\",\"usage\":{\"seconds\":12}}");
    CHECK(e.has_usage && e.usage_snapshot); CHECK(near(e.usage.seconds, 12)); meet_event_free(&e);
    e = parse(MEET_LIVE, "{\"type\":\"session.closed\",\"usage\":{\"seconds\":15},\"reason\":\"close_requested\"}");
    CHECK_INT(e.kind, MEET_EV_CLOSED); CHECK(near(e.usage.seconds, 15)); meet_event_free(&e);
    MeetEvent bad;
    CHECK(!meet_event_parse(MEET_LIVE, "not json", 8, &bad));
}
static void test_realtime_events_are_read(void) {
    MeetEvent e = parse(MEET_REALTIME, "{\"type\":\"session.updated\"}");
    CHECK_INT(e.kind, MEET_EV_READY); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.output_audio.delta\",\"delta\":\"AQD//w==\"}");
    CHECK_INT(e.kind, MEET_EV_AUDIO); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"input_audio_buffer.speech_started\"}");
    CHECK_INT(e.kind, MEET_EV_SPEECH_STARTED); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"Nadin, any news?\",\"usage\":{\"type\":\"duration\",\"seconds\":120}}");
    CHECK_INT(e.kind, MEET_EV_HEARD_TURN); CHECK_STR(e.text, "Nadin, any news?"); CHECK(e.has_usage && !e.usage_snapshot);
    CHECK(near(e.usage.transcribed_seconds, 120)); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"name\":\"ask_agent\",\"call_id\":\"call_1\",\"arguments\":\"{\\\"request\\\":\\\"Is CI green?\\\"}\"}}");
    CHECK_INT(e.kind, MEET_EV_ASK); CHECK_STR(e.id, "call_1"); CHECK_STR(e.request, "Is CI green?"); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\"}}");
    CHECK_INT(e.kind, MEET_EV_OTHER); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.done\",\"response\":{\"usage\":{\"input_token_details\":{\"text_tokens\":119,\"audio_tokens\":13,\"cached_tokens_details\":{\"text_tokens\":64,\"audio_tokens\":0}},\"output_token_details\":{\"text_tokens\":30,\"audio_tokens\":91}}}}");
    CHECK(e.has_usage); CHECK(near(e.usage.text_in, 55)); CHECK(near(e.usage.text_cached, 64)); CHECK(near(e.usage.audio_in, 13));
    CHECK(near(e.usage.text_out, 30)); CHECK(near(e.usage.audio_out, 91));
    meet_event_free(&e);
}
static void test_costs_follow_each_models_rates(void) {
    MeetUsage live = { .seconds = 90 };
    CHECK(near(meet_usage_cost(MEET_LIVE, &live), 0.075));
    MeetUsage rt = { .text_in = 1e6, .text_cached = 1e6, .text_out = 1e6, .audio_in = 1e6, .audio_cached = 1e6, .audio_out = 1e6, .transcribed_seconds = 60 };
    CHECK(near(meet_usage_cost(MEET_REALTIME, &rt), 0.6 + 0.06 + 2.4 + 10 + 0.3 + 20 + 0.0045));
    MeetUsage total = { 0 }, snap = { .seconds = 12 }, later = { .seconds = 15 }, tokens = { .audio_out = 5, .transcribed_seconds = 2 };
    meet_usage_merge(&total, &snap, true); meet_usage_merge(&total, &later, true); meet_usage_merge(&total, &snap, true);
    CHECK(near(total.seconds, 15));
    meet_usage_merge(&total, &tokens, false); meet_usage_merge(&total, &tokens, false);
    CHECK(near(total.audio_out, 10)); CHECK(near(total.transcribed_seconds, 4));
}
static void test_records_round_trip_and_add_up_by_model(void) {
    MeetRecord a = { MEET_LIVE, 1000, 600, { .seconds = 600 }, 0.4, 3, 2, 30 }, b = { MEET_REALTIME, 2000, 300, { .audio_out = 1e5 }, 0.1, 1, 1, 8 }, back;
    Json *records = json_array();
    json_array_push(records, meet_record_json(&a));
    json_array_push(records, meet_record_json(&b));
    json_array_push(records, meet_record_json(&a));
    json_array_push(records, json_parsez("{\"model\":\"gpt-unknown\"}"));
    CHECK(meet_record_parse(json_at(records, 1), &back));
    CHECK_INT(back.model, MEET_REALTIME); CHECK(near(back.usage.audio_out, 1e5)); CHECK(near(back.started, 2000)); CHECK_INT(back.answers, 1);
    CHECK(near(json_num_or(json_get(json_at(records, 0), "voiceCost"), 0), 0.5));
    MeetTotals t[MEET_MODEL_COUNT];
    meet_totals(records, t);
    CHECK_INT(t[MEET_LIVE].meetings, 2); CHECK(near(t[MEET_LIVE].seconds, 1200)); CHECK(near(t[MEET_LIVE].voice_cost, 1.0));
    CHECK(near(t[MEET_LIVE].agent_cost, 0.8)); CHECK_INT(t[MEET_LIVE].requests, 6); CHECK(near(t[MEET_LIVE].answer_seconds, 60));
    CHECK_INT(t[MEET_REALTIME].meetings, 1); CHECK(near(t[MEET_REALTIME].voice_cost, 2.0));
    json_free(records);
}
static void test_the_log_keeps_speakers_on_their_own_lines_and_its_tail(void) {
    MeetLog log; meet_log_init(&log);
    CHECK_OWNED_STR(meet_log_tail(&log, 100), "");
    meet_log_add(&log, MEET_SPEAKER_MEETING, " What is");
    meet_log_add(&log, MEET_SPEAKER_MEETING, " the status?");
    meet_log_add(&log, MEET_SPEAKER_ASSISTANT, "Let me check.");
    meet_log_add(&log, MEET_SPEAKER_ASSISTANT, NULL);
    CHECK_OWNED_STR(meet_log_tail(&log, 1000), "Meeting: What is the status?\nAssistant: Let me check.");
    CHECK_OWNED_STR(meet_log_tail(&log, 30), "Assistant: Let me check.");
    for (int i = 0; i < 2000; i++) { meet_log_add(&log, MEET_SPEAKER_MEETING, "and so on and so forth"); meet_log_add(&log, MEET_SPEAKER_ASSISTANT, "ok"); }
    CHECK(log.text.len <= 24000 + 40);
    CHECK(str_has_prefix(log.text.data, "Meeting: ") || str_has_prefix(log.text.data, "Assistant: "));
    meet_log_free(&log);
}
static void test_wake_words_match_whole_words_and_only_an_addressed_assistant_is_asked(void) {
    CHECK(meet_wake_word("Hey ASSISTANT, what's up?", "Nadin, assistant"));
    CHECK(meet_wake_word("nadin?", "Nadin, assistant"));
    CHECK(!meet_wake_word("the assistants are here", "Nadin, assistant"));
    CHECK(!meet_wake_word("nothing", ""));
    CHECK(!meet_wake_word(NULL, "x"));
    MeetPersona addressed = persona(false), independent = persona(true);
    CHECK(meet_should_answer(&addressed, "Nadin, how is it going?"));
    CHECK(!meet_should_answer(&addressed, "How is it going?"));
    CHECK(!meet_should_answer(&independent, "Nadin, how is it going?"));
}
static void test_the_agent_message_carries_its_marker_and_the_limits(void) {
    MeetPersona p = persona(false);
    char *m = meet_agent_message(3, &p, "Is CI green?", "Meeting: Is CI green?");
    char *marker = meet_agent_marker(3);
    CHECK(str_has_prefix(m, marker));
    CHECK(strstr(m, "Is CI green?\n\n") != NULL);
    CHECK(strstr(m, "only answer") != NULL);
    CHECK(strstr(m, "The meeting so far") != NULL);
    free(m);
    p = persona(true);
    m = meet_agent_message(4, &p, NULL, NULL);
    CHECK(strstr(m, "answer it.") != NULL);
    CHECK(strstr(m, "changes as well as answers") != NULL);
    CHECK(strstr(m, "The meeting so far") == NULL);
    free(m); free(marker);
}
static void test_replies_are_made_fit_to_say(void) {
    CHECK_OWNED_STR(meet_spoken("# Status\n\n- **CI** is `green`\n- PR #12 merged\n```\ncode\n```\n> done", 1000), "Status CI is green PR #12 merged done");
    CHECK_OWNED_STR(meet_spoken("One. Two. Three is long", 12), "One. Two.");
    CHECK_OWNED_STR(meet_spoken("abcdef ghijkl", 9), "abcdef");
    CHECK_OWNED_STR(meet_spoken("abcdefghijkl", 5), "abcde");
    CHECK_OWNED_STR(meet_spoken(NULL, 5), "");
}
static void test_the_reply_is_read_after_its_message_until_the_turn_ends(void) {
    char *marker = meet_agent_marker(2);
    MeetReplyScan scan; meet_reply_init(&scan);
    Json *before = json_parsez("[{\"seq\":1,\"kind\":\"text\",\"text\":\"older\"},{\"seq\":2,\"kind\":\"user\",\"text\":\"\xF0\x9F\x8E\x99 Meeting request 1, earlier\"}]");
    meet_reply_feed(&scan, before, marker);
    CHECK(!scan.seen);
    char *mine = xstrfmt("[{\"seq\":3,\"kind\":\"user\",\"text\":\"%s, from\"},{\"seq\":4,\"kind\":\"text\",\"text\":\"CI is green.\"}]", marker);
    Json *first = json_parsez(mine);
    meet_reply_feed(&scan, first, marker);
    CHECK(scan.seen && !scan.done);
    Json *rest = json_parsez("[{\"seq\":5,\"kind\":\"tool\",\"name\":\"Bash\"},{\"seq\":6,\"kind\":\"text\",\"text\":\"Deploy at 5.\"},{\"seq\":7,\"kind\":\"result\"},{\"seq\":8,\"kind\":\"text\",\"text\":\"later\"}]");
    meet_reply_feed(&scan, rest, marker);
    CHECK(scan.done);
    CHECK_STR(scan.reply.data, "CI is green.\nDeploy at 5.");
    meet_reply_free(&scan);
    meet_reply_init(&scan);
    meet_reply_feed(&scan, first, marker);
    Json *ask = json_parsez("[{\"seq\":9,\"kind\":\"ask\",\"question\":\"Which branch?\"}]");
    meet_reply_feed(&scan, ask, marker);
    CHECK(scan.done);
    CHECK_STR(scan.reply.data, "CI is green.\nThe agent asks: Which branch?");
    meet_reply_free(&scan);
    json_free(before); json_free(first); json_free(rest); json_free(ask); free(mine); free(marker);
}
static void test_an_elevenlabs_voice_makes_realtime_answer_in_text_as_the_user(void) {
    MeetPersona p = persona(false);
    p.eleven_voice = "zozOsuFj6BSfLStTxdrK";
    CHECK(meet_text_out(MEET_REALTIME, &p)); CHECK(!meet_text_out(MEET_LIVE, &p));
    Json *e = parsed(meet_setup_event(MEET_REALTIME, &p));
    const Json *s = json_get(e, "session");
    CHECK_STR(json_str(json_at(json_get(s, "output_modalities"), 0)), "text");
    CHECK(!json_is_object(json_get(json_get(s, "audio"), "output")));
    CHECK_STR(json_str(json_get(json_get(json_get(json_get(s, "audio"), "input"), "format"), "type")), "audio/pcm");
    const char *instructions = json_str(json_get(s, "instructions"));
    CHECK(strstr(instructions, "Speak as Nadin, in the first person") != NULL);
    CHECK(strstr(instructions, "no Markdown") != NULL);
    CHECK(strstr(instructions, "Never claim") == NULL);
    json_free(e);
    // GPT-Live keeps its OpenAI voice.
    e = parsed(meet_setup_event(MEET_LIVE, &p));
    CHECK_STR(json_str(json_get(json_get(json_get(json_get(e, "session"), "audio"), "output"), "voice")), "marin");
    CHECK(strstr(json_str(json_get(json_get(e, "session"), "instructions")), "AI voice assistant") != NULL);
    json_free(e);
    p.eleven_voice = "";
    CHECK(!meet_text_out(MEET_REALTIME, &p));
}
static void test_realtime_text_answers_are_read(void) {
    MeetEvent e = parse(MEET_REALTIME, "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}");
    CHECK_INT(e.kind, MEET_EV_RESPONSE); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.output_text.delta\",\"delta\":\"CI is \"}");
    CHECK_INT(e.kind, MEET_EV_SAID); CHECK_STR(e.text, "CI is "); meet_event_free(&e);
    e = parse(MEET_REALTIME, "{\"type\":\"response.output_text.done\",\"text\":\"CI is green.\"}");
    CHECK_INT(e.kind, MEET_EV_SAID_DONE); meet_event_free(&e);
}
static void test_elevenlabs_speaks_v4_turbo_pcm_at_24k(void) {
    CHECK_STR(eleven_ws_path(), "/v1/text-to-dialogue/stream-input?model_id=eleven_v4_turbo&output_format=pcm_24000");
    Json *open = parsed(eleven_open_event("v1"));
    CHECK_STR(json_str(json_at(json_get(open, "voices"), 0)), "v1"); CHECK_INT(json_count(json_get(open, "voices")), 1);
    Json *first = parsed(eleven_text_event("v1", "Hello there", true)), *more = parsed(eleven_text_event("v1", " again", false));
    const Json *input = json_at(json_get(first, "inputs"), 0);
    CHECK_STR(json_str(json_get(input, "text")), "Hello there"); CHECK_STR(json_str(json_get(input, "voice_id")), "v1");
    CHECK(json_bool_is(json_get(input, "new_turn"), true));
    CHECK(!json_bool_is(json_get(json_at(json_get(more, "inputs"), 0), "new_turn"), true));
    Json *flush = parsed(eleven_flush_event()), *alive = parsed(eleven_keep_alive_event());
    CHECK(json_bool_is(json_get(flush, "flush"), true)); CHECK(json_bool_is(json_get(alive, "keep_alive"), true));
    json_free(open); json_free(first); json_free(more); json_free(flush); json_free(alive);
    MeetEvent e;
    const char *audio = "{\"audio\":\"AQD//w==\",\"alignment\":null}";
    CHECK(eleven_event_parse(audio, strlen(audio), &e)); CHECK_INT(e.kind, MEET_EV_AUDIO); CHECK_INT((int)e.audio_len, 4); meet_event_free(&e);
    const char *error = "{\"message\":\"Invalid API key\",\"error\":\"authentication_required\",\"code\":1008}";
    CHECK(eleven_event_parse(error, strlen(error), &e)); CHECK_INT(e.kind, MEET_EV_ERROR); CHECK_STR(e.text, "ElevenLabs: Invalid API key"); meet_event_free(&e);
    const char *final = "{\"is_final\":true}";
    CHECK(eleven_event_parse(final, strlen(final), &e)); CHECK_INT(e.kind, MEET_EV_OTHER); meet_event_free(&e);
    CHECK(!eleven_event_parse("nope", 4, &e));
    CHECK_INT((int)meet_char_count("caf\xC3\xA9!"), 5);
    MeetUsage spoken = { .spoken_chars = 1000 };
    CHECK(near(meet_usage_cost(MEET_REALTIME, &spoken), 0.04));
}
static void test_models_are_named(void) {
    CHECK_STR(meet_model_label(MEET_LIVE), "GPT-Live 1");
    CHECK_STR(meet_ws_path(MEET_REALTIME), "/v1/realtime?model=gpt-realtime-2.1-mini");
    MeetPersona nameless = { 0 };
    char *i = meet_instructions(MEET_LIVE, &nameless);
    CHECK(strstr(i, "the user's AI voice assistant") != NULL && strstr(i, "\"this project\"") != NULL);
    free(i);
}

void meet_tests(void) {
    test_run("live starts with a client delegation and pcm at 24k", test_live_starts_with_a_client_delegation_and_pcm_at_24k);
    test_run("realtime offers ask_agent and answers only when asked unless independent", test_realtime_offers_ask_agent_and_answers_only_when_asked_unless_independent);
    test_run("a greeting goes only when asked for", test_a_greeting_goes_only_when_asked_for);
    test_run("base64 encodes every tail length", test_base64_encodes_every_tail_length);
    test_run("audio goes out as each api names it", test_audio_goes_out_as_each_api_names_it);
    test_run("results answer a delegation or a function call", test_results_answer_a_delegation_or_a_function_call);
    test_run("live events are read", test_live_events_are_read);
    test_run("realtime events are read", test_realtime_events_are_read);
    test_run("costs follow each model's rates", test_costs_follow_each_models_rates);
    test_run("records round trip and add up by model", test_records_round_trip_and_add_up_by_model);
    test_run("the log keeps speakers on their own lines and its tail", test_the_log_keeps_speakers_on_their_own_lines_and_its_tail);
    test_run("wake words match whole words and only an addressed assistant is asked", test_wake_words_match_whole_words_and_only_an_addressed_assistant_is_asked);
    test_run("the agent message carries its marker and the limits", test_the_agent_message_carries_its_marker_and_the_limits);
    test_run("replies are made fit to say", test_replies_are_made_fit_to_say);
    test_run("the reply is read after its message until the turn ends", test_the_reply_is_read_after_its_message_until_the_turn_ends);
    test_run("models are named", test_models_are_named);
    test_run("an elevenlabs voice makes realtime answer in text as the user", test_an_elevenlabs_voice_makes_realtime_answer_in_text_as_the_user);
    test_run("realtime text answers are read", test_realtime_text_answers_are_read);
    test_run("elevenlabs speaks v4 turbo pcm at 24k", test_elevenlabs_speaks_v4_turbo_pcm_at_24k);
}
