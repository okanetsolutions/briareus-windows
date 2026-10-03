// meet.c and meet_tools.c: the meeting assistant's ElevenLabs agent and its events, its project tools, its transcript
// and the costs.
#include "meet.h"
#include "meet_tools.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static MeetPersona persona(bool independent) {
    MeetPersona p = { "Nadin", "Nadin, assistant", "Briareus (nadinyamaui/briareus)", "zozOsuFj6BSfLStTxdrK", true, independent };
    return p;
}
static Json *parsed(char *text) { Json *j = json_parsez(text); free(text); return j; }
static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_the_agent_speaks_in_the_users_voice_with_the_project_tools(void) {
    MeetPersona p = persona(false);
    char *ids[] = { "tool_1", "tool_2" };
    Json *body = parsed(meet_agent_body(&p, ids, 2));
    CHECK_STR(json_str(json_get(body, "name")), "Briareus meeting assistant");
    const Json *config = json_get(body, "conversation_config"), *tts = json_get(config, "tts"), *agent = json_get(config, "agent"), *prompt = json_get(agent, "prompt");
    CHECK_STR(json_str(json_get(tts, "voice_id")), "zozOsuFj6BSfLStTxdrK");
    CHECK_STR(json_str(json_get(tts, "model_id")), "eleven_v4_turbo");
    CHECK_STR(json_str(json_get(tts, "agent_output_audio_format")), "pcm_24000");
    CHECK_STR(json_str(json_get(json_get(config, "asr"), "user_input_audio_format")), "pcm_24000");
    CHECK_INT(json_int_or(json_get(json_get(config, "conversation"), "max_duration_seconds"), 0), 7200);
    CHECK_STR(json_str(json_get(agent, "language")), "en");
    CHECK(strstr(json_str(json_get(agent, "first_message")), "Nadin's AI assistant") != NULL);
    CHECK_STR(json_str(json_get(prompt, "llm")), MEET_AGENT_LLM);
    CHECK_INT((int)json_count(json_get(prompt, "tool_ids")), 2);
    CHECK_STR(json_str(json_at(json_get(prompt, "tool_ids"), 1)), "tool_2");
    CHECK_STR(json_str(json_get(json_get(json_get(json_get(prompt, "built_in_tools"), "skip_turn"), "params"), "system_tool_type")), "skip_turn");
    const char *instructions = json_str(json_get(prompt, "prompt"));
    CHECK(strstr(instructions, "Speak as Nadin, in the first person") != NULL);
    CHECK(strstr(instructions, "Nadin, assistant") != NULL);
    CHECK(strstr(instructions, "call skip_turn") != NULL);
    CHECK(strstr(instructions, "Briareus (nadinyamaui/briareus)") != NULL);
    CHECK(strstr(instructions, "no tool changes anything") != NULL);
    json_free(body);
    p = persona(true);
    p.introduce = false;
    body = parsed(meet_agent_body(&p, NULL, 0));
    agent = json_get(json_get(body, "conversation_config"), "agent");
    CHECK_STR(json_str(json_get(agent, "first_message")), "");
    CHECK(strstr(json_str(json_get(json_get(agent, "prompt"), "prompt")), "Act independently on Nadin's behalf") != NULL);
    json_free(body);
    CHECK_OWNED_STR(meet_agent_ws_path("agent_01abc"), "/v1/convai/conversation?agent_id=agent_01abc");
    CHECK_OWNED_STR(meet_agent_ws_path("a&b=c"), "/v1/convai/conversation?agent_id=abc");
    Json *signed_url = json_parsez("{\"signed_url\":\"wss://api.elevenlabs.io/v1/convai/conversation?agent_id=a1&conversation_signature=s\"}");
    CHECK_OWNED_STR(meet_signed_ws_path(signed_url), "/v1/convai/conversation?agent_id=a1&conversation_signature=s");
    json_free(signed_url);
    signed_url = json_parsez("{\"signed_url\":\"wss://evil.example/x\"}");
    CHECK(meet_signed_ws_path(signed_url) == NULL);
    json_free(signed_url);
    Json *created = json_parsez("{\"agent_id\":\"agent_9\"}");
    CHECK_OWNED_STR(meet_created_id(created), "agent_9");
    json_free(created);
    created = json_parsez("{\"id\":\"tool_9\",\"tool_config\":{}}");
    CHECK_OWNED_STR(meet_created_id(created), "tool_9");
    json_free(created);
    CHECK(meet_created_id(NULL) == NULL);
}

