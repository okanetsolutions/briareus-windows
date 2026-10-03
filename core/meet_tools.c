#include "meet_tools.h"
#include "board.h"
#include "meet.h"
#include "models.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

// MARK: - Definitions

typedef struct { const char *name, *type, *about; } Param;
static const struct { const char *name, *description; Param params[2]; } TOOLS[MEET_TOOL_COUNT] = {
    [MEET_TOOL_LIST_CONVERSATIONS] = { "list_conversations",
        "The project's conversations (coding agents' sessions), newest first, with their status, whether the agent asks a "
        "question, and their pull request with its state (open, merged or closed) and checks.",
        { { "active_only", "boolean", "Only the conversations still open." } } },
    [MEET_TOOL_READ_CONVERSATION] = { "read_conversation",
        "A conversation's status and its latest messages: what was asked, what the agent said, and an open question.",
        { { "session_id", "string", "The conversation's id, from list_conversations." } } },
    [MEET_TOOL_LIST_PULL_REQUESTS] = { "list_pull_requests",
        "The project's open pull requests with their checks, conflicts, labels and review state, whether each is ready to "
        "merge, and the conversations working on it.", { { 0 } } },
    [MEET_TOOL_READ_PULL_REQUEST] = { "read_pull_request",
        "What one of the project's pull requests changes: how many files, lines added and removed, its description, and "
        "each changed file's name with its diff, to summarize what it touches.",
        { { "number", "integer", "The pull request's number." } } },
    [MEET_TOOL_LIST_FINDINGS] = { "list_findings",
        "The review findings left on one of the project's pull requests: each one's severity, place, what it says, the "
        "verdict given it (fix, dismissed or optional) and whether it is fixed.",
        { { "number", "integer", "The pull request's number." } } },
    [MEET_TOOL_LIST_ISSUES] = { "list_issues",
        "The project's open issues with their labels, epic progress, the pull requests that close them and the "
        "conversations started on them.", { { 0 } } },
    [MEET_TOOL_READ_ISSUE] = { "read_issue",
        "One of the project's issues in full: its state, labels, description and latest comments, its epic or sub-issues, "
        "the pull requests that close it and the conversations started on it.",
        { { "issue", "integer", "The issue's number." } } },
};

const char *meet_tool_name(MeetTool tool) { return TOOLS[tool].name; }
bool meet_tool_find(const char *name, MeetTool *out) {
    for (int t = 0; t < MEET_TOOL_COUNT; t++) if (str_eq(name, TOOLS[t].name)) { *out = (MeetTool)t; return true; }
    return false;
}

Json *meet_tools_json(void) {
    Json *tools = json_array();
    for (int t = 0; t < MEET_TOOL_COUNT; t++) {
        Json *tool = json_object(), *params = json_object(), *props = json_object(), *required = json_array();
        json_set_str(tool, "type", "function");
        json_set_str(tool, "name", TOOLS[t].name);
        json_set_str(tool, "description", TOOLS[t].description);
        for (size_t i = 0; i < 2 && TOOLS[t].params[i].name; i++) {
            const Param *p = &TOOLS[t].params[i];
            Json *prop = json_object();
            json_set_str(prop, "type", p->type);
            json_set_str(prop, "description", p->about);
            json_object_set(props, p->name, prop);
            // Every parameter but a filter is needed.
            if (!str_eq(p->type, "boolean")) json_array_push(required, json_string(p->name));
        }
        json_set_str(params, "type", "object");
        json_object_set(params, "properties", props);
        json_object_set(params, "required", required);
        json_set_bool(params, "additionalProperties", false);
        json_object_set(tool, "parameters", params);
        json_array_push(tools, tool);
    }
    return tools;
}

