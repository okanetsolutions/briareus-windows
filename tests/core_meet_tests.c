// meet.c and meet_tools.c: the meeting assistant's events with GPT-Realtime, its project tools, its transcript, its
// ElevenLabs voice and the costs.
#include "meet.h"
#include "meet_tools.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static MeetPersona persona(bool independent) {
    MeetPersona p = { "Nadin", "marin", "Nadin, assistant", "Briareus (nadinyamaui/briareus)", true, independent, NULL };
    return p;
}
static Json *parsed(char *text) { Json *j = json_parsez(text); free(text); return j; }
static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_realtime_offers_the_project_tools_and_answers_only_when_asked_unless_independent(void) {
    MeetPersona p = persona(false);
    Json *e = parsed(meet_setup_event(&p));
    CHECK_STR(json_str(json_get(e, "type")), "session.update");
    const Json *s = json_get(e, "session"), *input = json_get(json_get(s, "audio"), "input"), *turns = json_get(input, "turn_detection");
    CHECK_STR(json_str(json_get(s, "model")), "gpt-realtime-2.1-mini");
    CHECK_STR(json_str(json_at(json_get(s, "output_modalities"), 0)), "audio");
    CHECK_STR(json_str(json_get(json_get(json_get(s, "audio"), "output"), "voice")), "marin");
    CHECK_STR(json_str(json_get(json_get(input, "format"), "type")), "audio/pcm");
    CHECK_INT(json_int_or(json_get(json_get(input, "format"), "rate"), 0), 24000);
    CHECK(json_bool_is(json_get(turns, "create_response"), false));
    CHECK_STR(json_str(json_get(json_get(input, "transcription"), "model")), "gpt-transcribe");
    const Json *tools = json_get(s, "tools");
    CHECK_INT((int)json_count(tools), MEET_TOOL_COUNT);
    CHECK_STR(json_str(json_get(json_at(tools, 0), "name")), "list_conversations");
    for (size_t i = 0; i < json_count(tools); i++) {
        const char *name = json_str(json_get(json_at(tools, i), "name"));
        // Read-only: nothing that starts, messages, merges, closes or deletes.
        CHECK(!strstr(name, "start") && !strstr(name, "send") && !strstr(name, "merge") && !strstr(name, "delete") && !strstr(name, "close"));
    }
    const Json *read_pr = json_at(tools, MEET_TOOL_READ_PULL_REQUEST);
    CHECK_STR(json_str(json_at(json_get(json_get(read_pr, "parameters"), "required"), 0)), "number");
    CHECK(json_bool_is(json_get(json_get(read_pr, "parameters"), "additionalProperties"), false));
    const char *instructions = json_str(json_get(s, "instructions"));
    CHECK(strstr(instructions, "Speak only when someone addresses you or Nadin") != NULL);
    CHECK(strstr(instructions, "Briareus (nadinyamaui/briareus)") != NULL);
    CHECK(strstr(instructions, "no tool changes anything") != NULL);
    CHECK(strstr(instructions, "Never claim") == NULL);
    json_free(e);
    p = persona(true);
    e = parsed(meet_setup_event(&p));
    turns = json_get(json_get(json_get(json_get(e, "session"), "audio"), "input"), "turn_detection");
    CHECK(json_bool_is(json_get(turns, "create_response"), true));
    CHECK_STR(json_str(json_get(turns, "type")), "semantic_vad");
    CHECK(strstr(json_str(json_get(json_get(e, "session"), "instructions")), "Act independently on Nadin's behalf") != NULL);
    json_free(e);
}
static void test_an_elevenlabs_voice_makes_realtime_answer_in_text_as_the_user(void) {
    MeetPersona p = persona(false);
    p.eleven_voice = "zozOsuFj6BSfLStTxdrK";
    CHECK(meet_text_out(&p));
    Json *e = parsed(meet_setup_event(&p));
    const Json *s = json_get(e, "session");
    CHECK_STR(json_str(json_at(json_get(s, "output_modalities"), 0)), "text");
    CHECK(!json_is_object(json_get(json_get(s, "audio"), "output")));
    const char *instructions = json_str(json_get(s, "instructions"));
    CHECK(strstr(instructions, "Speak as Nadin, in the first person") != NULL);
    CHECK(strstr(instructions, "no Markdown") != NULL);
    json_free(e);
    p.eleven_voice = "";
    CHECK(!meet_text_out(&p));
}
static void test_a_greeting_goes_only_when_asked_for(void) {
    MeetPersona p = persona(false);
    Json *rt = parsed(meet_greeting_event(&p));
    CHECK_STR(json_str(json_get(rt, "type")), "response.create");
    CHECK(strstr(json_str(json_get(json_get(rt, "response"), "instructions")), "Nadin's AI assistant") != NULL);
    json_free(rt);
    p.introduce = false;
    CHECK(meet_greeting_event(&p) == NULL);
}
static void test_base64_encodes_every_tail_length(void) {
    CHECK_OWNED_STR(base64_encode("", 0), "");
    CHECK_OWNED_STR(base64_encode("f", 1), "Zg==");
    CHECK_OWNED_STR(base64_encode("fo", 2), "Zm8=");
    CHECK_OWNED_STR(base64_encode("foo", 3), "Zm9v");
    CHECK_OWNED_STR(base64_encode("foobar", 6), "Zm9vYmFy");
}
static void test_audio_and_tool_answers_go_out_as_realtime_names_them(void) {
    int16_t pcm[2] = { 1, -1 };
    Json *rt = parsed(meet_audio_event(pcm, 2));
    CHECK_STR(json_str(json_get(rt, "type")), "input_audio_buffer.append");
    CHECK_STR(json_str(json_get(rt, "audio")), "AQD//w==");
    json_free(rt);
    char *out[2] = { 0 };
    CHECK_INT((int)meet_tool_output_events("call_9", "{\"total\":0}", out), 2);
    Json *e = parsed(out[0]);
    CHECK_STR(json_str(json_get(e, "type")), "conversation.item.create");
    CHECK_STR(json_str(json_get(json_get(e, "item"), "type")), "function_call_output");
    CHECK_STR(json_str(json_get(json_get(e, "item"), "call_id")), "call_9");
    CHECK_STR(json_str(json_get(json_get(e, "item"), "output")), "{\"total\":0}");
    json_free(e);
    CHECK_OWNED_STR(out[1], "{\"type\":\"response.create\"}");
    CHECK_OWNED_STR(meet_answer_now_event(), "{\"type\":\"response.create\"}");
}

