#include "screen_new_session_internal.h"

static void remember(NewSessionScreen *s);
static void runtimes_done(void *owner, Request *req);
static void branches_done(void *owner, Request *req);
static int popup(NewSessionScreen *s, HMENU menu, const RECT *anchor);
static void append(HMENU menu, UINT id, const char *text, bool checked, bool enabled);

// MARK: - What the chips offer

bool screen_new_session_effective_choice(NewSessionScreen *s, RuntimeChoice *out) {
    if (s->has_runtime) { runtime_choice_copy(out, &s->runtime); return true; }
    if (s->has_catalog && s->catalog.has_default) { runtime_choice_copy(out, &s->catalog.def); return true; }
    return false;
}

/// Takes a project's catalog: a hand pick or else the last pick where this project still offers it; without a project
/// default a start needs a provider. The cached catalog comes first, so each catalog decides the screen's own picks afresh.
void screen_new_session_adopt(NewSessionScreen *s, RuntimeCatalog c) {
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    s->catalog = c; s->has_catalog = true;
    if (s->has_runtime) {
        RuntimeChoice kept = { 0 };
        bool keep = s->hand_picked && runtime_catalog_offered(&s->catalog, &s->runtime, &kept);
        runtime_choice_free(&s->runtime); s->has_runtime = false;
        if (keep) { s->runtime = kept; s->has_runtime = true; return; }
        s->hand_picked = false;
    }
    char *text = settings_read_last_runtime(); RuntimeChoice last;
    if (runtime_choice_from_saved(text, &last)) { s->has_runtime = runtime_catalog_offered(&s->catalog, &last, &s->runtime); runtime_choice_free(&last); }
    free(text);
    if (!s->has_runtime && !s->catalog.has_default) s->has_runtime = runtime_catalog_first_available(&s->catalog, &s->runtime);
}

/// A pick made by hand, kept for the next new session; the screen's own picks (the first available provider) are not.
static void remember(NewSessionScreen *s) {
    s->hand_picked = true;
    char *text = runtime_choice_saved(&s->runtime); settings_write_last_runtime(text); free(text);
}

static void runtimes_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    if (!req->ok || !screen_new_session_project(s)) return;
    RuntimeCatalog c;
    if (!runtime_catalog_parse(req->result, &c)) return;
    screen_new_session_adopt(s, c);
    char *key = xstrfmt("runtimes:%s", screen_new_session_project(s)->repo); cache_store(g_store.cache, req->result, key); free(key);
    pane_footer_changed(s->base.pane);
}

static void branches_done(void *owner, Request *req) {
    NewSessionScreen *s = owner;
    if (!req->ok || !screen_new_session_project(s)) return;
    str_array_free(s->branches, s->branch_count); s->branches = NULL; s->branch_count = 0;
    const Json *list = json_get(req->result, "branches");
    s->branches = xmalloc((json_count(list) ? json_count(list) : 1) * sizeof *s->branches);
    for (size_t i = 0; i < json_count(list); i++) { const char *b = json_str(json_at(list, i)); if (b) s->branches[s->branch_count++] = xstrdup(b); }
    set_string(&s->default_branch, json_str(json_get(req->result, "defaultBranch")));
    char *key = xstrfmt("branches:%s", screen_new_session_project(s)->repo); cache_store(g_store.cache, req->result, key); free(key);
    pane_footer_changed(s->base.pane);
}

/// Reads what the picked project offers: its runtimes and branches, the saved answer first.
void screen_new_session_load_choices(NewSessionScreen *s) {
    const Project *p = screen_new_session_project(s);
    request_cancel(&s->req_runtimes); request_cancel(&s->req_branches);
    if (s->has_catalog) { runtime_catalog_free(&s->catalog); s->has_catalog = false; }
    if (s->has_runtime) { runtime_choice_free(&s->runtime); s->has_runtime = false; }
    s->hand_picked = false;
    str_array_free(s->branches, s->branch_count); s->branches = NULL; s->branch_count = 0;
    set_string(&s->default_branch, NULL); set_string(&s->branch, NULL);
    if (s->workspace == WORKSPACE_LOCAL && (!p || !p->has_local)) s->workspace = WORKSPACE_WORKTREE;
    if (!p) return;
    if (store_supports("runtimes")) {
        char *key = xstrfmt("runtimes:%s", p->repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { RuntimeCatalog c; if (runtime_catalog_parse(saved, &c)) screen_new_session_adopt(s, c); json_free(saved); }
        free(key);
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("runtimes", args, 0, s, runtimes_done, 0, &s->req_runtimes);
    }
    if (store_supports("branches")) {
        char *key = xstrfmt("branches:%s", p->repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) {
            const Json *list = json_get(saved, "branches");
            s->branches = xmalloc((json_count(list) ? json_count(list) : 1) * sizeof *s->branches);
            for (size_t i = 0; i < json_count(list); i++) { const char *b = json_str(json_at(list, i)); if (b) s->branches[s->branch_count++] = xstrdup(b); }
            set_string(&s->default_branch, json_str(json_get(saved, "defaultBranch")));
            json_free(saved);
        }
        free(key);
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("branches", args, 0, s, branches_done, 0, &s->req_branches);
    }
}

// MARK: - Menus

static int popup(NewSessionScreen *s, HMENU menu, const RECT *anchor) {
    POINT pt = { anchor->left, anchor->bottom + px(4) };
    ClientToScreen(pane_hwnd(s->base.pane), &pt);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}

static void append(HMENU menu, UINT id, const char *text, bool checked, bool enabled) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
    free(w);
}

