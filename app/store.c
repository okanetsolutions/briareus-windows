#include "store.h"
#include "credentials.h"
#include "str.h"
#include <math.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

Store g_store;
static volatile LONG cache_jobs;    // async_run_cache's work, queued or running

static void notify(void) { if (g_store.hwnd) PostMessageW(g_store.hwnd, WM_APP_STORE_CHANGED, 0, 0); }

static void set_error(const char *text) { free(g_store.connection_error); g_store.connection_error = xstrdup(text); }
static void set_error_from(const ApiError *e) { char *t = api_error_description(e); set_error(t); free(t); }

static void drop_client(void) {
    if (g_store.client) { api_client_release(g_store.client); g_store.client = NULL; }
    if (g_store.has_device) { device_free(&g_store.device); g_store.has_device = false; }
    routes_free(g_store.routes, g_store.route_count); g_store.routes = NULL; g_store.route_count = 0;
    g_store.transcribes = -1;
}

static void adopt(const Device *device, const Route *routes, size_t count, int transcribe) {
    if (g_store.has_device) device_free(&g_store.device);
    device_copy(&g_store.device, device); g_store.has_device = true;
    routes_free(g_store.routes, g_store.route_count);
    g_store.routes = routes_copy(routes, count); g_store.route_count = count;
    g_store.transcribes = transcribe;
}

static void save_connection(const Device *device, const Route *routes, size_t count, int transcribe) {
    Connection c; memset(&c, 0, sizeof c);
    device_copy(&c.device, device); c.routes = routes_copy(routes, count); c.route_count = count; c.transcribe = transcribe;
    Json *j = connection_json(&c);
    cache_store(g_store.cache, j, "connection");
    json_free(j); connection_free(&c);
}

void store_init(HWND main_window) {
    memset(&g_store, 0, sizeof g_store);
    g_store.hwnd = main_window; g_store.transcribes = -1; g_store.active = true;
    wchar_t *base = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base) != S_OK) base = NULL;
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\Okanet\\Briareus\\Responses", base ? base : L".");
    if (base) CoTaskMemFree(base);
    g_store.cache = cache_new(dir);
    g_store.server = settings_read_origin();
    if (!g_store.server) g_store.server = xstrdup("");
}
void store_shutdown(void) {
    // A repository index still being written or read finishes first: the cache and its lock go next.
    while (InterlockedCompareExchange(&cache_jobs, 0, 0) > 0) Sleep(10);
    drop_client();
    cache_free(g_store.cache); g_store.cache = NULL;
    free(g_store.server); free(g_store.connection_error);
}
bool store_connected(void) { return g_store.client != NULL; }
bool store_can_manage(void) { return g_store.has_device && device_can_manage(&g_store.device); }
bool store_can_transcribe(void) { return store_can_manage(); }
bool store_supports(const char *call) {
    const ApiRoute *route = api_route(call);
    if (!route || !g_store.has_device) return false;
    return routes_allow(g_store.routes, g_store.route_count, route->method, route->path, g_store.device.permission);
}
bool store_supports_attachments_on(const char *call) { return store_can_manage() && store_supports("upload") && store_supports(call); }
bool store_supports_attachments(void) { return store_supports_attachments_on("message"); }

// MARK: - Async

typedef struct { AsyncWork work, done; void *ctx; bool cache; } AsyncJob;
static DWORD WINAPI async_thread(LPVOID p) {
    AsyncJob *job = p;
    bool cache = job->cache;
    job->work(job->ctx);
    PostMessageW(g_store.hwnd, WM_APP_ASYNC_DONE, 0, (LPARAM)job);
    if (cache) InterlockedDecrement(&cache_jobs);
    return 0;
}
static void async_queue(AsyncWork work, AsyncWork done, void *ctx, bool cache) {
    AsyncJob *job = xcalloc(1, sizeof *job);
    job->work = work; job->done = done; job->ctx = ctx; job->cache = cache;
    if (cache) InterlockedIncrement(&cache_jobs);
    if (!QueueUserWorkItem(async_thread, job, WT_EXECUTELONGFUNCTION)) {
        work(ctx); if (done) done(ctx); free(job);
        if (cache) InterlockedDecrement(&cache_jobs);
    }
}
void async_run(AsyncWork work, AsyncWork done, void *ctx) { async_queue(work, done, ctx, false); }
void async_run_cache(AsyncWork work, AsyncWork done, void *ctx) { async_queue(work, done, ctx, true); }