char *meet_tools_instructions(const char *project) {
    return xstrfmt(
        "## The project\n"
        "Coding agents work on %s in conversations (sessions). Every tool reads this project only; there is no way to "
        "reach another, and no tool changes anything: you can look things up, never start, message, merge or close. If "
        "asked to do something, say it has to be done in Briareus. Transcripts can contain mistakes and unfinished "
        "phrases; use the latest context, and ask when a needed detail is unclear instead of guessing.\n\n"
        "## Tools\n"
        "Find conversations with list_conversations before reading one; never invent an id, and match what people name "
        "against titles loosely. Each conversation carries its pull_request with its state (open, merged or closed) and "
        "checks, and each open pull request names the conversations working on it. list_pull_requests lists open pull "
        "requests only; one missing from it was merged or closed. read_conversation tells what an agent did, said or "
        "asks. For what a pull request changes, use read_pull_request with its number and summarize its description and "
        "diffs in plain words: what it touches and why, never the code itself. list_findings reads a pull request's "
        "review findings. list_issues and read_issue read the project's issues.\n\n"
        "## Ready to merge\n"
        "A pull request is ready to merge only when list_pull_requests marks it ready_to_merge: the code-approved label, "
        "checks passed, no conflicts, not a draft, and not stacked on another pull request. Otherwise say what it lacks.\n\n"
        "## Saying the result\n"
        "Say the relevant facts in a few plain sentences, without Markdown, ids or URLs, and only what the tools say.",
        project);
}

// MARK: - Calls

static Json *repo_args(const char *repo) { Json *a = json_object(); json_set_str(a, "repo", repo); return a; }
static int positive(const Json *args, const char *key) {
    double n;
    return json_num(json_get(args, key), &n) && n >= 1 && n < 1e9 ? (int)n : 0;
}

size_t meet_tool_calls(MeetTool tool, const Json *args, const char *repo, MeetCall out[MEET_TOOL_MAX_CALLS], char **refusal) {
    *refusal = NULL;
    size_t n = 0;
    Json *a;
    switch (tool) {
    case MEET_TOOL_LIST_CONVERSATIONS:
        out[n++] = (MeetCall){ "sessions", repo_args(repo) };
        break;
    case MEET_TOOL_READ_CONVERSATION: {
        const char *id = json_str_nonempty(json_get(args, "session_id"));
        if (!id) { *refusal = meet_tool_error("Say which conversation: its session_id, from list_conversations."); return 0; }
        // The list first, so a conversation of another project is not read.
        out[n++] = (MeetCall){ "sessions", repo_args(repo) };
        a = json_object(); json_set_str(a, "sessionId", id); json_set_num(a, "since", 0);
        out[n++] = (MeetCall){ "session", a };
        break;
    }
    case MEET_TOOL_LIST_PULL_REQUESTS:
    case MEET_TOOL_LIST_ISSUES:
        out[n++] = (MeetCall){ "pulls", repo_args(repo) };
        out[n++] = (MeetCall){ "sessions", repo_args(repo) };
        break;
    case MEET_TOOL_READ_PULL_REQUEST:
    case MEET_TOOL_LIST_FINDINGS: {
        int number = positive(args, "number");
        if (!number) { *refusal = meet_tool_error("Say which pull request: its number."); return 0; }
        a = repo_args(repo); json_set_num(a, "pr", number);
        out[n++] = (MeetCall){ tool == MEET_TOOL_LIST_FINDINGS ? "findings" : "pull_files", a };
        if (tool == MEET_TOOL_READ_PULL_REQUEST) { a = repo_args(repo); json_set_num(a, "pr", number); out[n++] = (MeetCall){ "pull_description", a }; }
        break;
    }
    case MEET_TOOL_READ_ISSUE: {
        int number = positive(args, "issue");
        if (!number) { *refusal = meet_tool_error("Say which issue: its number."); return 0; }
        a = repo_args(repo); json_set_num(a, "issue", number);
        out[n++] = (MeetCall){ "issue", a };
        a = repo_args(repo); json_set_num(a, "issue", number); json_set_num(a, "page", 1);
        out[n++] = (MeetCall){ "issue_timeline", a };
        out[n++] = (MeetCall){ "sessions", repo_args(repo) };
        break;
    }
    default: *refusal = meet_tool_error("No such tool."); return 0;
    }
    return n;
}
void meet_calls_free(MeetCall *calls, size_t count) { for (size_t i = 0; i < count; i++) { json_free(calls[i].args); calls[i].args = NULL; } }

// MARK: - Summaries

