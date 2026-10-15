#include "screen_conversation_internal.h"

static const char *decision_for(ConversationScreen *s, const Json *triage, const Json *finding);

// MARK: - Layout

bool screen_conversation_is_expanded(ConversationScreen *s, int seq) { for (size_t i = 0; i < s->expanded_count; i++) if (s->expanded[i] == seq) return true; return false; }

void screen_conversation_toggle_expanded(ConversationScreen *s, int seq) {
    for (size_t i = 0; i < s->expanded_count; i++) if (s->expanded[i] == seq) { s->expanded[i] = s->expanded[--s->expanded_count]; return; }
    s->expanded = xrealloc(s->expanded, (s->expanded_count + 1) * sizeof *s->expanded);
    s->expanded[s->expanded_count++] = seq;
}

static const char *decision_for(ConversationScreen *s, const Json *triage, const Json *finding) {
    const char *key = json_str(json_get(finding, "key"));
    if (!key) return "";
    size_t kl = strlen(key);
    for (size_t i = 0; i < s->decision_count; i++) if (strncmp(s->decisions[i], key, kl) == 0 && s->decisions[i][kl] == '=') return s->decisions[i] + kl + 1;
    const char *d = json_str(json_get(json_get(json_get(json_get(triage, "drafts"), "verdicts"), key), "decision"));
    return d ? d : "";
}

void screen_conversation_set_decision(ConversationScreen *s, const char *key, const char *decision) {
    size_t kl = strlen(key);
    char *entry = xstrfmt("%s=%s", key, decision);
    for (size_t i = 0; i < s->decision_count; i++) if (strncmp(s->decisions[i], key, kl) == 0 && s->decisions[i][kl] == '=') { free(s->decisions[i]); s->decisions[i] = entry; return; }
    s->decisions = xrealloc(s->decisions, (s->decision_count + 1) * sizeof *s->decisions);
    s->decisions[s->decision_count++] = entry;
}

void screen_conversation_layout_triage(ConversationScreen *s, Doc *doc, int x, int w, const Json *triage) {
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(12), iw = w - px(24);
    Str title; str_init(&title);
    double round, pr;
    if (json_num(json_get(triage, "round"), &round)) str_appendf(&title, "Round %d findings", (int)round); else str_appendz(&title, "Review findings");
    if (json_num(json_get(triage, "prNumber"), &pr)) str_appendf(&title, " \xC2\xB7 PR #%d", (int)pr);
    doc_text(doc, ix, iw, title.data, FONT_BODY_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    str_free(&title);
    const Json *findings = json_get(triage, "findings");
    size_t n = json_count(findings), fixes = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *f = json_at(findings, i);
        doc_space(doc, px(8)); doc_rule(doc, ix, iw); doc_space(doc, px(8));
        const char *ft = json_str(json_get(f, "title"));
        int ti = doc_text(doc, ix, iw, ft ? ft : "Finding", FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
        const char *url = json_str(json_get(f, "url"));
        if (safe_web_url(url)) { doc_item(doc, ti)->action = ACT_FINDING_LINK; doc_item(doc, ti)->arg = (intptr_t)i; doc_item(doc, ti)->hand = true; }
        Str meta; str_init(&meta);
        if (json_str(json_get(f, "file"))) { str_appendz(&meta, json_str(json_get(f, "file"))); double line; if (json_num(json_get(f, "line"), &line)) str_appendf(&meta, ":%d", (int)line); }
        if (meta.len) { doc_space(doc, px(3)); doc_text(doc, ix, iw, meta.data, FONT_MONO_CAPTION2, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS); }
        str_free(&meta);
        const char *why = json_str(json_get(f, "parkedWhy"));
        if (why) { doc_space(doc, px(3)); doc_text(doc, ix, iw, why, FONT_CAPTION, theme.muted, DT_WORDBREAK); }
        if (takes_verdicts && json_str(json_get(f, "key"))) {
            const char *decision = decision_for(s, triage, f);
            int selected = finding_decision_index(decision);
            if (selected == 0) fixes++;
            doc_space(doc, px(6));
            doc_segments(doc, ix, iw, finding_decision_titles, FINDING_DECISION_COUNT, selected, ACT_TRIAGE_DECISION, (intptr_t)(i * 4), !s->busy && !s->uncertain);
        }
    }
    doc_space(doc, px(8)); doc_rule(doc, ix, iw); doc_space(doc, px(8));
    if (takes_verdicts) {
        int nb = doc_box_begin(doc, ix, iw, px(3), theme.field, theme.line, px(4));
        doc_text(doc, ix + px(6), iw - px(12), str_empty(s->triage_note) ? "A note for the pull request and the fix session (optional)" : s->triage_note, FONT_CAPTION2, str_empty(s->triage_note) ? theme.muted : theme.ink, DT_WORDBREAK);
        doc_box_end(doc, nb, px(3));
        doc_box_action(doc, nb, ACT_TRIAGE_NOTE, 0);
        doc_space(doc, px(8));
    }
    char *complete = takes_verdicts ? (fixes ? xstrfmt("Complete \xC2\xB7 send %zu to be fixed", fixes) : xstrdup("Complete \xC2\xB7 nothing to fix, approve and close")) : xstrdup("Complete");
    ButtonSpec b = { 0, complete, BUTTON_PROMINENT, ACT_TRIAGE_COMPLETE, (intptr_t)fixes, !s->busy && !s->uncertain };
    doc_button_row(doc, ix, iw, &b, 1);
    free(complete);
    doc_box_end(doc, box, px(10));
}

void screen_conversation_complete_triage(ConversationScreen *s, size_t fixes) {
    const Json *triage = session_held_triage(screen_conversation_session(s));
    if (!triage) return;
    bool takes_verdicts = json_bool_tristate(json_get(triage, "mine")) != 0;
    char *title;
    if (!takes_verdicts) title = xstrdup("Take this review off the queue?");
    else if (!fixes) title = xstrdup("Complete with nothing to fix?");
    else title = xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s");
    s->dialog_open = true;
    bool ok = app_confirm(title, NULL, "Complete", false);
    s->dialog_open = false;
    free(title);
    if (!ok) return;
    Json *extra = json_object();
    if (takes_verdicts) {
        Json *verdicts = json_array();
        const Json *findings = json_get(triage, "findings");
        for (size_t i = 0; i < json_count(findings); i++) {
            const Json *f = json_at(findings, i);
            const char *key = json_str(json_get(f, "key"));
            if (!key) continue;
            const char *chosen = decision_for(s, triage, f);
            Json *v = json_object();
            json_set_str(v, "key", key); json_set_str(v, "decision", str_empty(chosen) ? "optional" : chosen);
            const char *reason = json_str(json_get(json_get(json_get(json_get(triage, "drafts"), "verdicts"), key), "reason"));
            if (!str_empty(reason)) json_set_str(v, "reason", reason);
            json_array_push(verdicts, v);
        }
        if (json_count(verdicts)) json_object_set(extra, "verdicts", verdicts); else json_free(verdicts);
        char *note = str_trim(s->triage_note);
        if (*note) json_set_str(extra, "note", note);
        free(note);
    }
    screen_conversation_mutate(s, "complete_findings", extra);
}