// MARK: - Pairing

typedef struct {
    ApiClient *client; ServerAddress address; char *token;
    Discovery discovery; bool has_discovery;
    Route *routes; size_t route_count; bool has_routes;
    ApiError error; bool ok;
    bool verifying;             // a saved connection being confirmed rather than a new pairing
    Connection saved; bool has_saved;
} PairJob;

static void pair_work(void *p) {
    PairJob *j = p;
    api_error_init(&j->error);
    if (!api_discovery(j->client, &j->discovery, &j->error)) return;
    j->has_discovery = true;
    if (!api_catalog(j->client, &j->routes, &j->route_count, &j->error)) return;
    j->has_routes = true; j->ok = true;
}
static void pair_free(PairJob *j) {
    api_client_release(j->client); server_address_free(&j->address); free(j->token);
    if (j->has_discovery) discovery_free(&j->discovery);
    if (j->has_routes) routes_free(j->routes, j->route_count);
    if (j->has_saved) connection_free(&j->saved);
    api_error_clear(&j->error); free(j);
}
static bool same_repos(const Device *a, const Device *b) {
    if (a->repo_count != b->repo_count) return false;
    for (size_t i = 0; i < a->repo_count; i++) {
        bool found = false;
        for (size_t k = 0; k < b->repo_count && !found; k++) found = str_eq(a->repos[i], b->repos[k]);
        if (!found) return false;
    }
    return true;
}
static void pair_done(void *p) {
    PairJob *j = p;
    if (j->verifying) {
        if (g_store.client == j->client) {
            if (j->ok) {
                // Saved screens may hold projects this device can no longer read.
                if (!str_eq(j->discovery.device.id, j->saved.device.id) || !same_repos(&j->discovery.device, &j->saved.device)) cache_remove_all(g_store.cache);
                adopt(&j->discovery.device, j->routes, j->route_count, j->discovery.transcribe);
                save_connection(&j->discovery.device, j->routes, j->route_count, j->discovery.transcribe);
                notify();
            } else if (api_error_unauthorized(&j->error)) {
                store_invalidate_credentials(&j->error);
            } else if (j->error.kind == API_INCOMPATIBLE_VERSION) {
                drop_client(); set_error_from(&j->error); notify();
            }
            // Anything else is a server out of reach: saved screens stay readable and report their own errors.
        }
        pair_free(j);
        return;
    }
    g_store.connecting = false;
    if (!j->ok) { set_error_from(&j->error); notify(); pair_free(j); return; }
    if (!credentials_save(j->token, j->address.origin)) { set_error(credentials_failure_text()); notify(); pair_free(j); return; }
    free(g_store.server); g_store.server = xstrdup(j->address.origin);
    settings_write_origin(j->address.origin);
    Json *saved = cache_value(g_store.cache, "connection");
    Connection previous = { 0 };
    bool had = saved && connection_parse(saved, &previous);
    if (!had || !str_eq(previous.device.id, j->discovery.device.id)) cache_remove_all(g_store.cache);
    if (had) connection_free(&previous);
    json_free(saved);
    save_connection(&j->discovery.device, j->routes, j->route_count, j->discovery.transcribe);
    drop_client();
    adopt(&j->discovery.device, j->routes, j->route_count, j->discovery.transcribe);
    g_store.client = api_client_retain(j->client);
    set_error(NULL);
    notify();
    pair_free(j);
}

void store_connect(const char *server, const char *token) {
    if (g_store.connecting) return;
    g_store.connecting = true; set_error(NULL); notify();
    PairJob *j = xcalloc(1, sizeof *j);
    api_error_init(&j->error);
    if (!server_address_parse(server, &j->address)) {
        ApiError e; api_error_init(&e); api_error_set(&e, API_INVALID_ADDRESS, 0, NULL, -1);
        set_error_from(&e); api_error_clear(&e); g_store.connecting = false; notify(); free(j); return;
    }
    j->token = str_trim(token);
    j->client = api_client_new(&j->address, j->token, &j->error);
    if (!j->client) { set_error_from(&j->error); g_store.connecting = false; notify(); pair_free(j); return; }
    async_run(pair_work, pair_done, j);
}

