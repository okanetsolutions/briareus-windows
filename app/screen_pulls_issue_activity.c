#include "screen_pulls_internal.h"

static bool event_words(const Json *e, const char *repo, FlowData *f, wchar_t *glyph, COLORREF *color);
static bool event_has_target(const Json *e);
static void issue_layout_activity(IssueScreen *s, Doc *doc, Col c);

/// An event as GitHub words it after its actor, with its badge. False for a kind this screen does not draw.
static bool event_words(const Json *e, const char *repo, FlowData *f, wchar_t *glyph, COLORREF *color) {
    const char *kind = json_str(json_get(e, "kind"));
    *glyph = 0xE8EC; *color = theme.muted;
    if (str_eq(kind, "labeled") || str_eq(kind, "unlabeled")) {
        const char *name = json_str(json_get(json_get(e, "label"), "name"));
        screen_pulls_flow_muted(f, str_eq(kind, "labeled") ? "added the " : "removed the ");
        if (name) screen_pulls_flow_add(f, name, FONT_CAPTION_SEMIBOLD, theme.accent, true);
        screen_pulls_flow_muted(f, "label ");
    } else if (str_eq(kind, "assigned") || str_eq(kind, "unassigned")) {
        const char *who = json_str(json_get(e, "assignee"));
        bool self = str_ieq(who, json_str(json_get(e, "actor")));
        *glyph = 0xE77B;
        if (str_eq(kind, "assigned")) { if (self) screen_pulls_flow_muted(f, "self-assigned this "); else { screen_pulls_flow_muted(f, "assigned "); screen_pulls_flow_strong(f, who); } }
        else if (self) screen_pulls_flow_muted(f, "removed their assignment ");
        else { screen_pulls_flow_muted(f, "unassigned "); screen_pulls_flow_strong(f, who); }
    } else if (str_eq(kind, "milestoned") || str_eq(kind, "demilestoned")) {
        *glyph = 0xE7C1;
        screen_pulls_flow_muted(f, str_eq(kind, "milestoned") ? "added this to the " : "removed this from the ");
        screen_pulls_flow_strong(f, json_str(json_get(e, "milestone")));
        screen_pulls_flow_muted(f, "milestone ");
    } else if (str_eq(kind, "renamed")) {
        *glyph = 0xE70F;
        const char *from = json_str(json_get(e, "from")), *to = json_str(json_get(e, "to"));
        screen_pulls_flow_muted(f, "changed the title ");
        if (from) { char *t = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D ", from); screen_pulls_flow_add(f, t, FONT_FOOTNOTE, theme.secondary, false); free(t); }
        screen_pulls_flow_muted(f, "to ");
        if (to) { char *t = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D ", to); screen_pulls_flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(t); }
    } else if (str_eq(kind, "closed")) {
        const char *reason = json_str(json_get(e, "stateReason"));
        bool aside = str_eq(reason, "not_planned") || str_eq(reason, "duplicate");
        *glyph = aside ? 0xE711 : 0xE73E; *color = aside ? theme.muted : theme.accent;
        screen_pulls_flow_muted(f, str_eq(reason, "not_planned") ? "closed this as not planned " : str_eq(reason, "duplicate") ? "closed this as a duplicate " : str_eq(reason, "completed") ? "closed this as completed " : "closed this ");
    } else if (str_eq(kind, "reopened")) {
        *glyph = 0xE72C; *color = theme.ok;
        screen_pulls_flow_muted(f, "reopened this ");
    } else if (str_eq(kind, "cross-referenced")) {
        *glyph = 0xE71B;
        screen_pulls_flow_muted(f, "mentioned this in ");
        screen_pulls_flow_ref(f, json_get(e, "source"), repo);
    } else if (str_eq(kind, "connected") || str_eq(kind, "disconnected")) {
        *glyph = 0xE71B;
        screen_pulls_flow_muted(f, str_eq(kind, "connected") ? "linked a pull request that will close this issue " : "removed a link to a pull request ");
        screen_pulls_flow_ref(f, json_get(e, "source"), repo);
    } else if (str_eq(kind, "referenced")) {
        const Json *commit = json_get(e, "commit");
        const char *sha = json_str(json_get(commit, "sha")), *message = json_str(json_get(commit, "message"));
        *glyph = 0xE8EE;
        screen_pulls_flow_muted(f, "referenced this in commit ");
        if (sha) { char *short_sha = xstrndup(sha, strlen(sha) > 7 ? 7 : strlen(sha)); screen_pulls_flow_add(f, short_sha, FONT_MONO_CAPTION2, theme.accent, true); free(short_sha); }
        if (message) { char *m = xstrfmt("%s ", message); screen_pulls_flow_add(f, m, FONT_FOOTNOTE, theme.ink, false); free(m); }
    } else if (str_eq(kind, "parent_issue_added") || str_eq(kind, "parent_issue_removed")) {
        *glyph = 0xE71B;
        screen_pulls_flow_muted(f, str_eq(kind, "parent_issue_added") ? "added a parent issue " : "removed a parent issue ");
        screen_pulls_flow_ref(f, json_get(e, "issue"), repo);
    } else if (str_eq(kind, "sub_issue_added") || str_eq(kind, "sub_issue_removed")) {
        *glyph = 0xE71B;
        screen_pulls_flow_muted(f, str_eq(kind, "sub_issue_added") ? "added a sub-issue " : "removed a sub-issue ");
        screen_pulls_flow_ref(f, json_get(e, "issue"), repo);
    } else if (str_eq(kind, "issue_type_added") || str_eq(kind, "issue_type_removed") || str_eq(kind, "issue_type_changed")) {
        const char *type = json_str(json_get(e, "type")), *before = json_str(json_get(e, "previousType"));
        if (str_eq(kind, "issue_type_changed")) { screen_pulls_flow_muted(f, "changed the issue type from "); screen_pulls_flow_strong(f, before); screen_pulls_flow_muted(f, "to "); screen_pulls_flow_strong(f, type); }
        else { screen_pulls_flow_muted(f, str_eq(kind, "issue_type_added") ? "added the " : "removed the "); screen_pulls_flow_strong(f, type); screen_pulls_flow_muted(f, "issue type "); }
    } else if (str_eq(kind, "added_to_project_v2") || str_eq(kind, "removed_from_project_v2") || str_eq(kind, "project_v2_item_status_changed")) {
        const char *project = json_str(json_get(e, "project"));
        *glyph = 0xE8FD;
        if (str_eq(kind, "project_v2_item_status_changed")) {
            const char *before = json_str(json_get(e, "previousStatus")), *after = json_str(json_get(e, "status"));
            screen_pulls_flow_muted(f, "moved this ");
            if (before) { screen_pulls_flow_muted(f, "from "); screen_pulls_flow_strong(f, before); }
            screen_pulls_flow_muted(f, "to "); screen_pulls_flow_strong(f, after ? after : "No status");
        } else screen_pulls_flow_muted(f, str_eq(kind, "added_to_project_v2") ? "added this to " : "removed this from ");
        if (project) { if (str_eq(kind, "project_v2_item_status_changed")) screen_pulls_flow_muted(f, "in "); screen_pulls_flow_strong(f, project); }
        else if (!str_eq(kind, "project_v2_item_status_changed")) screen_pulls_flow_muted(f, "a project ");
    } else return false;
    return true;
}

/// Where clicking an event goes: a comment's place on GitHub, the issue, pull request or commit it points to.
static bool event_has_target(const Json *e) {
    if (str_eq(json_str(json_get(e, "kind")), "commented")) return safe_web_url(json_str(json_get(e, "url")));
    double n;
    if (json_num(json_get(json_get(e, "source"), "number"), &n) || json_num(json_get(json_get(e, "issue"), "number"), &n)) return true;
    return safe_web_url(json_str(json_get(json_get(e, "commit"), "url")));
}

void screen_pulls_event_open(IssueScreen *s, const Json *e) {
    if (str_eq(json_str(json_get(e, "kind")), "commented")) { open_web_url(json_str(json_get(e, "url"))); return; }
    const Json *source = json_get(e, "source"), *ref = json_is_set(source) ? source : json_get(e, "issue");
    BoardLink link;
    if (board_link_parse(ref, &link)) {
        if (str_eq(json_str(json_get(ref, "kind")), "pull")) screen_pulls_issue_open_pull(s, &link); else screen_pulls_issue_open_link(s, &link);
        board_link_free(&link);
        return;
    }
    open_web_url(json_str(json_get(json_get(e, "commit"), "url")));
}

static void issue_layout_activity(IssueScreen *s, Doc *doc, Col c) {
    // The timeline is read once the issue is; when that read failed, there is none coming.
    if (!store_supports("issue_timeline") || (!s->events && !s->req_timeline && s->detail_error)) return;
    doc_section(doc, c.x, c.w, "Activity");
    if (s->timeline_error) { doc_notice(doc, c.x, c.w, s->timeline_error); doc_space(doc, px(10)); }
    if (!s->events) { if (!s->timeline_error) doc_loading(doc, c.x, c.w, "Loading the timeline\xE2\x80\xA6"); return; }
    size_t drawn = 0;
    for (size_t i = 0; i < json_count(s->events); i++) {
        const Json *e = json_at(s->events, i);
        const char *actor = json_str(json_get(e, "actor"));
        time_t at; bool dated = board_date_parse(json_str(json_get(e, "createdAt")), &at);
        int action = event_has_target(e) ? ACT_ISSUE_EVENT : 0;
        if (str_eq(json_str(json_get(e, "kind")), "commented")) {
            if (drawn) doc_space(doc, px(12));
            CommentHeadData *h = xcalloc(1, sizeof *h);
            h->author = xstrdup(actor ? actor : "ghost");
            if (dated) { char *rel = format_relative(at); h->when = xstrfmt("commented \xC2\xB7 %s", rel); free(rel); }
            else h->when = xstrdup("commented");
            int ix, iw, box = screen_pulls_comment_begin(doc, c, h, action, (intptr_t)i, &ix, &iw);
            char *body = visible_markdown(json_str(json_get(e, "body")) ? json_str(json_get(e, "body")) : "");
            if (*body) doc_markdown(doc, ix, iw, body, FONT_CALLOUT);
            else doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
            free(body);
            doc_box_end(doc, box, px(14));
            drawn++;
            continue;
        }
        FlowData *f = screen_pulls_flow_new(px(22));
        screen_pulls_flow_strong(f, actor ? actor : "ghost");
        wchar_t glyph; COLORREF color;
        if (!event_words(e, s->project.repo, f, &glyph, &color)) { screen_pulls_flow_free(f); continue; }
        if (dated) { char *rel = format_relative(at); screen_pulls_flow_muted(f, rel); free(rel); }
        doc_space(doc, px(drawn ? 10 : 2));
        int y = doc->y;
        EventBadge *b = xcalloc(1, sizeof *b); b->glyph = glyph; b->color = color;
        int bi = doc_custom(doc, c.x + px(14), px(26), px(26), screen_pulls_paint_event_badge, b, free, 0, 0);
        doc_item(doc, bi)->hover_fill = false;
        doc->y = y + px(2);
        int fi = screen_pulls_doc_flow(doc, c.x + px(50), c.w - px(50), 0, f);
        if (action) { doc_item(doc, fi)->action = action; doc_item(doc, fi)->arg = (intptr_t)i; doc_item(doc, fi)->hand = true; }
        if (doc->y < y + px(26)) doc->y = y + px(26);
        drawn++;
    }
    if (!drawn) doc_text(doc, c.x, c.w, "No activity yet", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    if (s->next_page) {
        doc_space(doc, px(14));
        doc_button(doc, c.x, 0, s->req_timeline ? "Loading\xE2\x80\xA6" : "Show more activity", BUTTON_BORDERED, ACT_ISSUE_MORE, 0, !s->req_timeline);
    }
}

void screen_pulls_issue_layout_main(IssueScreen *s, Doc *doc, Col c) {
    screen_pulls_issue_layout_body(s, doc, c);
    screen_pulls_issue_layout_subs(s, doc, c);
    screen_pulls_issue_layout_runs(s, doc, c);
    issue_layout_activity(s, doc, c);
}