static void test_tools_are_client_tools_that_wait_for_the_app(void) {
    Json *t = parsed(meet_tool_body(MEET_TOOL_READ_PULL_REQUEST));
    const Json *config = json_get(t, "tool_config");
    CHECK_STR(json_str(json_get(config, "type")), "client");
    CHECK_STR(json_str(json_get(config, "name")), "read_pull_request");
    CHECK(json_bool_is(json_get(config, "expects_response"), true));
    CHECK_STR(json_str(json_get(json_get(json_get(json_get(config, "parameters"), "properties"), "number"), "type")), "integer");
    CHECK_STR(json_str(json_at(json_get(json_get(config, "parameters"), "required"), 0)), "number");
    json_free(t);
    t = parsed(meet_tool_body(MEET_TOOL_LIST_CONVERSATIONS));
    CHECK_INT((int)json_count(json_get(json_get(json_get(t, "tool_config"), "parameters"), "required")), 0);
    json_free(t);
    for (int i = 0; i < MEET_TOOL_COUNT; i++) {
        const char *name = meet_tool_name((MeetTool)i);
        // Read-only: nothing that starts, messages, merges, closes or deletes.
        CHECK(!strstr(name, "start") && !strstr(name, "send") && !strstr(name, "merge") && !strstr(name, "delete") && !strstr(name, "close"));
    }
}

static void test_base64_encodes_every_tail_length(void) {
    CHECK_OWNED_STR(base64_encode("", 0), "");
    CHECK_OWNED_STR(base64_encode("f", 1), "Zg==");
    CHECK_OWNED_STR(base64_encode("fo", 2), "Zm8=");
    CHECK_OWNED_STR(base64_encode("foo", 3), "Zm9v");
    CHECK_OWNED_STR(base64_encode("foobar", 6), "Zm9vYmFy");
}
static void test_audio_pongs_and_tool_results_go_out_as_the_agent_names_them(void) {
    int16_t pcm[2] = { 1, -1 };
    CHECK_OWNED_STR(meet_audio_event(pcm, 2), "{\"user_audio_chunk\":\"AQD//w==\"}");
    CHECK_OWNED_STR(meet_pong_event(7), "{\"type\":\"pong\",\"event_id\":7}");
    CHECK_OWNED_STR(meet_start_event(), "{\"type\":\"conversation_initiation_client_data\"}");
    Json *e = parsed(meet_tool_result_event("call_9", "{\"total\":0}", false));
    CHECK_STR(json_str(json_get(e, "type")), "client_tool_result");
    CHECK_STR(json_str(json_get(e, "tool_call_id")), "call_9");
    CHECK_STR(json_str(json_get(e, "result")), "{\"total\":0}");
    CHECK(json_bool_is(json_get(e, "is_error"), false));
    json_free(e);
    e = parsed(meet_answer_now_event());
    CHECK_STR(json_str(json_get(e, "type")), "user_message");
    json_free(e);
}