void store_restore(void) {
    if (g_store.client || str_empty(g_store.server)) return;
    ServerAddress address;
    if (!server_address_parse(g_store.server, &address)) return;
    bool failed = false;
    char *token = credentials_read(address.origin, &failed);
    if (!token) { if (failed) { set_error(credentials_failure_text()); notify(); } server_address_free(&address); return; }
    cache_prune(g_store.cache, 30.0 * 86400, time(NULL));
    Json *saved_json = cache_value(g_store.cache, "connection");
    Connection saved;
    if (saved_json && connection_parse(saved_json, &saved)) {
        json_free(saved_json);
        ApiError e; api_error_init(&e);
        ApiClient *client = api_client_new(&address, token, &e);
        if (!client) { set_error_from(&e); api_error_clear(&e); connection_free(&saved); free(token); server_address_free(&address); notify(); return; }
        adopt(&saved.device, saved.routes, saved.route_count, saved.transcribe);
        g_store.client = client;
        notify();
        PairJob *j = xcalloc(1, sizeof *j);
        api_error_init(&j->error);
        j->client = api_client_retain(client); server_address_copy(&j->address, &address); j->token = xstrdup(token);
        j->verifying = true; j->saved = saved; j->has_saved = true;
        async_run(pair_work, pair_done, j);
    } else {
        json_free(saved_json);
        store_connect(g_store.server, token);
    }
    free(token); server_address_free(&address);
}

void store_invalidate_credentials(const ApiError *error) {
    drop_client();
    cache_remove_all(g_store.cache);
    // Keep the origin so a replacement token is easy to enter. A failed deletion is reported.
    ServerAddress address;
    bool removed = true;
    if (server_address_parse(g_store.server, &address)) { removed = credentials_remove(address.origin); server_address_free(&address); }
    if (!removed) set_error(credentials_failure_text()); else set_error_from(error);
    notify();
}

void store_forget(void) {
    ServerAddress address;
    if (server_address_parse(g_store.server, &address)) { credentials_remove(address.origin); server_address_free(&address); }
    drop_client();
    set_error(NULL);
    cache_remove_all(g_store.cache);
    settings_remove_origin();
    free(g_store.server); g_store.server = xstrdup("");
    notify();
}

typedef struct { ApiClient *client; Discovery discovery; bool ok; ApiError error; void (*done)(void *, const char *); void *ctx; } VoiceCheckJob;
static void voice_check_work(void *p) { VoiceCheckJob *j = p; api_error_init(&j->error); j->ok = api_discovery(j->client, &j->discovery, &j->error); }
static void voice_check_done(void *p) {
    VoiceCheckJob *j = p;
    if (g_store.client != j->client) { if (j->done) j->done(j->ctx, NULL); }
    else if (j->ok) {
        g_store.transcribes = j->discovery.transcribe;
        save_connection(&j->discovery.device, g_store.routes, g_store.route_count, j->discovery.transcribe);
        if (j->done) j->done(j->ctx, discovery_voice_notes_off(g_store.transcribes));
    } else if (api_error_unauthorized(&j->error)) {
        store_invalidate_credentials(&j->error);
        if (j->done) j->done(j->ctx, NULL);
    } else {
        char *why = api_error_description(&j->error);
        char *text = xstrfmt("The server could not be asked about voice notes: %s", why);
        if (j->done) j->done(j->ctx, text);
        free(text); free(why);
    }
    if (j->ok) discovery_free(&j->discovery);
    api_client_release(j->client); api_error_clear(&j->error); free(j);
}
void store_voice_notes_off(void (*done)(void *ctx, const char *reason), void *ctx) {
    if (g_store.transcribes == 1 || !g_store.client) { done(ctx, discovery_voice_notes_off(g_store.transcribes)); return; }
    VoiceCheckJob *j = xcalloc(1, sizeof *j);
    j->client = api_client_retain(g_store.client); j->done = done; j->ctx = ctx;
    async_run(voice_check_work, voice_check_done, j);
}

// MARK: - Requests