static char *serialize_free(Json *j) { char *t = json_serialize(j, false); json_free(j); return t; }
char *meet_tool_error(const char *why) { Json *o = json_object(); json_set_str(o, "error", why); return serialize_free(o); }

/// `text` on one line, Markdown dropped, cut to about `max` bytes at a sentence or a word.
static char *inline_cut(const char *text, size_t max) {
    char *all = meet_spoken(text ? text : "", (size_t)-1 / 2);
    if (strlen(all) <= max) return all;
    free(all);
    char *cut = meet_spoken(text, max), *more = xstrfmt("%s\xE2\x80\xA6", cut);
    free(cut);
    return more;
}
static void set_cut(Json *o, const char *key, const char *text, size_t max) { char *c = inline_cut(text, max); json_set_str(o, key, c); free(c); }

/// The project's conversations, newest first as the server lists them.
static Session *project_sessions(const Json *answer, const char *repo, size_t *count) {
    Session *all = NULL; size_t n = 0;
    *count = 0;
    if (!sessions_parse(answer, &all, &n)) return NULL;
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        const char *r = session_repo(&all[i]);
        if (!r || str_eq(r, repo)) { if (kept != i) all[kept] = all[i]; kept++; }
        else session_free(&all[i]);
    }
    *count = kept;
    return all;
}

static char *checks_text(const Json *checks) {
    double total = json_num_or(json_get(checks, "total"), 0), passed = json_num_or(json_get(checks, "passed"), 0);
    double failed = json_num_or(json_get(checks, "failed"), 0), pending = json_num_or(json_get(checks, "pending"), 0);
    if (total <= 0) return xstrdup("no checks");
    if (failed > 0) return xstrfmt("%d of %d failed", (int)failed, (int)total);
    if (pending > 0) return xstrfmt("%d of %d still running", (int)pending, (int)total);
    return xstrfmt("all %d passed", (int)passed);
}

/// The open question an agent asks: its last event, when that is a question no one answered yet.
static const Event *open_question(const Event *events, size_t count) {
    for (size_t i = count; i-- > 0; ) {
        const Event *e = &events[i];
        if (str_eq(e->kind, "ask")) return e->question ? e : NULL;
        if (str_eq(e->kind, "user") || str_eq(e->kind, "text") || str_eq(e->kind, "result")) return NULL;
    }
    return NULL;
}

static Json *conversation(const Session *s, const Event *events, size_t count) {
    Json *o = json_object();
    const Event *asking = open_question(events, count);
    json_set_str(o, "session_id", session_id(s));
    set_cut(o, "title", session_display_title(s), 200);
    json_set_str(o, "status", asking ? "waiting for an answer" : session_status(s) ? session_status(s) : "unknown");
    int number = session_pull_number(s);
    if (number) {
        Json *pr = json_object();
        const Json *st = json_get(s->raw, "prStatus");
        json_set_num(pr, "number", number);
        if (json_int_or(json_get(st, "number"), 0) == number) {
            if (json_str_nonempty(json_get(st, "state"))) json_set_str(pr, "state", json_str(json_get(st, "state")));
            if (json_str_nonempty(json_get(st, "title"))) set_cut(pr, "title", json_str(json_get(st, "title")), 200);
            if (json_bool_is(json_get(st, "draft"), true)) json_set_bool(pr, "draft", true);
            if (json_is_object(json_get(st, "checks"))) { char *c = checks_text(json_get(st, "checks")); json_set_str(pr, "checks", c); free(c); }
        }
        json_object_set(o, "pull_request", pr);
    }
    if (asking) {
        set_cut(o, "question", asking->question, 600);
        Json *options = json_array();
        for (size_t i = 0; i < json_count(asking->options); i++) {
            const char *label = json_str_nonempty(json_get(json_at(asking->options, i), "label"));
            if (label) json_array_push(options, json_string(label));
        }
        if (json_count(options)) json_object_set(o, "options", options); else json_free(options);
    }
    return o;
}

