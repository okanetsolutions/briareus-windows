#include "saved_prompts_internal.h"
#include <math.h>

static double now(void) { return (double)GetTickCount64() / 1000; }
static bool account(ApiClient *client, const Json *device) {
    if (client != g_store.client || !g_store.has_device) return false;
    Json *current = device_json(&g_store.device); bool same = json_equal(current, device); json_free(current); return same;
}
bool prompt_picker_current(PromptPicker *p) {
    if (p->retired || !p->shown || !account(p->client, p->device)) return false;
    if (!store_supports("prompts")) return false;
    if (!saved_prompt_scope_allowed(g_store.device.permission, g_store.device.repos, g_store.device.repo_count, p->repo)) return false;
    return p->current(p->ctx, p->repo, p->identity);
}
const Json *prompt_picker_state(PromptPicker *p) { return json_get(g_store.prompt_state, p->key); }
bool prompt_picker_offered(PromptPicker *p) { return p && prompt_picker_current(p); }
bool prompt_picker_writable(PromptPicker *p, const char *operation) {
    if (!prompt_picker_current(p) || p->write || !store_supports(operation)) return false;
    // Defense even against a catalog accidentally advertising weaker access for writes.
    if (!str_eq(g_store.device.permission, "admin")) return false;
    const Json *state = prompt_picker_state(p);
    if (json_bool_is(json_get(state, "pending"), true) || json_bool_is(json_get(state, "uncertain"), true)) return false;
    return now() >= json_num_or(json_get(state, "until"), 0);
}
static Json *state(const char *key) {
    if (!g_store.prompt_state) g_store.prompt_state = json_object();
    if (!json_is_object(json_get(g_store.prompt_state, key))) json_object_set(g_store.prompt_state, key, json_object());
    return (Json *)json_get(g_store.prompt_state, key);
}
void prompt_picker_changed(PromptPicker *p) { if (prompt_picker_current(p) && p->owner->pane) pane_footer_changed(p->owner->pane); }
void prompt_picker_clear_edit(PromptPicker *p) {
    free(p->title); free(p->body); json_free(p->original); p->title = p->body = NULL; p->original = NULL; p->editing = -1;
}
void prompt_picker_release(PromptPicker *p) {
    json_free(p->rows); json_free(p->device); api_client_release(p->client);
    prompt_picker_clear_edit(p); free(p->error); free(p->notice); free(p->repo); free(p->identity); free(p->key); free(p);
}
void prompt_picker_free(PromptPicker *p) {
    if (!p) return;
    p->retired = true; p->shown = false; request_cancel(&p->read); request_cancel(&p->write);
    if (!p->modal) prompt_picker_release(p);
}
void prompt_picker_visible(PromptPicker *p, bool shown) {
    if (!p) return;
    p->shown = shown; p->generation++;
    if (!shown) { request_cancel(&p->read); p->loaded = false; json_free(p->rows); p->rows = NULL; }
}
PromptPicker *prompt_picker_new(Screen *owner, const char *repo, const char *identity,
    PromptOwnerCurrent current, PromptDraft draft, PromptInsert insert, void *ctx) {
    PromptPicker *p = xcalloc(1, sizeof *p); p->owner = owner; p->ctx = ctx;
    p->client = api_client_retain(g_store.client); p->device = device_json(&g_store.device);
    p->key = json_serialize(p->device, true); p->repo = xstrdup(repo); p->identity = xstrdup(identity);
    p->current = current; p->draft = draft; p->insert = insert; p->editing = -1; return p;
}
// A finalizer survives navigation and records cooldown/interlocks even when the owner
// was cancelled. It never applies an old account's result to a replacement connection.
typedef struct { ApiClient *client; Json *device; char *key; bool write; } Completion;
static bool write_answer(Request *r) {
    if (!r->ok) return false;
    if (str_eq(r->operation, "delete_prompt")) return json_bool_is(json_get(r->result, "ok"), true);
    const Json *p = json_get(r->result, "prompt");
    if (json_int_or(json_get(p, "id"), 0) <= 0) return false;
    if (str_eq(r->operation, "update_prompt") && !json_equal(json_get(p, "id"), json_get(r->args, "id"))) return false;
    return json_str_nonempty(json_get(p, "title")) && json_str(json_get(p, "body"));
}
static void completed(void *unused, Request *r) {
    (void)unused; Completion *c = r->completion_ctx;
    if (account(c->client, c->device)) {
        Json *s = state(c->key);
        double retry = r->error.retry_after;
        if (r->error.status == 429 && retry < 0) retry = 60;
        if (!r->ok) json_set_num(s, "until", fmax(json_num_or(json_get(s, "until"), 0), now() + fmax(2, retry)));
        if (c->write) {
            json_set_bool(s, "pending", false);
            json_set_bool(s, "uncertain", !write_answer(r) && (r->ok || request_outcome_unknown(r)));
            // Invalidate all open scoped lists, including on a refusal or uncertain result.
            json_set_num(s, "epoch", json_num_or(json_get(s, "epoch"), 0) + 1);
        }
        if (api_error_unauthorized(&r->error)) { r->auth_checked = true; store_invalidate_credentials(&r->error); }
    } else r->auth_checked = true;
    api_client_release(c->client); json_free(c->device); free(c->key); free(c); r->completion_ctx = NULL;
}
static void attach(PromptPicker *p, Request *r, bool write) {
    Completion *c = xcalloc(1, sizeof *c); c->client = api_client_retain(p->client);
    c->device = json_clone(p->device); c->key = xstrdup(p->key); c->write = write;
    r->completed = completed; r->completion_ctx = c;
}
static void read_done(void *ctx, Request *r) {
    PromptPicker *p = ctx;
    if (!prompt_picker_current(p)) return;
    double epoch = json_num_or(json_get(prompt_picker_state(p), "epoch"), 0);
    if (epoch != p->epoch) { p->loaded = false; return; }
    Json *rows = r->ok ? saved_prompt_list(r->result, p->repo) : NULL;
    json_free(p->rows); p->rows = rows; p->loaded = true;
    set_string(&p->error, NULL);
    if (!rows) { p->error = request_error_or_unexpected(r); }
    p->generation++; prompt_picker_changed(p);
}
void prompt_picker_read(PromptPicker *p) {
    if (!prompt_picker_current(p) || p->read || p->write) return;
    if (now() < json_num_or(json_get(prompt_picker_state(p), "until"), 0)) return;
    p->epoch = json_num_or(json_get(prompt_picker_state(p), "epoch"), 0);
    p->loaded = false; p->generation++; json_free(p->rows); p->rows = NULL; set_string(&p->error, NULL);
    Json *args = json_object(); json_set_str(args, "repo", p->repo);
    Request *r = store_call("prompts", args, 0, p, read_done, 0, &p->read); attach(p, r, false);
}
static void write_done(void *ctx, Request *r) {
    PromptPicker *p = ctx;
    if (!prompt_picker_current(p)) return;
    if (write_answer(r)) { prompt_picker_clear_edit(p); set_string(&p->notice, NULL); }
    else { set_string(&p->notice, NULL); p->notice = request_error_or_unexpected(r); }
    p->loaded = false; json_free(p->rows); p->rows = NULL; p->generation++;
    // Reads are safe after uncertain writes too, but never resubmit the write.
    prompt_picker_read(p); prompt_picker_changed(p);
}
void prompt_picker_write(PromptPicker *p, const char *operation, Json *args) {
    if (!prompt_picker_writable(p, operation)) { json_free(args); return; }
    request_cancel(&p->read); p->loaded = false; p->generation++;
    json_set_bool(state(p->key), "pending", true);
    Request *r = store_call(operation, args, 0, p, write_done, 0, &p->write); attach(p, r, true);
}
