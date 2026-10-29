// UI-thread queue ownership; workers own only the active source and an atomic cancellation flag.
#include "attach_list_internal.h"
#include "screens.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static AttachList *lists;
static void advance(AttachList *l);

void attach_list_changed(AttachList *l) { if (!l->retired && l->screen->pane) pane_footer_changed(l->screen->pane); }
static void release(AttachList *l) {
    if (--l->refs) return;
    api_client_release(l->client); json_free(l->device); free(l->items); free(l->scope); free(l->call); free(l);
}
static void item_free(Attachment *a) {
    attach_file_clear(&a->source); free(a->name); free(a->id); free(a->error); free(a);
}
void attach_list_remove(AttachList *l, size_t i) {
    if (i >= l->count) return;
    Attachment *a = l->items[i];
    InterlockedExchange(&a->cancelled, 1); request_cancel(&a->req);
    memmove(&l->items[i], &l->items[i + 1], (l->count - i - 1) * sizeof *l->items); l->count--;
    if (a != l->active) item_free(a);
    attach_list_changed(l);
}
static void clear(AttachList *l) {
    if (l->attacher) { attacher_free(l->attacher); l->attacher = NULL; }
    while (l->count) attach_list_remove(l, l->count - 1);
}
void attach_lists_disconnect(void) { for (AttachList *l = lists; l; l = l->next) clear(l); }
static bool account_current(AttachList *l) {
    if (l->client != g_store.client || !g_store.has_device) return false;
    Json *device = device_json(&g_store.device);
    bool same = json_equal(device, l->device); json_free(device);
    return same && store_supports_attachments_on(l->call);
}
void attach_list_sync(AttachList *l) { if (!account_current(l)) clear(l); }
void attach_list_scope(AttachList *l, const char *scope) {
    if (str_eq(l->scope, scope)) return;
    clear(l); set_string(&l->scope, scope);
}
AttachList *attach_list_new(Screen *screen, const char *call) {
    AttachList *l = xcalloc(1, sizeof *l); l->refs = 1;
    l->screen = screen; l->call = xstrdup(call);
    l->next = lists; lists = l;
    return l;
}
void attach_list_free(AttachList *l) {
    if (!l) return;
    AttachList **link = &lists;
    while (*link && *link != l) link = &(*link)->next;
    if (*link) *link = l->next;
    l->retired = true; clear(l); release(l);
}
bool attach_list_supported(const AttachList *l) { return store_supports_attachments_on(l->call); }
size_t attach_list_count(const AttachList *l) { attach_list_sync((AttachList *)l); return l->count; }
bool attach_list_uploading(const AttachList *l) {
    attach_list_sync((AttachList *)l);
    if (attacher_pending(l->attacher)) return true;
    for (size_t i = 0; i < l->count; i++) if (!l->items[i]->id) return true;
    return false;
}
Json *attach_list_ids(const AttachList *l) {
    if (!attach_list_count(l) || attach_list_uploading(l)) return NULL;
    Json *ids = json_array();
    for (size_t i = 0; i < l->count; i++) json_array_push(ids, json_string(l->items[i]->id));
    return ids;
}
void attach_list_sent(AttachList *l, const Json *ids) {
    for (size_t i = 0; i < json_count(ids); i++)
        for (size_t j = 0; j < l->count; j++)
            if (str_eq(l->items[j]->id, json_str(json_at(ids, i)))) { attach_list_remove(l, j); break; }
    attach_list_changed(l);
}
static bool active_current(AttachList *l) {
    if (l->retired) return false;
    attach_list_sync(l);
    return !InterlockedCompareExchange(&l->active->cancelled, 0, 0);
}
static void finish(AttachList *l) {
    Attachment *a = l->active; l->active = NULL;
    if (InterlockedCompareExchange(&a->cancelled, 0, 0)) item_free(a);
    attach_list_changed(l);
    advance(l); release(l);
}
// Finalizers also run after request_cancel: the serial turn lasts until its worker has stopped.
static void uploaded(void *ctx, Request *r) {
    AttachList *l = ctx; Attachment *a = l->active;
    char *error = NULL;
    if (r->client == g_store.client && api_error_unauthorized(&r->error)) store_invalidate_credentials(&r->error);
    if (active_current(l)) {
        if (r->ok) a->id = xstrdup(r->text);
        else {
            char *why = request_error_text(r);
            error = xstrfmt("%s could not be attached: %s", a->name, why); free(why);
            for (size_t i = 0; i < l->count; i++) if (l->items[i] == a) { attach_list_remove(l, i); break; }
        }
    }
    finish(l);
    if (error) { app_alert("Attachment", error); free(error); }
}
static void read_work(void *ctx) {
    AttachList *l = ctx; Attachment *a = l->active;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    a->error = attach_file_read(&a->source, &a->cancelled);
    CoUninitialize();
}
static void read_done(void *ctx) {
    AttachList *l = ctx; Attachment *a = l->active;
    if (!active_current(l)) { finish(l); return; }
    if (!a->source.bytes) {
        char *error = a->error; a->error = NULL;
        for (size_t i = 0; i < l->count; i++) if (l->items[i] == a) { attach_list_remove(l, i); break; }
        finish(l);
        if (error) { app_alert("Attachments", error); free(error); }
        return;
    }
    a->size = a->source.len;
    void *bytes = a->source.bytes; a->source.bytes = NULL;
    Request *r = store_upload(a->name, bytes, a->size, NULL, NULL, 0, &a->req);
    r->completion_ctx = l; r->completed = uploaded;
    attach_list_changed(l);
}
static void advance(AttachList *l) {
    if (l->retired || l->active) return;
    attach_list_sync(l);
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i]->id) continue;
        l->active = l->items[i]; l->refs++;
        if (!async_try_run(read_work, read_done, l)) {
            l->active->error = xstrdup("The file worker could not be started."); read_done(l);
        }
        return;
    }
}
static void arrived(void *ctx, AttachFile *files, size_t count, char *error) {
    AttachList *l = ctx;
    if (!account_current(l)) {
        attach_list_sync(l); attach_files_free(files, count); free(error); return;
    }
    Str refused; str_init(&refused);
    if (error) str_appendz(&refused, error);
    size_t i = 0;
    for (; i < count && l->count < ATTACHMENTS_MAX; i++) {
        Attachment *a = xcalloc(1, sizeof *a);
        a->source = files[i]; memset(&files[i], 0, sizeof files[i]);
        a->name = xstrdup(a->source.name); a->size = a->source.len;
        l->items = xrealloc(l->items, (l->count + 1) * sizeof *l->items); l->items[l->count++] = a;
    }
    if (i < count) str_appendf(&refused, "%sAt most %d files go with one message.", refused.len ? "\n" : "", ATTACHMENTS_MAX);
    attach_files_free(files, count); free(error);
    // Modal feedback may retire the owner or change its scope. Hold only the queue until it closes.
    l->refs++; attach_list_changed(l);
    if (refused.len) app_alert("Attachments", refused.data);
    str_free(&refused); advance(l); release(l);
}
Attacher *attach_list_attacher(AttachList *l) {
    attach_list_sync(l);
    if (!l->attacher) {
        api_client_release(l->client); l->client = api_client_retain(g_store.client);
        json_free(l->device); l->device = device_json(&g_store.device);
        l->attacher = attacher_new(arrived, l);
    }
    return l->attacher;
}
void attach_list_paths(AttachList *l, const wchar_t *const *paths, size_t count) {
    if (!attach_list_supported(l)) return;
    attacher_from_paths(attach_list_attacher(l), paths, count); attach_list_changed(l);
}
