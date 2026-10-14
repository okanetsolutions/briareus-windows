#include "screen_findings_internal.h"

static Note *note_find(FindingsScreen *s, const char *sid, int round);
static bool round_live(FindingsScreen *s, const char *sid, int round);
static bool session_live(FindingsScreen *s, const char *sid);
static bool finding_live(FindingsScreen *s, const char *sid, const char *key);
static void prune(FindingsScreen *s);
static void seed(FindingsScreen *s);

/// The round a conversation holds now, by number, or -1 once it left the screen: nothing is drafted or sent for a round that is gone.
int screen_findings_find_round(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->round_count; i++) {
        if (str_eq(session_id(screen_findings_round_session(s, i)), sid) && screen_findings_round_number(s->rounds[i].held) == round) return (int)i;
    }
    return -1;
}

Draft *screen_findings_draft_find(FindingsScreen *s, const char *sid, int round, const char *key) {
    for (size_t i = 0; i < s->draft_count; i++) {
        if (s->drafts[i].round == round && str_eq(s->drafts[i].session_id, sid) && str_eq(s->drafts[i].key, key)) return &s->drafts[i];
    }
    return NULL;
}

Draft *screen_findings_draft_get(FindingsScreen *s, const char *sid, int round, const char *key) {
    Draft *d = screen_findings_draft_find(s, sid, round, key);
    if (d) return d;
    s->drafts = xrealloc(s->drafts, (s->draft_count + 1) * sizeof *s->drafts);
    d = &s->drafts[s->draft_count++];
    memset(d, 0, sizeof *d);
    d->session_id = xstrdup(sid); d->round = round; d->key = xstrdup(key);
    return d;
}

void screen_findings_draft_clear(Draft *d) { free(d->session_id); free(d->key); free(d->decision); free(d->reason); }

const char *screen_findings_decision_of(FindingsScreen *s, const char *sid, int round, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    Draft *d = key ? screen_findings_draft_find(s, sid, round, key) : NULL;
    return d && d->decision ? d->decision : "";
}

const char *screen_findings_reason_of(FindingsScreen *s, const char *sid, int round, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    Draft *d = key ? screen_findings_draft_find(s, sid, round, key) : NULL;
    return d && d->reason ? d->reason : "";
}

static Note *note_find(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->note_count; i++) if (s->notes[i].round == round && str_eq(s->notes[i].session_id, sid)) return &s->notes[i];
    return NULL;
}

void screen_findings_note_set(FindingsScreen *s, const char *sid, int round, const char *text) {
    Note *n = note_find(s, sid, round);
    if (!n) {
        s->notes = xrealloc(s->notes, (s->note_count + 1) * sizeof *s->notes);
        n = &s->notes[s->note_count++];
        memset(n, 0, sizeof *n);
        n->session_id = xstrdup(sid); n->round = round;
    }
    set_string(&n->note, text);
}

const char *screen_findings_note_of(FindingsScreen *s, const char *sid, int round) { Note *n = note_find(s, sid, round); return n && n->note ? n->note : ""; }

Replied *screen_findings_replied_find(FindingsScreen *s, const char *sid, const char *key) {
    for (size_t i = 0; i < s->replied_count; i++) if (str_eq(s->replied[i].session_id, sid) && str_eq(s->replied[i].key, key)) return &s->replied[i];
    return NULL;
}

Replied *screen_findings_replied_get(FindingsScreen *s, const char *sid, const char *key) {
    Replied *r = screen_findings_replied_find(s, sid, key);
    if (r) return r;
    s->replied = xrealloc(s->replied, (s->replied_count + 1) * sizeof *s->replied);
    r = &s->replied[s->replied_count++];
    memset(r, 0, sizeof *r);
    r->session_id = xstrdup(sid); r->key = xstrdup(key);
    return r;
}

void screen_findings_replied_clear(Replied *r) { free(r->session_id); free(r->key); free(r->url); free(r->error); }

void screen_findings_replied_drop(FindingsScreen *s, const char *sid, const char *key) {
    Replied *r = screen_findings_replied_find(s, sid, key);
    if (!r) return;
    screen_findings_replied_clear(r);
    *r = s->replied[--s->replied_count];
}

Card *screen_findings_card_find(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->card_count; i++) if (str_eq(s->cards[i].session_id, sid)) return &s->cards[i];
    return NULL;
}