static Json *conversations_on(const Session *sessions, size_t count, int pull, int issue) {
    Json *list = json_array();
    for (size_t i = 0; i < count; i++) {
        if (pull ? session_pull_number(&sessions[i]) != pull : !session_on_issue(&sessions[i], issue)) continue;
        Json *c = json_object();
        json_set_str(c, "session_id", session_id(&sessions[i]));
        set_cut(c, "title", session_display_title(&sessions[i]), 200);
        if (session_status(&sessions[i])) json_set_str(c, "status", session_status(&sessions[i]));
        json_array_push(list, c);
    }
    return list;
}

static Json *label_names(const PullLabel *labels, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) if (labels[i].name) json_array_push(a, json_string(labels[i].name));
    return a;
}

static Json *list_conversations(const Json *args, const char *repo, const Json *answer) {
    size_t n; Session *sessions = project_sessions(answer, repo, &n);
    bool active = json_bool_is(json_get(args, "active_only"), true);
    Json *out = json_object(), *list = json_array();
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        const char *st = session_status(&sessions[i]);
        if (active && (str_eq(st, "closed") || str_eq(st, "failed") || str_eq(st, "error"))) continue;
        if (total++ < 15) json_array_push(list, conversation(&sessions[i], NULL, 0));
    }
    json_object_set(out, "conversations", list);
    json_set_num(out, "total", (double)total);
    sessions_free(sessions, n);
    return out;
}

static Json *read_conversation(const Json *args, const char *repo, const Json *list, const Json *answer) {
    const char *id = json_str(json_get(args, "session_id"));
    size_t n; Session *sessions = project_sessions(list, repo, &n);
    bool mine = false;
    for (size_t i = 0; i < n && !mine; i++) mine = str_eq(session_id(&sessions[i]), id);
    sessions_free(sessions, n);
    Json *out = NULL;
    Session s;
    if (!mine) { out = json_object(); json_set_str(out, "error", "That conversation is not one of this project's."); return out; }
    if (!answer || !session_parse(json_get(answer, "session"), &s)) { out = json_object(); json_set_str(out, "error", "The server did not return the conversation."); return out; }
    Transcript t; transcript_init(&t);
    transcript_append(&t, json_get(answer, "events"));
    out = conversation(&s, t.events, t.count);
    // The last few things said, oldest first.
    Json *latest = json_array();
    size_t said = 0, from = t.count;
    while (from > 0 && said < 6) {
        const Event *e = &t.events[--from];
        if ((str_eq(e->kind, "user") || str_eq(e->kind, "text") || str_eq(e->kind, "result") || str_eq(e->kind, "ask")) && (e->question || (e->text && *e->text))) said++;
    }
    for (size_t i = from; i < t.count; i++) {
        const Event *e = &t.events[i];
        const char *text = e->question ? e->question : e->text;
        if (!(str_eq(e->kind, "user") || str_eq(e->kind, "text") || str_eq(e->kind, "result") || str_eq(e->kind, "ask")) || !text || !*text) continue;
        Json *line = json_object();
        json_set_str(line, "from", str_eq(e->kind, "user") ? "user" : "agent");
        set_cut(line, "text", text, 600);
        json_array_push(latest, line);
    }
    json_object_set(out, "latest", latest);
    transcript_free(&t);
    session_free(&s);
    return out;
}

static bool has_label(const PullSummary *pr, const char *name) {
    for (size_t i = 0; i < pr->label_count; i++) { char *a = str_fold(pr->labels[i].name), *b = str_fold(name); bool same = str_eq(a, b); free(a); free(b); if (same) return true; }
    return false;
}