static void request_free(Request *r) {
    free(r->operation); json_free(r->args); free(r->audio); free(r->audio_type); free(r->file); free(r->file_name);
    json_free(r->result); free(r->text); api_error_clear(&r->error);
    if (r->client) api_client_release(r->client);
    free(r);
}
static DWORD WINAPI request_thread(LPVOID p) {
    Request *r = p;
    if (r->audio) {
        r->text = api_transcribe(r->client, r->audio, r->audio_len, r->audio_type, &r->error);
        r->ok = r->text != NULL;
    } else if (r->file) {
        r->text = api_upload(r->client, r->file_name, r->file, r->file_len, &r->error);
        r->ok = r->text != NULL;
    } else {
        r->result = api_call(r->client, r->operation, r->args, r->timeout_ms, &r->error);
        r->ok = r->result != NULL;
    }
    PostMessageW(g_store.hwnd, WM_APP_REQUEST_DONE, 0, (LPARAM)r);
    return 0;
}
static Request *request_new(void *owner, RequestDone done, int tag, Request **slot) {
    Request *r = xcalloc(1, sizeof *r);
    api_error_init(&r->error);
    r->owner = owner; r->done = done; r->tag = tag; r->slot = slot;
    if (slot) { request_cancel(slot); *slot = r; }
    return r;
}
static void request_refuse(Request *r, const char *message) {
    api_error_set(&r->error, API_HTTP, 403, message, -1);
    PostMessageW(g_store.hwnd, WM_APP_REQUEST_DONE, 0, (LPARAM)r);
}
static void request_start(Request *r) {
    r->client = api_client_retain(g_store.client);
    if (!QueueUserWorkItem(request_thread, r, WT_EXECUTELONGFUNCTION)) request_thread(r);
}
Request *store_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone done, int tag, Request **slot) {
    Request *r = request_new(owner, done, tag, slot);
    r->operation = xstrdup(operation); r->args = args; r->timeout_ms = timeout_ms;
    if (!g_store.client || !store_supports(operation)) { request_refuse(r, "This token cannot perform that action."); return r; }
    request_start(r);
    return r;
}
Request *store_transcribe(const void *audio, size_t len, const char *content_type, void *owner, RequestDone done, int tag, Request **slot) {
    Request *r = request_new(owner, done, tag, slot);
    r->audio = xmalloc(len); memcpy(r->audio, audio, len); r->audio_len = len;
    r->audio_type = xstrdup(content_type ? content_type : "audio/mp4");
    if (!g_store.client || !store_can_transcribe()) { request_refuse(r, "This token cannot transcribe voice notes."); return r; }
    request_start(r);
    return r;
}
Request *store_upload(const char *name, void *bytes, size_t len, void *owner, RequestDone done, int tag, Request **slot) {
    Request *r = request_new(owner, done, tag, slot);
    r->file = bytes ? bytes : xmalloc(1); r->file_len = len; r->file_name = xstrdup(name ? name : "file");
    if (!g_store.client || !store_can_manage() || !store_supports("upload")) { request_refuse(r, "This server does not take files with a message."); return r; }
    request_start(r);
    return r;
}
void request_cancel(Request **slot) {
    if (!slot || !*slot) return;
    (*slot)->cancelled = true; (*slot)->slot = NULL;
    *slot = NULL;
}
char *request_error_text(const Request *req) { return api_error_description(&req->error); }
void request_error_into(char **slot, const Request *req) { free(*slot); *slot = request_error_text(req); }
bool request_outcome_unknown(const Request *req) { return !req->ok && !api_error_is_refusal(&req->error); }
char *request_error_or_unexpected(const Request *req) {
    return req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
}

void store_handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    if (msg == WM_APP_REQUEST_DONE) {
        Request *r = (Request *)lp;
        if (r->cancelled) { request_free(r); return; }
        if (r->slot) *r->slot = NULL;
        if (!r->ok && api_error_unauthorized(&r->error) && r->client == g_store.client) store_invalidate_credentials(&r->error);
        if (r->done) r->done(r->owner, r);
        request_free(r);
    } else if (msg == WM_APP_ASYNC_DONE) {
        AsyncJob *job = (AsyncJob *)lp;
        if (job->done) job->done(job->ctx);
        free(job);
    }
}

int poll_delay_ms(int base_ms, int failures, double retry_after_seconds) {
    if (failures <= 0) return base_ms;
    int capped = failures > 6 ? 6 : failures;
    double seconds = pow(2, capped);
    if (seconds > 60) seconds = 60;
    if (retry_after_seconds > seconds) seconds = retry_after_seconds;
    return (int)(seconds * 1000);
}
