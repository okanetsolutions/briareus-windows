// The findings waiting for a decision across every project the device may see, as the dashboard's Findings screen
// queues them: each review round held on a conversation, grouped by the pull request it was left on. A round on the
// user's own pull request takes a verdict on every finding and is completed from here, which starts the fix session;
// a review of somebody else's is read, replied to on its findings' threads, and taken off the queue.
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ACT_OPEN_SESSION = 1000, ACT_OPEN_PR, ACT_OPEN_FINDING, ACT_VERDICT, ACT_COMMENT, ACT_REPLY, ACT_DELETE, ACT_FIX_ALL,
    ACT_CLEAR_ALL, ACT_NOTE, ACT_SAVE, ACT_COMPLETE, ACT_DISMISS_OUTCOME, ACT_OUTCOME_SESSION, ACT_REPLIED_URL, ACT_SAVED_URL,
};
enum { TIMER_POLL = 1 };
enum { TAG_PROJECTS = 1, TAG_SESSIONS, TAG_WRITE };
enum { FINDING_STRIDE = 1024 };   // a finding's argument is round * stride + finding; a verdict's is that times 4 plus the segment
enum { WRITE_NONE, WRITE_COMPLETE, WRITE_SAVE, WRITE_REPLY, WRITE_DELETE };

typedef struct { char *session_id; int round; char *key; char *decision; char *reason; } Draft;   // decision NULL while unmarked
typedef struct { char *session_id; int round; char *note; } Note;
typedef struct { char *session_id; char *key; char *url; char *error; } Replied;   // what a reply on a finding came back with
typedef struct { char *session_id; char *error; char *saved; char *saved_url; bool saved_failed; char *info; bool info_danger; } Card;
typedef struct { char *session_id; char *title; char *text; bool danger; } Outcome;   // what a send came back with, once its card is gone
typedef struct { size_t *rounds; size_t count; const char *repo; int pr; } Group;

typedef struct {
    Screen base;
    Project *projects; size_t project_count;
    Session *all; size_t all_count;             // every project's conversations, which the rounds index
    Session *incoming; size_t incoming_count;   // what the read under way has gathered so far
    HeldRound *rounds; size_t round_count;
    bool loaded, doc_stale, cycle_failed, projects_failed;
    char *error;
    Request *req_projects, *req_sessions, *req_write;
    double retry_after;
    Poller poller;
    Draft *drafts; size_t draft_count;
    Note *notes; size_t note_count;
    char **seeded; size_t seeded_count;         // "session\nround" of the rounds whose saved drafts were taken once
    Replied *replied; size_t replied_count;
    Card *cards; size_t card_count;
    Outcome *outcomes; size_t outcome_count;
    int write_kind; char *write_session; char *write_key; char *write_title; int write_round;
    char *unsent, *unsent_title; int unsent_round;   // a completion that failed, whose card may be gone once the list is read again
} FindingsScreen;

// MARK: - Rounds and their drafts

static int round_number(const Json *held) { return json_int_or(json_get(held, "round"), 0); }
static const Session *round_session(FindingsScreen *s, size_t round) { return &s->all[s->rounds[round].index]; }
/// The round a conversation holds now, by number, or -1 once it left the screen: nothing is drafted or sent for a round that is gone.
static int find_round(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->round_count; i++) {
        if (str_eq(session_id(round_session(s, i)), sid) && round_number(s->rounds[i].held) == round) return (int)i;
    }
    return -1;
}

static Draft *draft_find(FindingsScreen *s, const char *sid, int round, const char *key) {
    for (size_t i = 0; i < s->draft_count; i++) {
        if (s->drafts[i].round == round && str_eq(s->drafts[i].session_id, sid) && str_eq(s->drafts[i].key, key)) return &s->drafts[i];
    }
    return NULL;
}
static Draft *draft_get(FindingsScreen *s, const char *sid, int round, const char *key) {
    Draft *d = draft_find(s, sid, round, key);
    if (d) return d;
    s->drafts = xrealloc(s->drafts, (s->draft_count + 1) * sizeof *s->drafts);
    d = &s->drafts[s->draft_count++];
    memset(d, 0, sizeof *d);
    d->session_id = xstrdup(sid); d->round = round; d->key = xstrdup(key);
    return d;
}
static void draft_clear(Draft *d) { free(d->session_id); free(d->key); free(d->decision); free(d->reason); }
static const char *decision_of(FindingsScreen *s, const char *sid, int round, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    Draft *d = key ? draft_find(s, sid, round, key) : NULL;
    return d && d->decision ? d->decision : "";
}
static const char *reason_of(FindingsScreen *s, const char *sid, int round, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    Draft *d = key ? draft_find(s, sid, round, key) : NULL;
    return d && d->reason ? d->reason : "";
}

static Note *note_find(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->note_count; i++) if (s->notes[i].round == round && str_eq(s->notes[i].session_id, sid)) return &s->notes[i];
    return NULL;
}
static void note_set(FindingsScreen *s, const char *sid, int round, const char *text) {
    Note *n = note_find(s, sid, round);
    if (!n) {
        s->notes = xrealloc(s->notes, (s->note_count + 1) * sizeof *s->notes);
        n = &s->notes[s->note_count++];
        memset(n, 0, sizeof *n);
        n->session_id = xstrdup(sid); n->round = round;
    }
    set_string(&n->note, text);
}
static const char *note_of(FindingsScreen *s, const char *sid, int round) { Note *n = note_find(s, sid, round); return n && n->note ? n->note : ""; }

static Replied *replied_find(FindingsScreen *s, const char *sid, const char *key) {
    for (size_t i = 0; i < s->replied_count; i++) if (str_eq(s->replied[i].session_id, sid) && str_eq(s->replied[i].key, key)) return &s->replied[i];
    return NULL;
}
static Replied *replied_get(FindingsScreen *s, const char *sid, const char *key) {
    Replied *r = replied_find(s, sid, key);
    if (r) return r;
    s->replied = xrealloc(s->replied, (s->replied_count + 1) * sizeof *s->replied);
    r = &s->replied[s->replied_count++];
    memset(r, 0, sizeof *r);
    r->session_id = xstrdup(sid); r->key = xstrdup(key);
    return r;
}
static void replied_clear(Replied *r) { free(r->session_id); free(r->key); free(r->url); free(r->error); }
static void replied_drop(FindingsScreen *s, const char *sid, const char *key) {
    Replied *r = replied_find(s, sid, key);
    if (!r) return;
    replied_clear(r);
    *r = s->replied[--s->replied_count];
}

