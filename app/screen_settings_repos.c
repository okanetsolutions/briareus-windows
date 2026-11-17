#include "screen_settings_internal.h"

typedef struct { char *text; bool on, gone, enabled; } ForgeCheck;

static bool repos_match(const char *a, const char *b, SettingsReposMatch match) {
    return match == SETTINGS_REPOS_IGNORE_CASE ? str_ieq(a, b) : str_eq(a, b);
}

static bool repos_has(const Json *repos, const char *repo, SettingsReposMatch match) {
    for (size_t i = 0; i < json_count(repos); i++) if (repos_match(json_str(json_at(repos, i)), repo, match)) return true;
    return false;
}

/// Whether two project lists hold the same projects, in any order.
bool screen_settings_repos_same(const Json *a, const Json *b, SettingsReposMatch match) {
    if (json_count(a) != json_count(b)) return false;
    for (size_t i = 0; i < json_count(a); i++) if (!repos_has(b, json_str(json_at(a, i)), match)) return false;
    return true;
}

/// What the Projects list offers: every project, then any ticked one that is no longer a project (the server refuses it
/// on save, so it stays visible to be unticked).
Json *screen_settings_repos_choices(const Json *repos, SettingsReposMatch match) {
    Json *out = json_array();
    const Json *rows = screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        const char *repo = json_str_nonempty(json_get(json_at(rows, i), "repo"));
        if (repo && !repos_has(out, repo, match)) json_array_push(out, json_string(repo));
    }
    for (size_t i = 0; i < json_count(repos); i++) {
        const char *repo = json_str(json_at(repos, i));
        if (repo && !repos_has(out, repo, match)) json_array_push(out, json_string(repo));
    }
    return out;
}

static void forge_check_free(void *p) { ForgeCheck *d = p; free(d->text); free(d); }

static void paint_forge_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ForgeCheck *d = it->data;
    bool hovered = d->enabled && doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), d->on ? theme.accent : theme.field, d->on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (d->on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, d->gone || !d->enabled ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// The projects the account is available to, one tick box each, as the web form's multiple select.
void screen_settings_repos_layout(Doc *doc, int x, int w, const Json *repos, int action, const char *hint, SettingsReposMatch match) {
    doc_field_label(doc, x, w, "Projects", theme.ink, hint);
    doc_space(doc, px(6));
    Json *choices = screen_settings_repos_choices(repos, match);
    const Json *rows = screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL;
    for (size_t i = 0; i < json_count(choices); i++) {
        const char *repo = json_str(json_at(choices, i));
        bool listed = false;
        for (size_t k = 0; k < json_count(rows); k++) listed |= repos_match(json_str(json_get(json_at(rows, k), "repo")), repo, match);
        ForgeCheck *d = xcalloc(1, sizeof *d);
        d->on = repos_has(repos, repo, match); d->gone = !listed; d->enabled = action != 0;
        d->text = listed ? xstrdup(repo) : xstrfmt("%s (not a project)", repo);
        int it = doc_custom(doc, x, w, px(28), paint_forge_check, d, forge_check_free, action, (intptr_t)i);
        doc_item(doc, it)->hand = action != 0;
    }
    if (!json_count(choices)) doc_text(doc, x, w, "No projects yet. Add one under Projects first.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    json_free(choices);
    doc_space(doc, px(14));
}

bool screen_settings_repos_toggle(Json **repos, size_t index, SettingsReposMatch match) {
    Json *choices = screen_settings_repos_choices(*repos, match);
    const char *repo = json_str(json_at(choices, index));
    bool changed = repo != NULL;
    if (repo) {
        Json *next = json_array();
        for (size_t i = 0; i < json_count(*repos); i++) {
            const char *r = json_str(json_at(*repos, i));
            if (r && !repos_match(r, repo, match)) json_array_push(next, json_string(r));
        }
        if (!repos_has(*repos, repo, match)) json_array_push(next, json_string(repo));
        json_free(*repos); *repos = next;
    }
    json_free(choices);
    return changed;
}
