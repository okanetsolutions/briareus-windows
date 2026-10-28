#include "saved_prompts_internal.h"

enum { PICK_SAVE = 1, PICK_EDIT, PICK_DELETE, PICK_RETRY, PICK_RESUME, PICK_ROW = 100 };
static void item(HMENU menu, UINT id, const char *text, bool enabled) {
    // Prompt titles are data, not menu mnemonics.
    char *escaped = str_replace(text, "&", "&&"); wchar_t *w = utf8_to_wide(escaped);
    AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, w); free(w); free(escaped);
}
static int popup(PromptPicker *p, HMENU menu, POINT pt) {
    HWND owner = pane_hwnd(p->owner->pane);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, owner, NULL);
    DestroyMenu(menu); return id;
}
static bool row_current(PromptPicker *p, const Json *row) {
    if (!prompt_picker_current(p) || !p->loaded || !p->rows) return false;
    if (p->epoch != json_num_or(json_get(prompt_picker_state(p), "epoch"), 0)) return false;
    for (size_t i = 0; i < json_count(p->rows); i++) if (json_equal(row, json_at(p->rows, i))) return true;
    return false;
}
void prompt_picker_insert_row(PromptPicker *p, const Json *row) {
    if (!row_current(p, row)) return;
    p->insert(p->ctx, json_str(json_get(row, "body")));
}
static bool dialog_current(PromptPicker *p, unsigned generation) {
    return prompt_picker_current(p) && p->generation == generation;
}
static void submit_dialog(PromptPicker *p) {
    const char *op = p->editing ? "update_prompt" : "create_prompt";
    if (!prompt_picker_writable(p, op)) return;
    if (p->editing && !row_current(p, p->original)) return;
    char *title = xstrdup(p->title), *body = xstrdup(p->body);
    unsigned generation = p->generation;
    bool ok = dialog_edit_item(app_dialog_owner(), p->editing ? "Edit saved prompt" : "Save as a prompt", &title, &body);
    if (!ok || !dialog_current(p, generation)) { free(title); free(body); return; }
    free(p->title); free(p->body); p->title = title; p->body = body;
    if (!p->editing && p->repo) {
        POINT pt; GetCursorPos(&pt); HMENU menu = CreatePopupMenu();
        item(menu, 1, "This project only", true); item(menu, 2, "Every project", true);
        int chosen = popup(p, menu, pt);
        if (!chosen || !dialog_current(p, generation)) return;
        p->everywhere = chosen == 2;
    }
    if (!dialog_current(p, generation)) return;
    Json *args = p->editing ? saved_prompt_edit(p->original, p->title, p->body)
        : saved_prompt_create(p->title, p->body, p->everywhere ? NULL : p->repo);
    if (!args) {
        set_string(&p->notice, "Enter a title (up to 120) and a body (up to 20000 UTF-16 units). Your edits are kept in Resume."); return;
    }
    if (!json_count(args)) { json_free(args); prompt_picker_clear_edit(p); return; }
    prompt_picker_write(p, op, args);
}
static void save(PromptPicker *p) {
    if (!prompt_picker_writable(p, "create_prompt")) return;
    // This form authors a new record, independent of the list currently loading.
    // Cancel that read so its completion cannot invalidate accepted dialog input.
    if (p->read) { request_cancel(&p->read); p->loaded = false; p->generation++; }
    char *text = p->draft(p->ctx);
    // The dialog can shorten an overlong authored draft; only reject a blank draft.
    char *joined = saved_prompt_insert(text, ""); bool nonempty = !str_empty(joined); free(joined);
    if (!nonempty) { free(text); return; }
    prompt_picker_clear_edit(p); p->title = xstrdup(""); p->body = text; p->editing = 0; p->everywhere = p->repo == NULL;
    submit_dialog(p);
}
static void edit(PromptPicker *p, const Json *row) {
    if (!prompt_picker_writable(p, "update_prompt") || !row_current(p, row)) return;
    int id = json_int_or(json_get(row, "id"), 0); if (id <= 0) return;
    // Clone before clearing the retained original, which may itself be this row.
    Json *original = json_clone(row); prompt_picker_clear_edit(p); p->original = original;
    p->title = json_dup_str(json_get(original, "title")); p->body = json_dup_str(json_get(original, "body")); p->editing = id;
    submit_dialog(p);
}
static void delete_prompt(PromptPicker *p, const Json *row) {
    if (!prompt_picker_writable(p, "delete_prompt") || !row_current(p, row)) return;
    int id = json_int_or(json_get(row, "id"), 0); if (id <= 0) return;
    Json *copy = json_clone(row); unsigned generation = p->generation;
    char *question = xstrfmt("Delete the prompt %s?", json_str(json_get(copy, "title")));
    bool yes = app_confirm(question, "This removes the saved prompt from its library.", "Delete", true); free(question);
    if (yes && dialog_current(p, generation) && row_current(p, copy)) {
        Json *args = json_object(); json_set_num(args, "id", id); prompt_picker_write(p, "delete_prompt", args);
    }
    json_free(copy);
}
static void end_modal(PromptPicker *p) {
    p->modal = false;
    if (p->retired) prompt_picker_release(p); else prompt_picker_changed(p);
}
void prompt_picker_save(PromptPicker *p) {
    if (!p || p->modal || !prompt_picker_current(p)) return;
    p->modal = true; save(p); end_modal(p);
}
void prompt_picker_edit(PromptPicker *p, const Json *row) {
    if (!p || p->modal || !prompt_picker_current(p)) return;
    p->modal = true; edit(p, row); end_modal(p);
}
void prompt_picker_delete(PromptPicker *p, const Json *row) {
    if (!p || p->modal || !prompt_picker_current(p)) return;
    p->modal = true; delete_prompt(p, row); end_modal(p);
}
static const Json *choose_row(PromptPicker *p, const Json *rows, POINT pt) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        item(menu, (UINT)i + 1, json_str(json_get(row, "title")), json_int_or(json_get(row, "id"), 0) > 0);
    }
    int selected = popup(p, menu, pt);
    return selected > 0 ? json_at(rows, (size_t)selected - 1) : NULL;
}
void prompt_picker_open(PromptPicker *p, POINT pt) {
    if (!p || p->modal || !prompt_picker_current(p)) return;
    if (p->epoch != json_num_or(json_get(prompt_picker_state(p), "epoch"), 0)) p->loaded = false;
    if (!p->loaded && !p->read) prompt_picker_read(p);
    p->modal = true; unsigned generation = p->generation;
    Json *rows = p->loaded && !p->error ? json_clone(p->rows) : NULL;
    HMENU menu = CreatePopupMenu();
    if (p->notice) item(menu, 0, p->notice, false);
    const Json *state = prompt_picker_state(p);
    if (json_bool_is(json_get(state, "uncertain"), true)) item(menu, 0, "Write outcome unknown. Inspect the refreshed library before reconnecting; do not repeat the write.", false);
    else if (json_bool_is(json_get(state, "pending"), true)) item(menu, 0, "Saving prompt...", false);
    if (p->read) item(menu, 0, "Loading saved prompts...", false);
    else if (p->error) { item(menu, 0, p->error, false); item(menu, PICK_RETRY, "Retry", true); }
    else if (!p->loaded) item(menu, PICK_RETRY, "Read paused; Retry after the server cooldown", true);
    else if (!json_count(rows)) item(menu, 0, "No saved prompts yet", false);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *r = json_at(rows, i);
        char *label = xstrfmt("%s%s", json_str_nonempty(json_get(r, "repo")) ? "" : "\xF0\x9F\x8C\x90 ", json_str(json_get(r, "title")));
        item(menu, PICK_ROW + (UINT)i, label, true); free(label);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    char *draft = p->draft(p->ctx); char *nonblank = saved_prompt_insert(draft, ""); free(draft);
    if (store_supports("create_prompt")) item(menu, PICK_SAVE, "Save the message as a prompt...", *nonblank && prompt_picker_writable(p, "create_prompt"));
    free(nonblank);
    if (store_supports("update_prompt")) item(menu, PICK_EDIT, "Edit a prompt...", json_count(rows) && prompt_picker_writable(p, "update_prompt"));
    if (store_supports("delete_prompt")) item(menu, PICK_DELETE, "Delete a prompt...", json_count(rows) && prompt_picker_writable(p, "delete_prompt"));
    if (p->editing >= 0) item(menu, PICK_RESUME, "Resume unsaved prompt...", prompt_picker_writable(p, p->editing ? "update_prompt" : "create_prompt"));
    int chosen = popup(p, menu, pt);
    if (dialog_current(p, generation)) {
        if (chosen >= PICK_ROW) prompt_picker_insert_row(p, json_at(rows, (size_t)(chosen - PICK_ROW)));
        else if (chosen == PICK_RETRY) prompt_picker_read(p);
        else if (chosen == PICK_SAVE) save(p);
        else if (chosen == PICK_RESUME) submit_dialog(p);
        else if (chosen == PICK_EDIT || chosen == PICK_DELETE) {
            const Json *row = choose_row(p, rows, pt);
            if (row && dialog_current(p, generation)) { if (chosen == PICK_EDIT) edit(p, row); else delete_prompt(p, row); }
        }
    }
    json_free(rows); end_modal(p);
}