static Card *card_find(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->card_count; i++) if (str_eq(s->cards[i].session_id, sid)) return &s->cards[i];
    return NULL;
}
static Card *card_get(FindingsScreen *s, const char *sid) {
    Card *c = card_find(s, sid);
    if (c) return c;
    s->cards = xrealloc(s->cards, (s->card_count + 1) * sizeof *s->cards);
    c = &s->cards[s->card_count++];
    memset(c, 0, sizeof *c);
    c->session_id = xstrdup(sid);
    return c;
}
static void card_clear(Card *c) { free(c->session_id); free(c->error); free(c->saved); free(c->saved_url); free(c->info); }
/// An edit after a save: the "Saved" line no longer describes what is on the screen.
static void card_forget_saved(FindingsScreen *s, const char *sid) {
    Card *c = card_find(s, sid);
    if (c) { set_string(&c->saved, NULL); set_string(&c->saved_url, NULL); c->saved_failed = false; }
}

static void outcome_clear(Outcome *o) { free(o->session_id); free(o->title); free(o->text); }
static void outcome_set(FindingsScreen *s, const char *sid, const char *title, const char *text, bool danger) {
    for (size_t i = 0; i < s->outcome_count; i++) {
        if (!str_eq(s->outcomes[i].session_id, sid)) continue;
        set_string(&s->outcomes[i].title, title); set_string(&s->outcomes[i].text, text); s->outcomes[i].danger = danger;
        return;
    }
    s->outcomes = xrealloc(s->outcomes, (s->outcome_count + 1) * sizeof *s->outcomes);
    Outcome *o = &s->outcomes[s->outcome_count++];
    memset(o, 0, sizeof *o);
    o->session_id = xstrdup(sid); o->title = xstrdup(title ? title : "(untitled)"); o->text = xstrdup(text); o->danger = danger;
}
static void outcome_drop(FindingsScreen *s, size_t i) {
    if (i >= s->outcome_count) return;
    outcome_clear(&s->outcomes[i]);
    memmove(&s->outcomes[i], &s->outcomes[i + 1], (s->outcome_count - i - 1) * sizeof *s->outcomes);
    s->outcome_count--;
}

static bool round_live(FindingsScreen *s, const char *sid, int round) { return find_round(s, sid, round) >= 0; }
static bool session_live(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->round_count; i++) if (str_eq(session_id(round_session(s, i)), sid)) return true;
    return false;
}
static bool finding_live(FindingsScreen *s, const char *sid, const char *key) {
    for (size_t i = 0; i < s->round_count; i++) {
        if (!str_eq(session_id(round_session(s, i)), sid)) continue;
        const Json *findings = json_get(s->rounds[i].held, "findings");
        for (size_t k = 0; k < json_count(findings); k++) if (str_eq(json_str(json_get(json_at(findings, k), "key")), key)) return true;
    }
    return false;
}
/// Drafts, notes and lines for a round no longer on the screen: sent from elsewhere, ruled on by an orchestrator, dropped with
/// its conversation or pull request.
static void prune(FindingsScreen *s) {
    for (size_t i = 0; i < s->draft_count;) {
        if (round_live(s, s->drafts[i].session_id, s->drafts[i].round)) { i++; continue; }
        draft_clear(&s->drafts[i]); s->drafts[i] = s->drafts[--s->draft_count];
    }
    for (size_t i = 0; i < s->note_count;) {
        if (round_live(s, s->notes[i].session_id, s->notes[i].round)) { i++; continue; }
        free(s->notes[i].session_id); free(s->notes[i].note); s->notes[i] = s->notes[--s->note_count];
    }
    for (size_t i = 0; i < s->seeded_count;) {
        char *nl = strchr(s->seeded[i], '\n');
        bool live = false;
        if (nl) { *nl = 0; live = round_live(s, s->seeded[i], atoi(nl + 1)); *nl = '\n'; }
        if (live) { i++; continue; }
        free(s->seeded[i]); s->seeded[i] = s->seeded[--s->seeded_count];
    }
    for (size_t i = 0; i < s->replied_count;) {
        if (finding_live(s, s->replied[i].session_id, s->replied[i].key)) { i++; continue; }
        replied_clear(&s->replied[i]); s->replied[i] = s->replied[--s->replied_count];
    }
    for (size_t i = 0; i < s->card_count;) {
        if (session_live(s, s->cards[i].session_id)) { i++; continue; }
        card_clear(&s->cards[i]); s->cards[i] = s->cards[--s->card_count];
    }
}
/// The drafts a round was saved with (Save comments) become this screen's drafts the first time the round is drawn here.
/// Only once per round: after that what was typed here is the truth, and a poll must not put back what has since changed.
static void seed(FindingsScreen *s) {
    for (size_t i = 0; i < s->round_count; i++) {
        const Session *ses = round_session(s, i);
        const Json *held = s->rounds[i].held;
        int round = round_number(held);
        char *key = xstrfmt("%s\n%d", session_id(ses), round);
        bool seen = false;
        for (size_t k = 0; k < s->seeded_count && !seen; k++) seen = str_eq(s->seeded[k], key);
        if (seen) { free(key); continue; }
        s->seeded = xrealloc(s->seeded, (s->seeded_count + 1) * sizeof *s->seeded);
        s->seeded[s->seeded_count++] = key;
        const Json *saved = json_get(held, "drafts");
        if (!json_is_object(saved)) continue;
        const Json *findings = json_get(held, "findings");
        for (size_t k = 0; k < json_count(findings); k++) {
            const char *fk = json_str(json_get(json_at(findings, k), "key"));
            if (!fk) continue;
            const Json *d = json_get(json_get(saved, "verdicts"), fk);
            if (!json_is_object(d) || draft_find(s, session_id(ses), round, fk)) continue;
            Draft *draft = draft_get(s, session_id(ses), round, fk);
            set_string(&draft->decision, json_str_nonempty(json_get(d, "decision")));
            set_string(&draft->reason, json_str_nonempty(json_get(d, "reason")));
        }
        const char *note = json_str_nonempty(json_get(saved, "note"));
        if (note && !note_find(s, session_id(ses), round)) note_set(s, session_id(ses), round, note);
    }
}
static void drop_round_drafts(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->draft_count;) {
        if (!(s->drafts[i].round == round && str_eq(s->drafts[i].session_id, sid))) { i++; continue; }
        draft_clear(&s->drafts[i]); s->drafts[i] = s->drafts[--s->draft_count];
    }
    Note *n = note_find(s, sid, round);
    if (n) { free(n->session_id); free(n->note); *n = s->notes[--s->note_count]; }
}