static Json *list_pull_requests(const char *repo, const Json *board, const Json *sessions_answer) {
    size_t sn = 0; Session *sessions = sessions_answer ? project_sessions(sessions_answer, repo, &sn) : NULL;
    size_t n; PullSummary *pulls = pull_summaries_parse(json_get(board, "pulls"), &n);
    Json *out = json_object(), *list = json_array();
    for (size_t i = 0; i < n && i < 15; i++) {
        const PullSummary *pr = &pulls[i];
        Json *o = json_object();
        json_set_num(o, "number", pr->number);
        set_cut(o, "title", pr->title, 200);
        Str state; str_init(&state);
        str_appendz(&state, pr->draft ? "draft" : "open");
        if (pull_has_conflicts(pr)) str_appendz(&state, ", has conflicts");
        if (pr->checks) str_appendf(&state, ", checks %s", pr->checks);
        if (pr->review_decision) str_appendf(&state, ", review %s", pr->review_decision);
        json_set_str(o, "state", state.data);
        str_free(&state);
        json_object_set(o, "labels", label_names(pr->labels, pr->label_count));
        StackPosition stack; bool stacked = stack_position_parse(json_get(pr->raw, "stack"), json_get(board, "stacks"), &stack);
        int position = stacked && stack.total > 1 ? stack.position : 1;
        json_set_bool(o, "ready_to_merge", has_label(pr, "code-approved") && str_eq(pr->checks, "success") && !pull_has_conflicts(pr) && !pr->draft && position == 1);
        if (stacked && stack.total > 1) {
            char *said = position > 1 ? xstrfmt("Position %d of a stack of %d%s: the pull requests under it merge first.", position, stack.total, stack.partial ? " or more" : "")
                                      : xstrfmt("Bottom of a stack of %d%s: it merges first.", stack.total, stack.partial ? " or more" : "");
            json_set_str(o, "stack", said);
            free(said);
        }
        if (stacked) stack_position_free(&stack);
        json_object_set(o, "conversations", conversations_on(sessions, sn, pr->number, 0));
        json_array_push(list, o);
    }
    json_object_set(out, "pull_requests", list);
    json_set_num(out, "total", (double)n);
    pull_summaries_free(pulls, n);
    sessions_free(sessions, sn);
    return out;
}

static Json *read_pull_request(const Json *files, const Json *description) {
    PullFilesPage page;
    if (!files || !pull_files_page_parse(files, &page)) { Json *e = json_object(); json_set_str(e, "error", "The server did not return the pull request's files."); return e; }
    Json *out = json_object();
    const Json *pr = page.pr;
    json_set_num(out, "changed_files", json_num_or(json_get(pr, "changedFiles"), (double)page.file_count));
    double v;
    if (json_num(json_get(pr, "additions"), &v)) json_set_num(out, "lines_added", v);
    if (json_num(json_get(pr, "deletions"), &v)) json_set_num(out, "lines_removed", v);
    if (json_num(json_get(pr, "commits"), &v)) json_set_num(out, "commits", v);
    if (json_str_nonempty(json_get(pr, "title"))) set_cut(out, "title", json_str(json_get(pr, "title")), 200);
    const char *body = json_str_nonempty(json_get(json_get(description, "pr"), "body"));
    if (!body) body = json_str_nonempty(json_get(pr, "body"));
    if (body) set_cut(out, "description", body, 6000);
    // Each file's diff, cut to 4,000 bytes and 60,000 in all; past that only the names.
    long left = 60000;
    Json *list = json_array();
    for (size_t i = 0; i < page.file_count; i++) {
        const PullFile *f = &page.files[i];
        Json *o = json_object();
        json_set_str(o, "file", f->filename ? f->filename : "");
        if (!f->patch || !*f->patch) json_set_str(o, "diff", "No diff: a binary file, or one too large for GitHub to show.");
        else if (left > 0) {
            size_t max = left < 4000 ? (size_t)left : 4000, len = strlen(f->patch);
            if (len > max) { while (max && ((unsigned char)f->patch[max] & 0xC0) == 0x80) max--; }
            char *shown = len > max ? xstrfmt("%.*s\xE2\x80\xA6", (int)max, f->patch) : xstrdup(f->patch);
            left -= (long)(len > max ? max : len);
            json_set_str(o, "diff", shown);
            free(shown);
        }
        json_array_push(list, o);
    }
    json_object_set(out, "files", list);
    if (left <= 0) json_set_str(out, "diffs", "Cut short: the later files are listed by name only.");
    if (page.next_page || page.truncated) { char *t = xstrfmt("the first %zu only", page.file_count); json_set_str(out, "files_listed", t); free(t); }
    pull_files_page_free(&page);
    return out;
}