static MeetEvent parse(const char *json) { MeetEvent e; CHECK(meet_event_parse(json, strlen(json), &e)); return e; }
static void test_agent_events_are_read(void) {
    MeetEvent e = parse("{\"type\":\"conversation_initiation_metadata\",\"conversation_initiation_metadata_event\":{\"conversation_id\":\"c\",\"agent_output_audio_format\":\"pcm_24000\",\"user_input_audio_format\":\"pcm_24000\"}}");
    CHECK_INT(e.kind, MEET_EV_READY); meet_event_free(&e);
    e = parse("{\"type\":\"audio\",\"audio_event\":{\"audio_base_64\":\"AQD//w==\",\"event_id\":3}}");
    CHECK_INT(e.kind, MEET_EV_AUDIO); CHECK_INT((int)e.audio_len, 4); meet_event_free(&e);
    e = parse("{\"type\":\"interruption\",\"interruption_event\":{\"event_id\":4}}");
    CHECK_INT(e.kind, MEET_EV_INTERRUPTED); meet_event_free(&e);
    e = parse("{\"type\":\"user_transcript\",\"user_transcription_event\":{\"user_transcript\":\"Nadin, any news?\",\"event_id\":5}}");
    CHECK_INT(e.kind, MEET_EV_HEARD_TURN); CHECK_STR(e.text, "Nadin, any news?"); meet_event_free(&e);
    e = parse("{\"type\":\"agent_response\",\"agent_response_event\":{\"agent_response\":\"CI is green.\",\"event_id\":6,\"response_id\":\"r\"}}");
    CHECK_INT(e.kind, MEET_EV_SAID); CHECK_STR(e.text, "CI is green."); meet_event_free(&e);
    e = parse("{\"type\":\"ping\",\"ping_event\":{\"event_id\":8,\"ping_ms\":40}}");
    CHECK_INT(e.kind, MEET_EV_PING); CHECK_INT(e.event_id, 8); meet_event_free(&e);
    e = parse("{\"type\":\"client_tool_call\",\"client_tool_call\":{\"tool_name\":\"read_pull_request\",\"tool_call_id\":\"call_1\",\"parameters\":{\"number\":12},\"event_id\":9,\"expects_response\":true}}");
    CHECK_INT(e.kind, MEET_EV_TOOL); CHECK_STR(e.id, "call_1"); CHECK_STR(e.text, "read_pull_request"); CHECK_STR(e.request, "{\"number\":12}"); meet_event_free(&e);
    e = parse("{\"type\":\"client_error\",\"error_event\":{\"code\":1008,\"error_name\":\"auth\",\"message\":\"Bad key\"}}");
    CHECK_INT(e.kind, MEET_EV_ERROR); CHECK_STR(e.text, "ElevenLabs: Bad key"); meet_event_free(&e);
    e = parse("{\"type\":\"vad_score\",\"vad_score_event\":{\"vad_score\":0.4}}");
    CHECK_INT(e.kind, MEET_EV_OTHER); meet_event_free(&e);
    MeetEvent bad;
    CHECK(!meet_event_parse("not json", 8, &bad));
}
static void test_costs_follow_each_models_rates(void) {
    MeetUsage agent = { .seconds = 90 };
    CHECK(near(meet_usage_cost(MEET_AGENT, &agent), 0.12));
    MeetUsage live = { .seconds = 90 };
    CHECK(near(meet_usage_cost(MEET_LIVE, &live), 0.075));
    MeetUsage rt = { .text_in = 1e6, .text_cached = 1e6, .text_out = 1e6, .audio_in = 1e6, .audio_cached = 1e6, .audio_out = 1e6, .transcribed_seconds = 60, .spoken_chars = 1000 };
    CHECK(near(meet_usage_cost(MEET_REALTIME, &rt), 0.6 + 0.06 + 2.4 + 10 + 0.3 + 20 + 0.0045 + 0.04));
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
    meet_log_line(&log, MEET_SPEAKER_LOOKUP, "list_pull_requests");
    meet_log_line(&log, MEET_SPEAKER_LOOKUP, "list_issues");
    meet_log_line(&log, MEET_SPEAKER_ASSISTANT, "Two are\nready.");
    meet_log_line(&log, MEET_SPEAKER_ASSISTANT, "One waits.");
    CHECK_OWNED_STR(meet_log_tail(&log, 1000), "Meeting: What is the status?\nAssistant: Let me check.\nLookup: list_pull_requests\n"
                                               "Lookup: list_issues\nAssistant: Two are ready.\nAssistant: One waits.");
    meet_log_free(&log); meet_log_init(&log);
    for (int i = 0; i < 2000; i++) { meet_log_add(&log, MEET_SPEAKER_MEETING, "and so on and so forth"); meet_log_add(&log, MEET_SPEAKER_ASSISTANT, "ok"); }
    CHECK(log.text.len <= 24000 + 40);
    CHECK(str_has_prefix(log.text.data, "Meeting: ") || str_has_prefix(log.text.data, "Assistant: "));
    meet_log_free(&log);
}
static void test_replies_are_made_fit_to_say(void) {
    CHECK_OWNED_STR(meet_spoken("# Status\n\n- **CI** is `green`\n- PR #12 merged\n```\ncode\n```\n> done", 1000), "Status CI is green PR #12 merged done");
    CHECK_OWNED_STR(meet_spoken("One. Two. Three is long", 12), "One. Two.");
    CHECK_OWNED_STR(meet_spoken("abcdef ghijkl", 9), "abcdef");
    CHECK_OWNED_STR(meet_spoken("abcdefghijkl", 5), "abcde");
    CHECK_OWNED_STR(meet_spoken(NULL, 5), "");
}
static void test_models_are_named(void) {
    CHECK_STR(meet_model_label(MEET_LIVE), "GPT-Live 1");
    CHECK_STR(meet_model_label(MEET_AGENT), "ElevenLabs agent");
    CHECK_STR(meet_model_id(MEET_AGENT), "elevenlabs-agent");
    MeetPersona nameless = { 0 };
    char *i = meet_instructions(&nameless);
    CHECK(strstr(i, "You speak for the user") != NULL && strstr(i, "the project this project") != NULL);
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
    test_run("the agent speaks in the user's voice with the project tools", test_the_agent_speaks_in_the_users_voice_with_the_project_tools);
    test_run("tools are client tools that wait for the app", test_tools_are_client_tools_that_wait_for_the_app);
    test_run("base64 encodes every tail length", test_base64_encodes_every_tail_length);
    test_run("audio, pongs and tool results go out as the agent names them", test_audio_pongs_and_tool_results_go_out_as_the_agent_names_them);
    test_run("agent events are read", test_agent_events_are_read);
    test_run("costs follow each model's rates", test_costs_follow_each_models_rates);
    test_run("records round trip and add up by model", test_records_round_trip_and_add_up_by_model);
    test_run("the log keeps speakers on their own lines and its tail", test_the_log_keeps_speakers_on_their_own_lines_and_its_tail);
    test_run("replies are made fit to say", test_replies_are_made_fit_to_say);
    test_run("models are named", test_models_are_named);
    test_run("each tool reads the project only", test_each_tool_reads_the_project_only);
    test_run("conversations are listed with their pull requests", test_conversations_are_listed_with_their_pull_requests);
    test_run("a conversation is read with its open question", test_a_conversation_is_read_with_its_open_question);
    test_run("pull requests say whether they are ready to merge", test_pull_requests_say_whether_they_are_ready_to_merge);
    test_run("a pull request is read with its diffs", test_a_pull_request_is_read_with_its_diffs);
    test_run("findings say their verdicts", test_findings_say_their_verdicts);
    test_run("issues are listed and read with their comments", test_issues_are_listed_and_read_with_their_comments);
}