static void rebuild(FindingsScreen *s) {
    free(s->rounds);
    s->rounds = sessions_held_rounds(s->all, s->all_count, &s->round_count);
    prune(s); seed(s);
    s->doc_stale = true;
}

/// Rounds by the pull request they were left on, in the queue's oldest-first order.
static size_t build_groups(FindingsScreen *s, Group **out) {
    Group *groups = xcalloc(s->round_count, sizeof *groups);
    size_t n = 0;
    for (size_t i = 0; i < s->round_count; i++) {
        const char *repo = session_repo(round_session(s, i));
        int pr = held_round_pr_number(s->rounds[i].held);
        size_t g = 0;
        while (g < n && !(groups[g].pr == pr && str_ieq(groups[g].repo, repo))) g++;
        if (g == n) { groups[n].repo = repo ? repo : ""; groups[n].pr = pr; groups[n].rounds = xcalloc(s->round_count, sizeof *groups[n].rounds); n++; }
        groups[g].rounds[groups[g].count++] = i;
    }
    *out = groups;
    return n;
}
static void groups_free(Group *groups, size_t n) { for (size_t i = 0; i < n; i++) free(groups[i].rounds); free(groups); }

// MARK: - Reading

static void cycle_end(FindingsScreen *s);

static void sessions_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    Session *items; size_t n;
    if (req->ok && sessions_parse(req->result, &items, &n)) {
        // One list for every project; each project's own rows are saved under its key.
        for (size_t p = 0; p < s->project_count; p++) {
            const char *repo = s->projects[p].repo;
            Json *list = json_array();
            for (size_t i = 0; i < n; i++) {
                if (!str_eq(session_repo(&items[i]), repo)) continue;
                json_array_push(list, json_clone(items[i].raw));
                s->incoming = xrealloc(s->incoming, (s->incoming_count + 1) * sizeof *s->incoming);
                session_copy(&s->incoming[s->incoming_count++], &items[i]);
            }
            char *key = xstrfmt("sessions:%s", repo);
            cache_store(g_store.cache, list, key);
            json_free(list); free(key);
        }
        sessions_free(items, n);
    } else {
        // A refusal keeps what was shown before rather than emptying the cards mid-edit.
        for (size_t i = 0; i < s->all_count; i++) {
            s->incoming = xrealloc(s->incoming, (s->incoming_count + 1) * sizeof *s->incoming);
            session_copy(&s->incoming[s->incoming_count++], &s->all[i]);
        }
        char *text = request_error_or_unexpected(req);
        set_string(&s->error, text); free(text);
        s->cycle_failed = true; s->retry_after = req->error.retry_after;
    }
    cycle_end(s);
}
static void projects_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    Project *items; size_t n;
    if (req->ok && projects_parse(req->result, &items, &n)) {
        projects_free(s->projects, s->project_count);
        s->projects = items; s->project_count = n;
        Json *list = projects_json(items, n); cache_store(g_store.cache, list, "projects"); json_free(list);
        store_call("sessions", json_object(), 0, s, sessions_done, TAG_SESSIONS, &s->req_sessions);
        return;
    }
    request_error_into(&s->error, req);
    s->cycle_failed = true; s->projects_failed = true; s->retry_after = req->error.retry_after;
    cycle_end(s);
}
static void cycle_start(FindingsScreen *s) {
    if (s->req_projects || s->req_sessions) return;
    s->cycle_failed = false; s->projects_failed = false; s->retry_after = -1;
    sessions_free(s->incoming, s->incoming_count); s->incoming = NULL; s->incoming_count = 0;
    store_call("projects", json_object(), 0, s, projects_done, TAG_PROJECTS, &s->req_projects);
}
/// Reads everything again now, as after a write: what the server holds is the answer to what was sent.
static void cycle_now(FindingsScreen *s) {
    request_cancel(&s->req_projects); request_cancel(&s->req_sessions);
    cycle_start(s);
}
static void cycle_end(FindingsScreen *s) {
    if (!s->projects_failed) {
        sessions_free(s->all, s->all_count);
        s->all = s->incoming; s->all_count = s->incoming_count;
        s->incoming = NULL; s->incoming_count = 0;
        rebuild(s);
    }
    if (!s->cycle_failed) set_string(&s->error, NULL);
    s->loaded = true;
    // A completion that failed because its round is no longer held has no card left to carry its error.
    if (s->unsent) {
        Card *c = card_find(s, s->unsent);
        if (c && c->error && find_round(s, s->unsent, s->unsent_round) < 0) {
            const char *title = NULL;
            for (size_t i = 0; i < s->all_count && !title; i++) if (str_eq(session_id(&s->all[i]), s->unsent)) title = session_display_title(&s->all[i]);
            char *text = xstrfmt("Not sent: %s", c->error);
            outcome_set(s, s->unsent, title ? title : s->unsent_title, text, true);
            free(text);
            set_string(&c->error, NULL);
        }
        set_string(&s->unsent, NULL); set_string(&s->unsent_title, NULL);
    }
    poller_finished(&s->poller, s->cycle_failed, s->retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    projects_recount_findings();
}
/// What was saved opens at once; the server is asked for the rest.
static void restore(FindingsScreen *s) {
    Json *saved = cache_value(g_store.cache, "projects");
    if (!saved) return;
    Project *items; size_t n;
    if (projects_parse(saved, &items, &n)) {
        projects_free(s->projects, s->project_count);
        s->projects = items; s->project_count = n;
        for (size_t i = 0; i < n; i++) {
            char *key = xstrfmt("sessions:%s", items[i].repo);
            Json *list = cache_value(g_store.cache, key);
            free(key);
            if (!list) continue;
            Session *sessions; size_t m;
            if (sessions_parse(list, &sessions, &m)) {
                s->all = xrealloc(s->all, (s->all_count + m + 1) * sizeof *s->all);
                for (size_t k = 0; k < m; k++) session_copy(&s->all[s->all_count++], &sessions[k]);
                sessions_free(sessions, m);
            }
            json_free(list);
        }
        s->loaded = true;
        rebuild(s);
    }
    json_free(saved);
}

size_t findings_waiting(const Project *projects, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        char *key = xstrfmt("sessions:%s", projects[i].repo);
        Json *list = cache_value(g_store.cache, key);
        free(key);
        if (!list) continue;
        Session *sessions; size_t m;
        if (sessions_parse(list, &sessions, &m)) {
            for (size_t k = 0; k < m; k++) if (session_held_round(&sessions[k])) total++;
            sessions_free(sessions, m);
        }
        json_free(list);
    }
    return total;
}

// MARK: - Writing