static Json *list_findings(const Json *answer) {
    const Json *findings = json_get(answer, "findings");
    Json *out = json_object(), *list = json_array();
    int to_fix = 0, not_fixed = 0;
    for (size_t i = 0; i < json_count(findings); i++) {
        const Json *f = json_at(findings, i);
        Json *o = json_object();
        set_cut(o, "title", json_str(json_get(f, "title")), 200);
        if (json_str_nonempty(json_get(f, "severity"))) json_set_str(o, "severity", json_str(json_get(f, "severity")));
        const char *file = json_str_nonempty(json_get(f, "file"));
        if (file) { int line = json_int_or(json_get(f, "line"), 0); char *place = line > 0 ? xstrfmt("%s line %d", file, line) : xstrdup(file); json_set_str(o, "place", place); free(place); }
        const char *says = json_str_nonempty(json_get(f, "body"));
        if (!says) says = json_str_nonempty(json_get(f, "detail"));
        if (!says) says = json_str_nonempty(json_get(f, "description"));
        if (says) set_cut(o, "says", says, 500);
        const char *decision = json_str(json_get(f, "decision"));
        json_set_str(o, "verdict", str_eq(decision, "fix") ? "fix it" : str_eq(decision, "dismissed") ? "dismissed" : str_eq(decision, "optional") ? "optional" : "not decided");
        bool fixed = json_bool_is(json_get(f, "fixed"), true);
        if (fixed) json_set_bool(o, "fixed", true);
        if (str_eq(decision, "fix")) { to_fix++; if (!fixed) not_fixed++; }
        json_array_push(list, o);
    }
    json_object_set(out, "findings", list);
    json_set_num(out, "to_fix", to_fix);
    json_set_num(out, "to_fix_not_fixed", not_fixed);
    return out;
}

static Json *issue_links(const BoardLink *links, size_t count) {
    Json *a = json_array();
    for (size_t i = 0; i < count; i++) {
        Json *o = json_object();
        json_set_num(o, "number", links[i].number);
        set_cut(o, "title", links[i].title, 200);
        if (links[i].state) json_set_str(o, "state", links[i].state);
        if (links[i].draft) json_set_bool(o, "draft", true);
        json_array_push(a, o);
    }
    return a;
}

static Json *list_issues(const char *repo, const Json *board, const Json *sessions_answer) {
    size_t sn = 0; Session *sessions = sessions_answer ? project_sessions(sessions_answer, repo, &sn) : NULL;
    size_t n; IssueSummary *issues = issue_summaries_parse(json_get(board, "issues"), &n);
    Json *out = json_object(), *list = json_array();
    for (size_t i = 0; i < n && i < 20; i++) {
        const IssueSummary *is = &issues[i];
        Json *o = json_object();
        json_set_num(o, "number", is->number);
        set_cut(o, "title", is->title, 200);
        json_object_set(o, "labels", label_names(is->labels, is->label_count));
        Json *prs = json_array();
        for (size_t p = 0; p < is->pull_count; p++) json_array_push(prs, json_number(is->pulls[p].number));
        json_object_set(o, "pull_requests", prs);
        json_object_set(o, "conversations", conversations_on(sessions, sn, 0, is->number));
        if (issue_is_epic(is)) { char *t = xstrfmt("%d of %d done", is->sub_issues_done, is->sub_issues); json_set_str(o, "sub_issues", t); free(t); }
        if (is->has_parent) json_set_num(o, "epic", is->parent.number);
        json_array_push(list, o);
    }
    json_object_set(out, "issues", list);
    json_set_num(out, "total", (double)n);
    issue_summaries_free(issues, n);
    sessions_free(sessions, sn);
    return out;
}

static const char *issue_state(const Json *raw) {
    const char *state = json_str(json_get(raw, "state"));
    if (!str_eq(state, "closed")) return state ? state : "open";
    const char *why = json_str(json_get(raw, "stateReason"));
    return str_eq(why, "not_planned") ? "closed as not planned" : str_eq(why, "duplicate") ? "closed as a duplicate"
         : str_eq(why, "completed") ? "closed as completed" : "closed";
}