void screen_new_session_pick_chip(NewSessionScreen *s, int chip) {
    if (s->busy) return;
    const RECT *r = &s->chip_rc[chip];
    RuntimeChoice eff; bool has = screen_new_session_effective_choice(s, &eff);
    switch (chip) {
    case CHIP_WORKSPACE: {
        const Project *p = screen_new_session_project(s);
        bool can = p && p->has_local;
        HMENU m = CreatePopupMenu();
        append(m, WORKSPACE_WORKTREE, "\xE2\x8C\x97 Worktree: a fresh clone and database", s->workspace == WORKSPACE_WORKTREE, true);
        append(m, WORKSPACE_LOCAL, can ? "\xE2\x8C\x82 Local: the project's own checkout" : "\xE2\x8C\x82 Local: no local checkout set in Settings", s->workspace == WORKSPACE_LOCAL, can);
        append(m, WORKSPACE_ORCHESTRATOR, "\xF0\x9F\xA7\xAD Orchestrator: plan and coordinate workers", s->workspace == WORKSPACE_ORCHESTRATOR, true);
        int chosen = popup(s, m, r);
        if (chosen >= WORKSPACE_WORKTREE && chosen <= WORKSPACE_ORCHESTRATOR && (WorkspaceMode)chosen != s->workspace) {
            // Each mode starts with its own branch default; an orchestrator cannot take a branch at all.
            s->workspace = (WorkspaceMode)chosen; set_string(&s->branch, NULL);
            pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
        }
        break;
    }
    case CHIP_PROJECT: {
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < s->count; i++) append(m, (UINT)i + 1, project_title(&s->projects[i]), i == s->chosen, true);
        int chosen = popup(s, m, r);
        if (chosen > 0 && (size_t)chosen - 1 != s->chosen) { s->chosen = (size_t)chosen - 1; screen_new_session_load_choices(s); pane_relayout(s->base.pane); pane_footer_changed(s->base.pane); }
        break;
    }
    case CHIP_BRANCH: {
        HMENU m = CreatePopupMenu();
        char *first = screen_new_session_no_branch_label(s);
        append(m, 1, first, s->branch == NULL, true); free(first);
        if (s->branch_count) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < s->branch_count && i < 60; i++) append(m, (UINT)i + 2, s->branches[i], str_eq(s->branch, s->branches[i]), true);
        int chosen = popup(s, m, r);
        if (chosen == 1) set_string(&s->branch, NULL);
        else if (chosen >= 2 && (size_t)chosen - 2 < s->branch_count) set_string(&s->branch, s->branches[chosen - 2]);
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_PROVIDER: case CHIP_MODEL: {
        if (!s->has_catalog) break;
        HMENU m = CreatePopupMenu();
        UINT id = 1;
        struct { int provider; const char *model; } rows[128]; size_t n = 0;
        if (chip == CHIP_PROVIDER) {
            for (size_t p = 0; p < s->catalog.provider_count && n < 128; p++) {
                const RuntimeProvider *pr = &s->catalog.providers[p];
                char *label = runtime_provider_available(pr) ? xstrdup(pr->label) : xstrfmt("%s (unavailable)", pr->label);
                append(m, id++, label, has && eff.provider_id == pr->id, runtime_provider_available(pr)); free(label);
                rows[n].provider = pr->id; rows[n].model = pr->default_model ? pr->default_model : (pr->model_count ? pr->models[0].id : NULL); n++;
            }
        } else {
            const RuntimeProvider *pr = has ? runtime_catalog_provider(&s->catalog, eff.provider_id) : NULL;
            for (size_t k = 0; pr && k < pr->model_count && n < 128; k++) {
                append(m, id++, runtime_model_title(&pr->models[k]), has && str_eq(eff.model, pr->models[k].id), true);
                rows[n].provider = pr->id; rows[n].model = pr->models[k].id; n++;
            }
        }
        int chosen = popup(s, m, r);
        if (chosen >= 1 && (size_t)chosen - 1 < n) {
            RuntimeChoice c;
            if (runtime_catalog_choice(&s->catalog, rows[chosen - 1].provider, rows[chosen - 1].model, &c)) { if (s->has_runtime) runtime_choice_free(&s->runtime); s->runtime = c; s->has_runtime = true; remember(s); }
        }
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_EFFORT: {
        if (!has) break;
        size_t n; const char *const *efforts = runtime_catalog_efforts(&s->catalog, &eff, &n);
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < n; i++) append(m, (UINT)i + 1, efforts[i], str_eq(eff.effort, efforts[i]), true);
        int chosen = popup(s, m, r);
        if (chosen >= 1 && (size_t)chosen - 1 < n) {
            RuntimeChoice c = { eff.provider_id, xstrdup(eff.model), xstrdup(efforts[chosen - 1]) };
            if (s->has_runtime) runtime_choice_free(&s->runtime);
            s->runtime = c; s->has_runtime = true; remember(s);
        }
        pane_footer_changed(s->base.pane);
        break;
    }
    case CHIP_LOOP: s->review_loop = !s->review_loop; settings_write_review_loop(s->review_loop); pane_footer_changed(s->base.pane); break;
    }
    if (has) runtime_choice_free(&eff);
}