static void write_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    int kind = s->write_kind; s->write_kind = WRITE_NONE;
    char *sid = s->write_session; s->write_session = NULL;
    char *key = s->write_key; s->write_key = NULL;
    char *title = s->write_title; s->write_title = NULL;
    int round = s->write_round;
    char *text = req->ok ? NULL : request_error_text(req);
    Card *card = card_get(s, sid);
    switch (kind) {
    case WRITE_COMPLETE:
        if (req->ok) {
            drop_round_drafts(s, sid, round);
            const Json *r = req->result;
            // A dismissal of somebody else's review says nothing more; anything else is worth a line once the card is gone.
            if (!json_is_set(json_get(r, "dismissed")) || json_is_set(json_get(r, "approved"))) {
                const Json *os = json_get(r, "session");
                const char *oid = json_str_nonempty(json_get(os, "id")), *ot = json_str_nonempty(json_get(os, "title"));
                bool danger; char *line = triage_outcome_text(r, &danger);
                outcome_set(s, oid ? oid : sid, ot ? ot : title, line, danger);
                free(line);
            }
        } else { set_string(&card->error, text); set_string(&s->unsent, sid); set_string(&s->unsent_title, title); s->unsent_round = round; }
        cycle_now(s);
        break;
    case WRITE_SAVE:
        if (req->ok) {
            const char *warning = json_str_nonempty(json_get(req->result, "warning")), *url = json_str(json_get(req->result, "url"));
            set_string(&card->saved, warning ? warning : "Saved"); card->saved_failed = warning != NULL;
            set_string(&card->saved_url, !warning && safe_web_url(url) ? url : NULL);
        } else { char *m = xstrfmt("Not saved: %s", text); set_string(&card->saved, m); free(m); card->saved_failed = true; set_string(&card->saved_url, NULL); }
        break;
    case WRITE_REPLY: {
        Replied *r = replied_get(s, sid, key);
        if (req->ok) {
            const char *url = json_str(json_get(req->result, "url"));
            set_string(&r->error, NULL); set_string(&r->url, safe_web_url(url) ? url : NULL);
            Draft *d = draft_find(s, sid, round, key);
            if (d) set_string(&d->reason, NULL);
        } else set_string(&r->error, text);
        break;
    }
    case WRITE_DELETE:
        if (req->ok) {
            const char *warning = json_str_nonempty(json_get(req->result, "warning"));
            if (warning) { char *m = xstrfmt("Deleted, but the review still declares it: %s", warning); set_string(&card->info, m); free(m); card->info_danger = true; }
            else { set_string(&card->info, json_is_set(json_get(req->result, "commentDeleted")) ? "Deleted from the review" : "The review no longer declares it; it had no comment of its own to delete"); card->info_danger = false; }
        } else { char *m = xstrfmt("Not deleted: %s", text); set_string(&card->error, m); free(m); }
        cycle_now(s);
        break;
    }
    free(text); free(sid); free(key); free(title);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
/// One write at a time; the card it is on says what is happening until the answer comes.
static void write_start(FindingsScreen *s, int kind, const char *operation, const Session *session, int round, const char *key, Json *args) {
    if (s->req_write) { json_free(args); return; }
    s->write_kind = kind; s->write_round = round;
    set_string(&s->write_session, session_id(session)); set_string(&s->write_key, key); set_string(&s->write_title, session_display_title(session));
    json_set_str(args, "sessionId", session_id(session));
    Card *c = card_find(s, session_id(session));
    if (c && kind != WRITE_REPLY) set_string(&c->error, NULL);
    if (c && kind == WRITE_DELETE) set_string(&c->info, NULL);
    store_call(operation, args, 0, s, write_done, TAG_WRITE, &s->req_write);
    pane_relayout(s->base.pane);
}
static bool writing(FindingsScreen *s, int kind, const char *sid, const char *key) {
    return s->req_write && s->write_kind == kind && str_eq(s->write_session, sid) && (!key || str_eq(s->write_key, key));
}

static Json *verdicts_for(FindingsScreen *s, const Session *ses, const Json *held, bool completing) {
    Json *verdicts = json_array();
    const Json *findings = json_get(held, "findings");
    int round = round_number(held);
    for (size_t i = 0; i < json_count(findings); i++) {
        const Json *f = json_at(findings, i);
        const char *key = json_str(json_get(f, "key"));
        if (!key) continue;
        const char *decision = decision_of(s, session_id(ses), round, f), *reason = reason_of(s, session_id(ses), round, f);
        Json *v = json_object();
        json_set_str(v, "key", key);
        // Completing rules on every finding: an unmarked one goes as optional, which the loop never offers again.
        if (*decision) json_set_str(v, "decision", decision);
        else if (completing) json_set_str(v, "decision", "optional");
        else json_object_set(v, "decision", json_null());
        if (*reason || !completing) json_set_str(v, "reason", reason);
        json_array_push(verdicts, v);
    }
    return verdicts;
}
static void complete_round(FindingsScreen *s, size_t r) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    int round = round_number(held), pr = held_round_pr_number(held);
    bool mine = held_round_is_mine(held);
    char *sid = xstrdup(session_id(ses));
    Json *args = json_object();
    char *title, *message;
    if (mine) {
        size_t fixes = 0;
        const Json *findings = json_get(held, "findings");
        for (size_t i = 0; i < json_count(findings); i++) if (str_eq(decision_of(s, sid, round, json_at(findings, i)), "fix")) fixes++;
        json_object_set(args, "verdicts", verdicts_for(s, ses, held, true));
        const char *note = note_of(s, sid, round);
        if (*note) json_set_str(args, "note", note);
        title = fixes ? xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s") : xstrdup("Complete with nothing to fix?");
        message = fixes ? xstrfmt("Every verdict and comment is recorded on PR #%d; what is marked fix goes to the fix session.", pr)
                        : xstrfmt("Every verdict is recorded on PR #%d, which is approved and its loop closed.", pr);
    } else {
        title = xstrdup("Take this review off the queue?");
        message = xstrfmt("What it found stays on PR #%d for its author.", pr);
    }
    bool ok = app_confirm(title, message, "Complete", false);
    free(title); free(message);
    int again = ok ? find_round(s, sid, round) : -1;
    if (again < 0) { json_free(args); free(sid); return; }
    write_start(s, WRITE_COMPLETE, "complete_findings", round_session(s, (size_t)again), round, NULL, args);
    free(sid);
}
static void save_round(FindingsScreen *s, size_t r) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    int round = round_number(held);
    Json *args = json_object();
    json_object_set(args, "verdicts", verdicts_for(s, ses, held, false));
    json_set_str(args, "note", note_of(s, session_id(ses), round));
    card_forget_saved(s, session_id(ses));
    write_start(s, WRITE_SAVE, "save_findings", ses, round, NULL, args);
}
static void reply_on(FindingsScreen *s, size_t r, size_t finding) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), finding);
    const char *key = json_str(json_get(f, "key"));
    if (!key) return;
    int round = round_number(held);
    char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
    const char *ft = json_str(json_get(f, "title"));
    char *typed = dialog_text(app_window(), "Reply on this finding\xE2\x80\x99s thread", ft ? ft : "Reply", "Reply", reason_of(s, sid, round, f));
    int again = typed ? find_round(s, sid, round) : -1;
    if (again >= 0) {
        char *text = str_trim(typed);
        Draft *d = draft_get(s, sid, round, fkey);
        set_string(&d->reason, text);
        if (*text) {
            replied_drop(s, sid, fkey);
            Json *args = json_object(); json_set_str(args, "key", fkey); json_set_str(args, "text", text);
            write_start(s, WRITE_REPLY, "reply_finding", round_session(s, (size_t)again), round, fkey, args);
        }
        free(text);
        pane_relayout(s->base.pane);
    }
    free(typed); free(sid); free(fkey);
}
static void delete_finding(FindingsScreen *s, size_t r, size_t finding) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), finding);
    const char *key = json_str(json_get(f, "key"));
    if (!key) return;
    int round = round_number(held);
    char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
    const char *ft = json_str(json_get(f, "title"));
    char *message = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D \xE2\x80\x94 its comment on PR #%d is deleted on GitHub and the review stops declaring it. This cannot be undone.", ft ? ft : "Finding", held_round_pr_number(held));
    bool ok = app_confirm("Delete this finding from the review?", message, "Delete from the review", true);
    free(message);
    int again = ok ? find_round(s, sid, round) : -1;
    if (again >= 0) {
        Json *args = json_object(); json_set_str(args, "key", fkey);
        write_start(s, WRITE_DELETE, "delete_finding", round_session(s, (size_t)again), round, fkey, args);
    }
    free(sid); free(fkey);
}