static Json *read_issue(const char *repo, const Json *answer, const Json *timeline, const Json *sessions_answer) {
    const Json *raw = json_get(answer, "issue");
    IssueSummary is;
    if (!answer || !issue_summary_parse(raw, &is)) { Json *e = json_object(); json_set_str(e, "error", "The server did not return the issue."); return e; }
    size_t sn = 0; Session *sessions = sessions_answer ? project_sessions(sessions_answer, repo, &sn) : NULL;
    Json *out = json_object();
    json_set_num(out, "number", is.number);
    set_cut(out, "title", is.title, 200);
    json_set_str(out, "state", issue_state(raw));
    if (json_str_nonempty(json_get(raw, "type"))) json_set_str(out, "type", json_str(json_get(raw, "type")));
    json_object_set(out, "labels", label_names(is.labels, is.label_count));
    if (is.author) json_set_str(out, "author", is.author);
    set_cut(out, "description", json_str(json_get(raw, "body")), 4000);
    json_object_set(out, "pull_requests", issue_links(is.pulls, is.pull_count));
    json_object_set(out, "conversations", conversations_on(sessions, sn, 0, is.number));
    if (is.has_parent) { Json *epic = json_object(); json_set_num(epic, "number", is.parent.number); set_cut(epic, "title", is.parent.title, 200); json_object_set(out, "epic", epic); }
    if (issue_is_epic(&is)) {
        char *t = xstrfmt("%d of %d done", is.sub_issues_done, is.sub_issues); json_set_str(out, "sub_issues", t); free(t);
        const Json *items = json_get(json_get(raw, "subIssues"), "items");
        Json *open = json_array();
        for (size_t i = 0; i < json_count(items) && json_count(open) < 10; i++) {
            BoardLink l;
            if (!board_link_parse(json_at(items, i), &l)) continue;
            if (str_eq(l.state, "open")) { Json *o = json_object(); json_set_num(o, "number", l.number); set_cut(o, "title", l.title, 200); json_array_push(open, o); }
            board_link_free(&l);
        }
        json_object_set(out, "open_sub_issues", open);
    }
    // The last five comments of the first page of the timeline.
    const Json *events = json_get(timeline, "events");
    size_t comments = 0;
    for (size_t i = 0; i < json_count(events); i++) if (str_eq(json_str(json_get(json_at(events, i), "kind")), "commented") && json_str_nonempty(json_get(json_at(events, i), "body"))) comments++;
    Json *said = json_array();
    for (size_t i = 0, seen = 0; i < json_count(events); i++) {
        const Json *c = json_at(events, i);
        if (!str_eq(json_str(json_get(c, "kind")), "commented") || !json_str_nonempty(json_get(c, "body"))) continue;
        if (++seen + 5 <= comments) continue;
        Json *o = json_object();
        json_set_str(o, "from", json_str_or(json_get(c, "actor"), "a deleted account"));
        set_cut(o, "text", json_str(json_get(c, "body")), 800);
        json_array_push(said, o);
    }
    json_object_set(out, "comments", said);
    json_set_num(out, "comments_total", is.comments);
    issue_summary_free(&is);
    sessions_free(sessions, sn);
    return out;
}

char *meet_tool_summary(MeetTool tool, const Json *args, const char *repo, const Json *const *answers, size_t count) {
    const Json *a0 = count > 0 ? answers[0] : NULL, *a1 = count > 1 ? answers[1] : NULL, *a2 = count > 2 ? answers[2] : NULL;
    if (!a0) return meet_tool_error("The server could not be read.");
    Json *out;
    switch (tool) {
    case MEET_TOOL_LIST_CONVERSATIONS: out = list_conversations(args, repo, a0); break;
    case MEET_TOOL_READ_CONVERSATION: out = read_conversation(args, repo, a0, a1); break;
    case MEET_TOOL_LIST_PULL_REQUESTS: out = list_pull_requests(repo, a0, a1); break;
    case MEET_TOOL_READ_PULL_REQUEST: out = read_pull_request(a0, a1); break;
    case MEET_TOOL_LIST_FINDINGS: out = list_findings(a0); break;
    case MEET_TOOL_LIST_ISSUES: out = list_issues(repo, a0, a1); break;
    case MEET_TOOL_READ_ISSUE: out = read_issue(repo, a0, a1, a2); break;
    default: return meet_tool_error("No such tool.");
    }
    return serialize_free(out);
}