Card *screen_findings_card_get(FindingsScreen *s, const char *sid) {
    Card *c = screen_findings_card_find(s, sid);
    if (c) return c;
    s->cards = xrealloc(s->cards, (s->card_count + 1) * sizeof *s->cards);
    c = &s->cards[s->card_count++];
    memset(c, 0, sizeof *c);
    c->session_id = xstrdup(sid);
    return c;
}

void screen_findings_card_clear(Card *c) { free(c->session_id); free(c->error); free(c->saved); free(c->saved_url); free(c->info); }

/// An edit after a save: the "Saved" line no longer describes what is on the screen.
void screen_findings_card_forget_saved(FindingsScreen *s, const char *sid) {
    Card *c = screen_findings_card_find(s, sid);
    if (c) { set_string(&c->saved, NULL); set_string(&c->saved_url, NULL); c->saved_failed = false; }
}

void screen_findings_outcome_clear(Outcome *o) { free(o->session_id); free(o->title); free(o->text); }

void screen_findings_outcome_set(FindingsScreen *s, const char *sid, const char *title, const char *text, bool danger) {
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

void screen_findings_outcome_drop(FindingsScreen *s, size_t i) {
    if (i >= s->outcome_count) return;
    screen_findings_outcome_clear(&s->outcomes[i]);
    memmove(&s->outcomes[i], &s->outcomes[i + 1], (s->outcome_count - i - 1) * sizeof *s->outcomes);
    s->outcome_count--;
}

static bool round_live(FindingsScreen *s, const char *sid, int round) { return screen_findings_find_round(s, sid, round) >= 0; }

static bool session_live(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->round_count; i++) if (str_eq(session_id(screen_findings_round_session(s, i)), sid)) return true;
    return false;
}

static bool finding_live(FindingsScreen *s, const char *sid, const char *key) {
    for (size_t i = 0; i < s->round_count; i++) {
        if (!str_eq(session_id(screen_findings_round_session(s, i)), sid)) continue;
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
        screen_findings_draft_clear(&s->drafts[i]); s->drafts[i] = s->drafts[--s->draft_count];
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
        screen_findings_replied_clear(&s->replied[i]); s->replied[i] = s->replied[--s->replied_count];
    }
    for (size_t i = 0; i < s->card_count;) {
        if (session_live(s, s->cards[i].session_id)) { i++; continue; }
        screen_findings_card_clear(&s->cards[i]); s->cards[i] = s->cards[--s->card_count];
    }
}

/// The drafts a round was saved with (Save comments) become this screen's drafts the first time the round is drawn here.
/// Only once per round: after that what was typed here is the truth, and a poll must not put back what has since changed.
static void seed(FindingsScreen *s) {
    for (size_t i = 0; i < s->round_count; i++) {
        const Session *ses = screen_findings_round_session(s, i);
        const Json *held = s->rounds[i].held;
        int round = screen_findings_round_number(held);
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
            if (!json_is_object(d) || screen_findings_draft_find(s, session_id(ses), round, fk)) continue;
            Draft *draft = screen_findings_draft_get(s, session_id(ses), round, fk);
            set_string(&draft->decision, json_str_nonempty(json_get(d, "decision")));
            set_string(&draft->reason, json_str_nonempty(json_get(d, "reason")));
        }
        const char *note = json_str_nonempty(json_get(saved, "note"));
        if (note && !note_find(s, session_id(ses), round)) screen_findings_note_set(s, session_id(ses), round, note);
    }
}

void screen_findings_drop_round_drafts(FindingsScreen *s, const char *sid, int round) {
    for (size_t i = 0; i < s->draft_count;) {
        if (!(s->drafts[i].round == round && str_eq(s->drafts[i].session_id, sid))) { i++; continue; }
        screen_findings_draft_clear(&s->drafts[i]); s->drafts[i] = s->drafts[--s->draft_count];
    }
    Note *n = note_find(s, sid, round);
    if (n) { free(n->session_id); free(n->note); *n = s->notes[--s->note_count]; }
}

void screen_findings_rebuild(FindingsScreen *s) {
    free(s->rounds);
    s->rounds = sessions_held_rounds(s->all, s->all_count, &s->round_count);
    prune(s); seed(s);
    s->doc_stale = true;
}