// MARK: - Layout

static void clickable(Doc *doc, int item, int action, intptr_t arg) { doc_item(doc, item)->action = action; doc_item(doc, item)->arg = arg; doc_item(doc, item)->hand = true; }

static void layout_outcome(FindingsScreen *s, Doc *doc, int x, int w, size_t i) {
    const Outcome *o = &s->outcomes[i];
    int box = doc_box_begin(doc, x, w, px(8), theme.elevated, theme.border, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(10), iw = w - px(20), close_w = px(22);
    int tw = text_width(doc->cv, o->title, FONT_CAPTION_SEMIBOLD) + px(2);
    if (tw > iw / 2) tw = iw / 2;
    int top = doc->y, lh = font_height(doc->cv, FONT_CAPTION) + px(2);
    RECT tr = { ix, top, ix + tw, top + lh };
    int ti = doc_text_at(doc, &tr, o->title, FONT_CAPTION_SEMIBOLD, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    clickable(doc, ti, ACT_OUTCOME_SESSION, (intptr_t)i);
    int mx = ix + tw + px(8), mw = iw - tw - px(8) - close_w;
    doc->y = top;
    doc_text(doc, mx, mw, o->text, FONT_CAPTION, o->danger ? theme.danger : theme.secondary, DT_WORDBREAK);
    if (doc->y < top + lh) doc->y = top + lh;
    RECT cr = { ix + iw - close_w + px(4), top, ix + iw, top + lh };
    wchar_t glyph[2] = { 0xE711, 0 }; char *close = wide_to_utf8(glyph);
    int ci = doc_text_at(doc, &cr, close, FONT_ICON_SMALL, theme.secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    free(close);
    clickable(doc, ci, ACT_DISMISS_OUTCOME, (intptr_t)i);
    doc_box_end(doc, box, px(8));
    doc_space(doc, px(8));
}

static void layout_finding(FindingsScreen *s, Doc *doc, int fx, int fw, size_t r, size_t k, bool mine, bool busy) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), k);
    const char *sid = session_id(ses), *key = json_str(json_get(f, "key"));
    int round = round_number(held);
    intptr_t arg = (intptr_t)(r * FINDING_STRIDE + k);
    doc_rule(doc, fx, fw);
    doc_space(doc, px(8));
    COLORREF sev_color;
    BadgeSpec sev = { 0, finding_severity_label(json_str(json_get(f, "severity")), &sev_color), 0, false };
    sev.color = sev_color;
    int chip_w = doc_badges_width(doc, &sev, 1);
    int top = doc->y;
    doc_badges(doc, fx, chip_w, &sev, 1, theme.elevated);
    int chip_h = doc->y - top;
    doc->y = top;
    const char *ft = json_str(json_get(f, "title"));
    int ti = doc_text(doc, fx + chip_w + px(6), fw - chip_w - px(6), ft ? ft : "Finding", FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
    clickable(doc, ti, ACT_OPEN_FINDING, arg);
    if (doc->y < top + chip_h) doc->y = top + chip_h;
    const char *file = json_str_nonempty(json_get(f, "file"));
    if (file) {
        int line = json_int_or(json_get(f, "line"), 0);
        char *loc = line ? xstrfmt("%s:%d \xE2\x86\x97", file, line) : xstrfmt("%s \xE2\x86\x97", file);
        doc_space(doc, px(4));
        int li = doc_text(doc, fx, fw, loc, FONT_MONO_CAPTION2, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        clickable(doc, li, ACT_OPEN_FINDING, arg);
        free(loc);
    }
    if (json_is_set(json_get(f, "parked"))) {
        const char *why = json_str_nonempty(json_get(f, "parkedWhy")), *parked = json_str_nonempty(json_get(f, "parked"));
        char *advice = xstrfmt("The loop would have parked it: %s.", why ? why : parked ? parked : "");
        doc_space(doc, px(3));
        doc_text(doc, fx, fw, advice, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        free(advice);
    }
    if (!store_can_manage() || !key) return;
    if (mine) {
        if (!store_supports("complete_findings")) return;
        const char *decision = decision_of(s, sid, round, f);
        int selected = finding_decision_index(decision);
        doc_space(doc, px(4));
        doc_segments(doc, fx, fw, finding_decision_titles, FINDING_DECISION_COUNT, selected, ACT_VERDICT, arg * 4, !busy);
        doc_space(doc, px(4));
        const char *reason = reason_of(s, sid, round, f);
        const char *placeholder = "Comment (saved to the pull request)";
        int cb = doc_box_begin(doc, fx, fw, px(3), theme.field, theme.line, px(4));
        doc_item(doc, cb)->hover_fill = false;
        doc_text(doc, fx + px(6), fw - px(12), *reason ? reason : placeholder, FONT_CAPTION2, *reason ? theme.ink : theme.muted, DT_WORDBREAK);
        doc_box_end(doc, cb, px(3));
        if (!busy) doc_box_action(doc, cb, ACT_COMMENT, arg);
    } else {
        bool replying = writing(s, WRITE_REPLY, sid, key), deleting = writing(s, WRITE_DELETE, sid, key);
        ButtonSpec buttons[2]; size_t n = 0;
        if (store_supports("reply_finding")) { ButtonSpec b = { 0xE97A, replying ? "Replying\xE2\x80\xA6" : "Reply", BUTTON_BORDERED, ACT_REPLY, arg, !busy }; buttons[n++] = b; }
        if (store_supports("delete_finding")) { ButtonSpec b = { 0xE74D, deleting ? "Deleting\xE2\x80\xA6" : "Delete from the review", BUTTON_DESTRUCTIVE, ACT_DELETE, arg, !busy }; buttons[n++] = b; }
        if (n) { doc_space(doc, px(6)); doc_button_row(doc, fx, fw, buttons, n); }
        const Replied *rp = replied_find(s, sid, key);
        if (rp && rp->error) { doc_space(doc, px(4)); char *m = xstrfmt("Not replied: %s", rp->error); doc_text(doc, fx, fw, m, FONT_CAPTION, theme.danger, DT_WORDBREAK); free(m); }
        else if (rp) {
            doc_space(doc, px(4));
            int li = doc_text(doc, fx, fw, rp->url ? "Replied \xC2\xB7 on the pull request \xE2\x86\x97" : "Replied", FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
            if (rp->url) clickable(doc, li, ACT_REPLIED_URL, arg);
        }
    }
}

static void layout_round(FindingsScreen *s, Doc *doc, int x, int w, size_t r) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    const char *sid = session_id(ses);
    int round = round_number(held);
    bool mine = held_round_is_mine(held), standalone = json_is_set(json_get(held, "standalone"));
    bool manage = store_can_manage(), busy = s->req_write != NULL;
    bool sending = writing(s, WRITE_COMPLETE, sid, NULL), saving = writing(s, WRITE_SAVE, sid, NULL);
    const Json *findings = json_get(held, "findings");
    size_t n = json_count(findings);
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(12), iw = w - px(24);
    // The conversation's title, and what round this is and since when.
    Str meta; str_init(&meta);
    if (standalone) str_appendz(&meta, "code review"); else str_appendf(&meta, "round %d", round);
    time_t held_at;
    if (board_date_parse(json_str(json_get(held, "heldAt")), &held_at)) { char *when = format_event_time(held_at); str_appendf(&meta, " \xC2\xB7 held since %s", when); free(when); }
    const char *title = session_display_title(ses);
    int top = doc->y, th = px(22);
    int tw = text_width(doc->cv, title, FONT_BODY_SEMIBOLD) + px(4), mw = text_width(doc->cv, meta.data, FONT_CAPTION) + px(4);
    if (tw + px(8) + mw <= iw) {
        RECT tr = { ix, top, ix + tw, top + th };
        int ti = doc_text_at(doc, &tr, title, FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        clickable(doc, ti, ACT_OPEN_SESSION, (intptr_t)r);
        RECT mr = { ix + tw + px(8), top, ix + iw, top + th };
        doc_text_at(doc, &mr, meta.data, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        doc->y = top + th;
    } else {
        int ti = doc_text(doc, ix, iw, title, FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK);
        clickable(doc, ti, ACT_OPEN_SESSION, (intptr_t)r);
        doc_space(doc, px(2));
        doc_text(doc, ix, iw, meta.data, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    str_free(&meta);
    if (json_is_set(json_get(held, "stale"))) {
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "The branch moved after this round was reviewed: some of these may already be fixed. Sending with nothing to fix reviews the new commits instead of closing the loop.", FONT_CAPTION, theme.danger, DT_WORDBREAK);
    }
    // How this card is worked: findings on the user's own pull request are triaged here, because the verdicts are what
    // the fix session works from; a review of somebody else's is only read, its author answers what it found.
    char *count = xstrfmt("%zu finding%s", n, n == 1 ? "" : "s");
    char *how;
    if (!mine) how = n ? xstrfmt("%s, already on the pull request. Read them there; Reply says something on a finding\xE2\x80\x99s own thread, Delete takes one out of the review itself, and Complete takes this card off the queue and leaves the rest to the pull request\xE2\x80\x99s author.", count)
                      : xstrdup("Every finding was deleted from the review. Complete takes this card off the queue.");
    else if (!manage) how = xstrfmt("%s. This device is read-only: the verdicts are given on the dashboard.", count);
    else how = xstrfmt("%s. Give each one a verdict and a comment if you have one; Save comments keeps them here and on the pull request, and Complete appears once every finding is marked.", count);
    doc_space(doc, px(4));
    doc_text(doc, ix, iw, how, FONT_CAPTION, theme.muted, DT_WORDBREAK);
    free(how); free(count);
    if (mine && manage && n && store_supports("complete_findings")) {
        ButtonSpec all[2] = { { 0, "Fix all", BUTTON_PLAIN, ACT_FIX_ALL, (intptr_t)r, !busy }, { 0, "Clear", BUTTON_PLAIN, ACT_CLEAR_ALL, (intptr_t)r, !busy } };
        doc_space(doc, px(4));
        doc_button_row(doc, ix, iw, all, 2);
    }
    doc_space(doc, px(8));
    for (size_t k = 0; k < n; k++) { layout_finding(s, doc, ix, iw, r, k, mine, busy); doc_space(doc, px(8)); }
    const Card *card = card_find(s, sid);
    if (manage && store_supports("complete_findings")) {
        doc_rule(doc, ix, iw);
        doc_space(doc, px(8));
        if (mine) {
            const char *note = note_of(s, sid, round);
            int nb = doc_box_begin(doc, ix, iw, px(3), theme.field, theme.line, px(4));
            doc_item(doc, nb)->hover_fill = false;
            doc_text(doc, ix + px(6), iw - px(12), *note ? note : "A note for the pull request and the fix session (optional)", FONT_CAPTION2, *note ? theme.ink : theme.muted, DT_WORDBREAK);
            doc_box_end(doc, nb, px(3));
            if (!busy) doc_box_action(doc, nb, ACT_NOTE, (intptr_t)r);
            doc_space(doc, px(8));
            size_t fixes = 0, unruled = 0;
            for (size_t k = 0; k < n; k++) {
                const char *d = decision_of(s, sid, round, json_at(findings, k));
                if (str_eq(d, "fix")) fixes++;
                if (!*d) unruled++;
            }
            ButtonSpec buttons[2]; size_t bn = 0;
            if (store_supports("save_findings")) { ButtonSpec b = { 0, saving ? "Saving\xE2\x80\xA6" : "Save comments", BUTTON_BORDERED, ACT_SAVE, (intptr_t)r, !busy }; buttons[bn++] = b; }
            char *complete = sending ? xstrdup("Completing\xE2\x80\xA6") : fixes ? xstrfmt("Complete \xC2\xB7 send %zu to be fixed", fixes) : xstrdup("Complete \xC2\xB7 nothing to fix, approve and close");
            if (!unruled) { ButtonSpec b = { 0, complete, BUTTON_PROMINENT, ACT_COMPLETE, (intptr_t)r, !busy }; buttons[bn++] = b; }
            if (bn) doc_button_row(doc, ix, iw, buttons, bn);
            free(complete);
            if (unruled) {
                char *m = xstrfmt("%zu finding%s still unmarked; Complete appears once every finding has a verdict.", unruled, unruled == 1 ? "" : "s");
                doc_space(doc, px(6));
                doc_text(doc, ix, iw, m, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
                free(m);
            }
            if (card && card->saved) {
                doc_space(doc, px(6));
                int li = doc_text(doc, ix, iw, card->saved_url ? "Saved \xC2\xB7 comment on the pull request \xE2\x86\x97" : card->saved, FONT_CAPTION, card->saved_failed ? theme.danger : theme.secondary, DT_WORDBREAK);
                if (card->saved_url) clickable(doc, li, ACT_SAVED_URL, (intptr_t)r);
            } else {
                const char *saved_at = json_str_nonempty(json_get(json_get(held, "drafts"), "savedAt"));
                time_t at;
                if (saved_at && board_date_parse(saved_at, &at)) {
                    char *when = format_event_time(at), *m = xstrfmt("Comments saved %s", when);
                    doc_space(doc, px(6));
                    doc_text(doc, ix, iw, m, FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
                    free(when); free(m);
                }
            }
        } else {
            ButtonSpec b = { 0, sending ? "Completing\xE2\x80\xA6" : "Complete", BUTTON_PROMINENT, ACT_COMPLETE, (intptr_t)r, !busy };
            doc_button_row(doc, ix, iw, &b, 1);
        }
    }
    if (card && card->info) { doc_space(doc, px(6)); doc_text(doc, ix, iw, card->info, FONT_CAPTION, card->info_danger ? theme.danger : theme.secondary, DT_WORDBREAK); }
    if (card && card->error) { doc_space(doc, px(6)); doc_notice(doc, ix, iw, card->error); }
    doc_box_end(doc, box, px(10));
    doc_space(doc, px(12));
}

static void layout_group(FindingsScreen *s, Doc *doc, int x, int w, const Group *g) {
    size_t findings = 0;
    for (size_t i = 0; i < g->count; i++) findings += json_count(json_get(s->rounds[g->rounds[i]].held, "findings"));
    char *heading = xstrfmt("%s \xC2\xB7 PR #%d \xE2\x86\x97", g->repo, g->pr);
    char *count = g->count > 1 ? xstrfmt("%zu finding%s across %zu reviews", findings, findings == 1 ? "" : "s", g->count) : xstrfmt("%zu finding%s", findings, findings == 1 ? "" : "s");
    int top = doc->y, h = px(22);
    int hw = text_width(doc->cv, heading, FONT_BODY_SEMIBOLD) + px(4);
    if (hw > w - px(80)) hw = w - px(80);
    RECT hr = { x, top, x + hw, top + h };
    int hi = doc_text_at(doc, &hr, heading, FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    clickable(doc, hi, ACT_OPEN_PR, (intptr_t)g->rounds[0]);
    RECT cr = { hr.right + px(8), top, x + w, top + h };
    doc_text_at(doc, &cr, count, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = top + h + px(6);
    free(heading); free(count);
    for (size_t i = 0; i < g->count; i++) layout_round(s, doc, x, w, g->rounds[i]);
    doc_space(doc, px(6));
}

static void findings_layout(Screen *base, Doc *doc) {
    FindingsScreen *s = (FindingsScreen *)base;
    s->doc_stale = false;
    int w = doc->width, x = 0, iw = w;
    doc_space(doc, px(14));
    if (s->error) { doc_notice(doc, x, iw, s->error); doc_space(doc, px(10)); }
    for (size_t i = 0; i < s->outcome_count; i++) layout_outcome(s, doc, x, iw, i);
    if (s->round_count) {
        Group *groups; size_t n = build_groups(s, &groups);
        for (size_t i = 0; i < n; i++) layout_group(s, doc, x, iw, &groups[i]);
        groups_free(groups, n);
    } else if (s->loaded) {
        doc_text(doc, x, iw, "No review is waiting. Findings arrive here from \xE2\x8C\x95 Code review and from every review-loop round.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    }
    if (!s->loaded) doc_loading(doc, 0, w, "Loading findings\xE2\x80\xA6");
    doc_space(doc, px(12));
}
static void findings_header(Screen *base, HeaderInfo *info) {
    FindingsScreen *s = (FindingsScreen *)base;
    snprintf(info->title, sizeof info->title, "\xE2\x9A\x91 Findings");
    Group *groups; size_t n = build_groups(s, &groups);
    groups_free(groups, n);
    char *sub = findings_subtitle(s->round_count, n);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub);
    free(sub);
}

// MARK: - Actions

static void open_session_by_id(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->all_count; i++) if (str_eq(session_id(&s->all[i]), sid)) { app_push_detail(conversation_screen_new(&s->all[i])); return; }
}
static void set_all(FindingsScreen *s, size_t r, const char *decision) {
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held, *findings = json_get(held, "findings");
    int round = round_number(held);
    for (size_t k = 0; k < json_count(findings); k++) {
        const char *key = json_str(json_get(json_at(findings, k), "key"));
        if (key) set_string(&draft_get(s, session_id(ses), round, key)->decision, decision);
    }
    card_forget_saved(s, session_id(ses));
}
static void findings_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    FindingsScreen *s = (FindingsScreen *)base;
    if (action == ACT_DISMISS_OUTCOME) { outcome_drop(s, (size_t)arg); pane_relayout(base->pane); return; }
    if (action == ACT_OUTCOME_SESSION) {
        if ((size_t)arg >= s->outcome_count) return;
        char *sid = xstrdup(s->outcomes[arg].session_id);
        outcome_drop(s, (size_t)arg);
        open_session_by_id(s, sid);
        free(sid);
        pane_relayout(base->pane);
        return;
    }
    // A click that lands between a poll and the redraw is on rounds the screen no longer shows: nothing is drafted for it.
    if (s->doc_stale) return;
    size_t r, k = 0; int segment = -1;
    if (action == ACT_VERDICT) { segment = (int)(arg % 4); arg /= 4; }
    if (action == ACT_VERDICT || action == ACT_COMMENT || action == ACT_REPLY || action == ACT_DELETE || action == ACT_OPEN_FINDING || action == ACT_REPLIED_URL) { r = (size_t)arg / FINDING_STRIDE; k = (size_t)arg % FINDING_STRIDE; }
    else r = (size_t)arg;
    if (r >= s->round_count) return;
    const Session *ses = round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), k);
    const char *key = json_str(json_get(f, "key"));
    int round = round_number(held);
    switch (action) {
    case ACT_OPEN_SESSION: app_push_detail(conversation_screen_new(ses)); break;
    case ACT_OPEN_PR: { char *url = held_round_pr_url(ses, held); open_web_url(url); free(url); break; }
    case ACT_OPEN_FINDING: {
        const char *url = json_str(json_get(f, "url"));
        if (safe_web_url(url)) open_web_url(url);
        else { char *pr = held_round_pr_url(ses, held); if (pr) { char *files = xstrfmt("%s/files", pr); open_web_url(files); free(files); } free(pr); }
        break;
    }
    case ACT_REPLIED_URL: { const Replied *rp = key ? replied_find(s, session_id(ses), key) : NULL; if (rp && rp->url) open_web_url(rp->url); break; }
    case ACT_SAVED_URL: { const Card *c = card_find(s, session_id(ses)); if (c && c->saved_url) open_web_url(c->saved_url); break; }
    case ACT_VERDICT: {
        if (!key || segment < 0 || segment >= FINDING_DECISION_COUNT) break;
        Draft *d = draft_get(s, session_id(ses), round, key);
        set_string(&d->decision, str_eq(d->decision, finding_decision_ids[segment]) ? NULL : finding_decision_ids[segment]);   // the same pick twice clears it
        card_forget_saved(s, session_id(ses));
        pane_relayout(base->pane);
        break;
    }
    case ACT_COMMENT: {
        if (!key) break;
        char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
        const char *ft = json_str(json_get(f, "title"));
        char *typed = dialog_text(app_window(), "Comment on this finding", ft ? ft : "Comment", "Save", reason_of(s, sid, round, f));
        if (typed && find_round(s, sid, round) >= 0) { set_string(&draft_get(s, sid, round, fkey)->reason, typed); card_forget_saved(s, sid); pane_relayout(base->pane); }
        free(typed); free(sid); free(fkey);
        break;
    }
    case ACT_REPLY: reply_on(s, r, k); break;
    case ACT_DELETE: delete_finding(s, r, k); break;
    case ACT_FIX_ALL: set_all(s, r, "fix"); pane_relayout(base->pane); break;
    case ACT_CLEAR_ALL: set_all(s, r, NULL); pane_relayout(base->pane); break;
    case ACT_NOTE: {
        char *sid = xstrdup(session_id(ses));
        char *typed = dialog_text(app_window(), "Note for the fix session", "A note for the pull request and the fix session", "Save", note_of(s, sid, round));
        if (typed && find_round(s, sid, round) >= 0) { note_set(s, sid, round, typed); card_forget_saved(s, sid); pane_relayout(base->pane); }
        free(typed); free(sid);
        break;
    }
    case ACT_SAVE: save_round(s, r); break;
    case ACT_COMPLETE: complete_round(s, r); break;
    }
}

// MARK: - Lifecycle

static void findings_timer(Screen *base, UINT id) {
    FindingsScreen *s = (FindingsScreen *)base;
    if (!poller_fired(&s->poller, id)) return;
    // A read still under way answers this tick too; the poller only needs its next one scheduled.
    if (s->req_projects || s->req_sessions) poller_finished(&s->poller, false, -1); else cycle_start(s);
}
static void findings_visible(Screen *base, bool shown) {
    FindingsScreen *s = (FindingsScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 7000);
    else { poller_stop(&s->poller); request_cancel(&s->req_projects); request_cancel(&s->req_sessions); sessions_free(s->incoming, s->incoming_count); s->incoming = NULL; s->incoming_count = 0; }
}
static void findings_refresh(Screen *base) { cycle_now((FindingsScreen *)base); }
static void findings_activated(Screen *base, bool active) { if (active) { FindingsScreen *s = (FindingsScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 7000); } }
static void findings_destroy(Screen *base) {
    FindingsScreen *s = (FindingsScreen *)base;
    poller_stop(&s->poller);
    request_cancel(&s->req_projects); request_cancel(&s->req_sessions); request_cancel(&s->req_write);
    projects_free(s->projects, s->project_count);
    sessions_free(s->all, s->all_count); sessions_free(s->incoming, s->incoming_count);
    free(s->rounds); free(s->error);
    for (size_t i = 0; i < s->draft_count; i++) draft_clear(&s->drafts[i]);
    free(s->drafts);
    for (size_t i = 0; i < s->note_count; i++) { free(s->notes[i].session_id); free(s->notes[i].note); }
    free(s->notes);
    str_array_free(s->seeded, s->seeded_count);
    for (size_t i = 0; i < s->replied_count; i++) replied_clear(&s->replied[i]);
    free(s->replied);
    for (size_t i = 0; i < s->card_count; i++) card_clear(&s->cards[i]);
    free(s->cards);
    for (size_t i = 0; i < s->outcome_count; i++) outcome_clear(&s->outcomes[i]);
    free(s->outcomes);
    free(s->write_session); free(s->write_key); free(s->write_title); free(s->unsent); free(s->unsent_title);
    screen_release(base);
}

static const ScreenVTable findings_vt = {
    .destroy = findings_destroy, .layout = findings_layout, .header = findings_header, .action = findings_action,
    .timer = findings_timer, .visible = findings_visible, .refresh = findings_refresh, .activated = findings_activated,
};
Screen *findings_screen_new(void) {
    FindingsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &findings_vt; s->base.id = xstrdup("findings");
    s->write_kind = WRITE_NONE; s->retry_after = -1;
    restore(s);
    return &s->base;
}