static MeetEvent parse(const char *json) { MeetEvent e; CHECK(meet_event_parse(json, strlen(json), &e)); return e; }
static void test_realtime_events_are_read(void) {
    MeetEvent e = parse("{\"type\":\"session.updated\"}");
    CHECK_INT(e.kind, MEET_EV_READY); meet_event_free(&e);
    e = parse("{\"type\":\"response.output_audio.delta\",\"delta\":\"AQD//w==\"}");
    CHECK_INT(e.kind, MEET_EV_AUDIO); CHECK_INT((int)e.audio_len, 4); meet_event_free(&e);
    e = parse("{\"type\":\"input_audio_buffer.speech_started\"}");
    CHECK_INT(e.kind, MEET_EV_SPEECH_STARTED); meet_event_free(&e);
    e = parse("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"Nadin, any news?\",\"usage\":{\"type\":\"duration\",\"seconds\":120}}");
    CHECK_INT(e.kind, MEET_EV_HEARD_TURN); CHECK_STR(e.text, "Nadin, any news?"); CHECK(e.has_usage && !e.usage_snapshot);
    CHECK(near(e.usage.transcribed_seconds, 120)); meet_event_free(&e);
    e = parse("{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"name\":\"read_pull_request\",\"call_id\":\"call_1\",\"arguments\":\"{\\\"number\\\":12}\"}}");
    CHECK_INT(e.kind, MEET_EV_TOOL); CHECK_STR(e.id, "call_1"); CHECK_STR(e.text, "read_pull_request"); CHECK_STR(e.request, "{\"number\":12}"); meet_event_free(&e);
    e = parse("{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\"}}");
    CHECK_INT(e.kind, MEET_EV_OTHER); meet_event_free(&e);
    e = parse("{\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}");
    CHECK_INT(e.kind, MEET_EV_RESPONSE); meet_event_free(&e);
    e = parse("{\"type\":\"response.output_text.delta\",\"delta\":\"CI is \"}");
    CHECK_INT(e.kind, MEET_EV_SAID); CHECK_STR(e.text, "CI is "); meet_event_free(&e);
    e = parse("{\"type\":\"response.output_text.done\",\"text\":\"CI is green.\"}");
    CHECK_INT(e.kind, MEET_EV_SAID_DONE); meet_event_free(&e);
    e = parse("{\"type\":\"error\",\"error\":{\"message\":\"Bad key\"}}");
    CHECK_INT(e.kind, MEET_EV_ERROR); CHECK_STR(e.text, "Bad key"); meet_event_free(&e);
    e = parse("{\"type\":\"response.done\",\"response\":{\"usage\":{\"input_token_details\":{\"text_tokens\":119,\"audio_tokens\":13,\"cached_tokens_details\":{\"text_tokens\":64,\"audio_tokens\":0}},\"output_token_details\":{\"text_tokens\":30,\"audio_tokens\":91}}}}");
    CHECK(e.has_usage); CHECK(near(e.usage.text_in, 55)); CHECK(near(e.usage.text_cached, 64)); CHECK(near(e.usage.audio_in, 13));
    CHECK(near(e.usage.text_out, 30)); CHECK(near(e.usage.audio_out, 91));
    meet_event_free(&e);
    MeetEvent bad;
    CHECK(!meet_event_parse("not json", 8, &bad));
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
static void test_replies_are_made_fit_to_say(void) {
    CHECK_OWNED_STR(meet_spoken("# Status\n\n- **CI** is `green`\n- PR #12 merged\n```\ncode\n```\n> done", 1000), "Status CI is green PR #12 merged done");
    CHECK_OWNED_STR(meet_spoken("One. Two. Three is long", 12), "One. Two.");
    CHECK_OWNED_STR(meet_spoken("abcdef ghijkl", 9), "abcdef");
    CHECK_OWNED_STR(meet_spoken("abcdefghijkl", 5), "abcde");
    CHECK_OWNED_STR(meet_spoken(NULL, 5), "");
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
    char *i = meet_instructions(&nameless);
    CHECK(strstr(i, "the user's AI voice assistant") != NULL && strstr(i, "the project this project") != NULL);
    free(i);
}

// MARK: - Tools

#define REPO "okanet/app"
static const char SESSIONS[] = "{\"sessions\":["
    "{\"id\":\"s1\",\"repo\":\"okanet/app\",\"title\":\"Fix the login\",\"status\":\"idle\",\"prStatus\":{\"number\":12,\"state\":\"merged\",\"title\":\"Fix login\",\"checks\":{\"total\":3,\"passed\":3,\"failed\":0,\"pending\":0}}},"
    "{\"id\":\"s2\",\"repo\":\"okanet/app\",\"title\":\"Issue #7: Dark mode\",\"status\":\"running\"},"
    "{\"id\":\"s3\",\"repo\":\"okanet/other\",\"title\":\"Elsewhere\",\"status\":\"idle\"},"
    "{\"id\":\"s4\",\"repo\":\"okanet/app\",\"title\":\"Old\",\"status\":\"closed\",\"startedOnPr\":20}]}";

static char *run_tool(MeetTool tool, const char *args_json, const char *const *answers_json, size_t count) {
    Json *args = json_parsez(args_json);
    Json *answers[MEET_TOOL_MAX_CALLS] = { 0 };
    for (size_t i = 0; i < count; i++) answers[i] = answers_json[i] ? json_parsez(answers_json[i]) : NULL;
    char *out = meet_tool_summary(tool, args, REPO, (const Json *const *)answers, count);
    for (size_t i = 0; i < count; i++) json_free(answers[i]);
    json_free(args);
    return out;
}

static void test_each_tool_reads_the_project_only(void) {
    MeetTool t;
    CHECK(meet_tool_find("list_issues", &t) && t == MEET_TOOL_LIST_ISSUES);
    CHECK(!meet_tool_find("merge_pull_request", &t));
    MeetCall calls[MEET_TOOL_MAX_CALLS]; char *refusal = NULL;
    Json *args = json_parsez("{\"number\":12}");
    size_t n = meet_tool_calls(MEET_TOOL_READ_PULL_REQUEST, args, REPO, calls, &refusal);
    CHECK_INT((int)n, 2); CHECK(refusal == NULL);
    CHECK_STR(calls[0].op, "pull_files"); CHECK_STR(json_str(json_get(calls[0].args, "repo")), REPO); CHECK_INT(json_int_or(json_get(calls[0].args, "pr"), 0), 12);
    CHECK_STR(calls[1].op, "pull_description");
    meet_calls_free(calls, n); json_free(args);
    args = json_parsez("{\"issue\":7}");
    n = meet_tool_calls(MEET_TOOL_READ_ISSUE, args, REPO, calls, &refusal);
    CHECK_INT((int)n, 3); CHECK_STR(calls[0].op, "issue"); CHECK_STR(calls[1].op, "issue_timeline"); CHECK_STR(calls[2].op, "sessions");
    CHECK_INT(json_int_or(json_get(calls[1].args, "page"), 0), 1);
    meet_calls_free(calls, n); json_free(args);
    args = json_parsez("{\"session_id\":\"s1\"}");
    n = meet_tool_calls(MEET_TOOL_READ_CONVERSATION, args, REPO, calls, &refusal);
    CHECK_INT((int)n, 2); CHECK_STR(calls[0].op, "sessions"); CHECK_STR(calls[1].op, "session"); CHECK_STR(json_str(json_get(calls[1].args, "sessionId")), "s1");
    meet_calls_free(calls, n); json_free(args);
    // Missing arguments are refused with what to say.
    args = json_parsez("{}");
    CHECK_INT((int)meet_tool_calls(MEET_TOOL_LIST_FINDINGS, args, REPO, calls, &refusal), 0);
    CHECK(refusal && strstr(refusal, "pull request") != NULL);
    free(refusal);
    CHECK_INT((int)meet_tool_calls(MEET_TOOL_READ_CONVERSATION, args, REPO, calls, &refusal), 0);
    free(refusal);
    json_free(args);
    char *instructions = meet_tools_instructions("App (okanet/app)");
    CHECK(strstr(instructions, "App (okanet/app)") != NULL && strstr(instructions, "ready_to_merge") != NULL);
    free(instructions);
}

static void test_conversations_are_listed_with_their_pull_requests(void) {
    const char *answers[] = { SESSIONS };
    Json *o = parsed(run_tool(MEET_TOOL_LIST_CONVERSATIONS, "{}", answers, 1));
    const Json *list = json_get(o, "conversations");
    CHECK_INT(json_int_or(json_get(o, "total"), 0), 3);
    CHECK_STR(json_str(json_get(json_at(list, 0), "session_id")), "s1");
    const Json *pr = json_get(json_at(list, 0), "pull_request");
    CHECK_INT(json_int_or(json_get(pr, "number"), 0), 12);
    CHECK_STR(json_str(json_get(pr, "state")), "merged");
    CHECK_STR(json_str(json_get(pr, "checks")), "all 3 passed");
    CHECK_INT(json_int_or(json_get(json_get(json_at(list, 2), "pull_request"), "number"), 0), 20);
    json_free(o);
    o = parsed(run_tool(MEET_TOOL_LIST_CONVERSATIONS, "{\"active_only\":true}", answers, 1));
    CHECK_INT(json_int_or(json_get(o, "total"), 0), 2);
    json_free(o);
}

static void test_a_conversation_is_read_with_its_open_question(void) {
    const char *answers[] = { SESSIONS, "{\"session\":{\"id\":\"s2\",\"repo\":\"okanet/app\",\"title\":\"Issue #7: Dark mode\",\"status\":\"idle\"},\"events\":["
        "{\"seq\":1,\"kind\":\"user\",\"text\":\"Add **dark** mode\"},{\"seq\":2,\"kind\":\"tool\",\"name\":\"Bash\"},"
        "{\"seq\":3,\"kind\":\"text\",\"text\":\"Done with the toggle.\"},{\"seq\":4,\"kind\":\"ask\",\"question\":\"Which default?\",\"options\":[{\"label\":\"Light\"},{\"label\":\"Dark\"}]}]}" };
    Json *o = parsed(run_tool(MEET_TOOL_READ_CONVERSATION, "{\"session_id\":\"s2\"}", answers, 2));
    CHECK_STR(json_str(json_get(o, "status")), "waiting for an answer");
    CHECK_STR(json_str(json_get(o, "question")), "Which default?");
    CHECK_STR(json_str(json_at(json_get(o, "options"), 1)), "Dark");
    const Json *latest = json_get(o, "latest");
    CHECK_INT((int)json_count(latest), 3);
    CHECK_STR(json_str(json_get(json_at(latest, 0), "from")), "user");
    CHECK_STR(json_str(json_get(json_at(latest, 0), "text")), "Add dark mode");
    CHECK_STR(json_str(json_get(json_at(latest, 2), "from")), "agent");
    json_free(o);
    // Another project's conversation is not read.
    o = parsed(run_tool(MEET_TOOL_READ_CONVERSATION, "{\"session_id\":\"s3\"}", answers, 2));
    CHECK(strstr(json_str(json_get(o, "error")), "not one of this project's") != NULL);
    json_free(o);
}

static const char BOARD[] = "{\"pulls\":["
    "{\"number\":12,\"title\":\"Fix login\",\"labels\":[{\"name\":\"Code-Approved\"}],\"checks\":\"success\",\"mergeable\":\"mergeable\"},"
    "{\"number\":13,\"title\":\"Dark mode\",\"labels\":[],\"checks\":\"failure\",\"mergeable\":\"conflicting\",\"draft\":true}],"
    "\"issues\":["
    "{\"number\":7,\"title\":\"Dark mode\",\"labels\":[{\"name\":\"ui\"}],\"pulls\":[{\"number\":13,\"title\":\"Dark mode\"}]},"
    "{\"number\":8,\"title\":\"Epic\",\"subIssues\":{\"total\":4,\"completed\":1}}]}";

static void test_pull_requests_say_whether_they_are_ready_to_merge(void) {
    const char *answers[] = { BOARD, SESSIONS };
    Json *o = parsed(run_tool(MEET_TOOL_LIST_PULL_REQUESTS, "{}", answers, 2));
    const Json *list = json_get(o, "pull_requests");
    CHECK_INT(json_int_or(json_get(o, "total"), 0), 2);
    CHECK(json_bool_is(json_get(json_at(list, 0), "ready_to_merge"), true));
    CHECK_STR(json_str(json_get(json_at(json_get(json_at(list, 0), "conversations"), 0), "session_id")), "s1");
    CHECK(json_bool_is(json_get(json_at(list, 1), "ready_to_merge"), false));
    CHECK_STR(json_str(json_get(json_at(list, 1), "state")), "draft, has conflicts, checks failure");
    json_free(o);
    // Without the conversations, the pull requests still come.
    const char *alone[] = { BOARD, NULL };
    o = parsed(run_tool(MEET_TOOL_LIST_PULL_REQUESTS, "{}", alone, 2));
    CHECK_INT(json_int_or(json_get(o, "total"), 0), 2);
    json_free(o);
    const char *none[] = { NULL };
    o = parsed(run_tool(MEET_TOOL_LIST_PULL_REQUESTS, "{}", none, 1));
    CHECK(json_str(json_get(o, "error")) != NULL);
    json_free(o);
}

static void test_a_pull_request_is_read_with_its_diffs(void) {
    const char *answers[] = { "{\"pr\":{\"title\":\"Fix login\",\"changedFiles\":2,\"additions\":10,\"deletions\":3},\"files\":["
        "{\"filename\":\"app/login.c\",\"status\":\"modified\",\"patch\":\"@@ -1 +1 @@\\n-old\\n+new\"},{\"filename\":\"logo.png\",\"status\":\"added\"}]}",
        "{\"pr\":{\"body\":\"## Why\\nThe **login** failed.\"}}" };
    Json *o = parsed(run_tool(MEET_TOOL_READ_PULL_REQUEST, "{\"number\":12}", answers, 2));
    CHECK_INT(json_int_or(json_get(o, "changed_files"), 0), 2);
    CHECK_INT(json_int_or(json_get(o, "lines_added"), 0), 10);
    CHECK_STR(json_str(json_get(o, "description")), "Why The login failed.");
    CHECK_STR(json_str(json_get(json_at(json_get(o, "files"), 0), "diff")), "@@ -1 +1 @@\n-old\n+new");
    CHECK(strstr(json_str(json_get(json_at(json_get(o, "files"), 1), "diff")), "binary") != NULL);
    json_free(o);
}

static void test_findings_say_their_verdicts(void) {
    const char *answers[] = { "{\"findings\":[{\"key\":\"a\",\"title\":\"Null check\",\"severity\":\"high\",\"file\":\"app/x.c\",\"line\":4,\"decision\":\"fix\",\"fixed\":true},"
        "{\"key\":\"b\",\"title\":\"Naming\",\"body\":\"Rename it\",\"decision\":\"fix\"},{\"key\":\"c\",\"title\":\"Style\"}]}" };
    Json *o = parsed(run_tool(MEET_TOOL_LIST_FINDINGS, "{\"number\":12}", answers, 1));
    const Json *list = json_get(o, "findings");
    CHECK_STR(json_str(json_get(json_at(list, 0), "place")), "app/x.c line 4");
    CHECK_STR(json_str(json_get(json_at(list, 0), "verdict")), "fix it");
    CHECK_STR(json_str(json_get(json_at(list, 1), "says")), "Rename it");
    CHECK_STR(json_str(json_get(json_at(list, 2), "verdict")), "not decided");
    CHECK_INT(json_int_or(json_get(o, "to_fix"), 0), 2);
    CHECK_INT(json_int_or(json_get(o, "to_fix_not_fixed"), 0), 1);
    json_free(o);
}

static void test_issues_are_listed_and_read_with_their_comments(void) {
    const char *board[] = { BOARD, SESSIONS };
    Json *o = parsed(run_tool(MEET_TOOL_LIST_ISSUES, "{}", board, 2));
    const Json *list = json_get(o, "issues");
    CHECK_INT(json_int_or(json_get(o, "total"), 0), 2);
    CHECK_INT(json_int_or(json_at(json_get(json_at(list, 0), "pull_requests"), 0), 0), 13);
    CHECK_STR(json_str(json_get(json_at(json_get(json_at(list, 0), "conversations"), 0), "session_id")), "s2");
    CHECK_STR(json_str(json_get(json_at(list, 1), "sub_issues")), "1 of 4 done");
    json_free(o);
    Str timeline; str_init(&timeline);
    str_appendz(&timeline, "{\"events\":[");
    for (int i = 1; i <= 7; i++) str_appendf(&timeline, "%s{\"kind\":\"commented\",\"actor\":\"dev%d\",\"body\":\"Comment %d\"}", i > 1 ? "," : "", i, i);
    str_appendz(&timeline, ",{\"kind\":\"labeled\"}]}");
    const char *answers[] = { "{\"issue\":{\"number\":7,\"title\":\"Dark mode\",\"state\":\"closed\",\"stateReason\":\"not_planned\",\"body\":\"Add a *dark* theme.\",\"comments\":7}}", timeline.data, SESSIONS };
    o = parsed(run_tool(MEET_TOOL_READ_ISSUE, "{\"issue\":7}", answers, 3));
    CHECK_STR(json_str(json_get(o, "state")), "closed as not planned");
    CHECK_STR(json_str(json_get(o, "description")), "Add a dark theme.");
    const Json *comments = json_get(o, "comments");
    CHECK_INT((int)json_count(comments), 5);
    CHECK_STR(json_str(json_get(json_at(comments, 0), "from")), "dev3");
    CHECK_STR(json_str(json_get(json_at(comments, 4), "text")), "Comment 7");
    CHECK_INT(json_int_or(json_get(o, "comments_total"), 0), 7);
    CHECK_STR(json_str(json_get(json_at(json_get(o, "conversations"), 0), "session_id")), "s2");
    json_free(o);
    str_free(&timeline);
}

void meet_tests(void) {
    test_run("realtime offers the project tools and answers only when asked unless independent", test_realtime_offers_the_project_tools_and_answers_only_when_asked_unless_independent);
    test_run("an elevenlabs voice makes realtime answer in text as the user", test_an_elevenlabs_voice_makes_realtime_answer_in_text_as_the_user);
    test_run("a greeting goes only when asked for", test_a_greeting_goes_only_when_asked_for);
    test_run("base64 encodes every tail length", test_base64_encodes_every_tail_length);
    test_run("audio and tool answers go out as realtime names them", test_audio_and_tool_answers_go_out_as_realtime_names_them);
    test_run("realtime events are read", test_realtime_events_are_read);
    test_run("costs follow each model's rates", test_costs_follow_each_models_rates);
    test_run("records round trip and add up by model", test_records_round_trip_and_add_up_by_model);
    test_run("the log keeps speakers on their own lines and its tail", test_the_log_keeps_speakers_on_their_own_lines_and_its_tail);
    test_run("wake words match whole words and only an addressed assistant is asked", test_wake_words_match_whole_words_and_only_an_addressed_assistant_is_asked);
    test_run("replies are made fit to say", test_replies_are_made_fit_to_say);
    test_run("elevenlabs speaks v4 turbo pcm at 24k", test_elevenlabs_speaks_v4_turbo_pcm_at_24k);
    test_run("models are named", test_models_are_named);
    test_run("each tool reads the project only", test_each_tool_reads_the_project_only);
    test_run("conversations are listed with their pull requests", test_conversations_are_listed_with_their_pull_requests);
    test_run("a conversation is read with its open question", test_a_conversation_is_read_with_its_open_question);
    test_run("pull requests say whether they are ready to merge", test_pull_requests_say_whether_they_are_ready_to_merge);
    test_run("a pull request is read with its diffs", test_a_pull_request_is_read_with_its_diffs);
    test_run("findings say their verdicts", test_findings_say_their_verdicts);
    test_run("issues are listed and read with their comments", test_issues_are_listed_and_read_with_their_comments);
}
