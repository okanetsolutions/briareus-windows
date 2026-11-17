// Remote pictures for the transcripts: a few downloads at a time on worker threads, their results kept by address.
#include "image_cache.h"
#include "picture.h"
#include "remote_image.h"
#include "store.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

enum { ENTRIES = 1024, HOSTS = 32, DOWNLOADS = 4, PREVIEW_W = 720, PREVIEW_H = 480 };
// Room for the files and previews together. Past it, pictures not painted for a while give theirs up, and are fetched
// again when they come back into view: a long transcript keeps what is on screen, not everything it ever showed.
static const size_t BUDGET = 256u * 1024 * 1024;
static const ULONGLONG IN_VIEW_MS = 15000, IN_USE_MS = 120000;
static size_t budget = 256u * 1024 * 1024;
static ULONGLONG in_view_ms = 15000;

typedef struct {
    char *url; ImageState state;
    bool queued, fetching;    // wants a download; has one under way
    bool shed;                // IMAGE_READY whose file and preview gave way: its sizes stay for the layout
    Picture preview; int preview_w, preview_h, width, height;
    void *bytes; size_t len;
    char *error;
    ULONGLONG used, painted;  // when a layout last asked for it; when it was last drawn, or arrived
} Entry;

typedef struct {
    char *url; size_t generation; int max_w, max_h;
    void *bytes; size_t len; Picture preview; int width, height; char *error;
} Job;

typedef struct { ImageCacheChanged changed; void *ctx; } Watcher;

// Everything a picture keeps is a kept allocation: it outlives the screen that asked for it.
static Entry *entries[ENTRIES]; static size_t entry_count;
static char *hosts[HOSTS]; static size_t host_count;
static bool hosts_read;       // the saved list was taken from this connection's cache
static Watcher *watchers; static size_t watcher_count;
static size_t generation; static int downloads;
static UINT_PTR trim_timer;    // trims again once the pictures that kept the cache over its budget leave the view

static void start_queued(void);

static char *kept_string(const char *z) {
    if (!z) return NULL;
    size_t n = strlen(z) + 1;
    char *copy = xmalloc_kept(n); memcpy(copy, z, n);
    return copy;
}
static void *kept_copy(const void *bytes, size_t len) { void *copy = xmalloc_kept(len); memcpy(copy, bytes, len); return copy; }

static size_t entry_size(const Entry *e) { return e->len + (size_t)e->preview.width * e->preview.height * 4; }
/// Lets go of the file and the preview, which is all of an entry that weighs anything.
static void entry_unload(Entry *e) {
    xfree_kept(e->preview.pixels); xfree_kept(e->bytes);
    memset(&e->preview, 0, sizeof e->preview); e->bytes = NULL; e->len = 0;
}
static void entry_clear(Entry *e) {
    entry_unload(e);
    xfree_kept(e->error); e->error = NULL;
    e->width = e->height = e->preview_w = e->preview_h = 0; e->shed = false;
}
static void entry_free(Entry *e) { entry_clear(e); xfree_kept(e->url); xfree_kept(e); }
static Entry *find(const char *url) {
    for (size_t i = 0; url && i < entry_count; i++) if (str_eq(entries[i]->url, url)) return entries[i];
    return NULL;
}
static void notify(void) {
    // A watcher may leave while it is told: the list is walked from its end.
    for (size_t i = watcher_count; i-- > 0;) if (i < watcher_count) watchers[i].changed(watchers[i].ctx);
}

// MARK: - Who may be fetched

// The allowed hosts are saved with the connection's responses, so they last as long as it does and go when it is
// forgotten, revoked or replaced.
static const char HOSTS_KEY[] = "picture-hosts";

static void forget_hosts(void) {
    for (size_t i = 0; i < host_count; i++) xfree_kept(hosts[i]);
    host_count = 0; hosts_read = false;
}
static void read_hosts(void) {
    if (hosts_read || !g_store.cache) return;
    hosts_read = true;
    Json *saved = cache_value(g_store.cache, HOSTS_KEY);
    for (size_t i = 0; i < json_count(saved) && host_count < HOSTS; i++) {
        const char *host = json_str(json_at(saved, i));
        if (host && *host) hosts[host_count++] = kept_string(host);
    }
    // Read is use: pruning leaves the list alone while the app is opened.
    if (saved) cache_touch(g_store.cache, HOSTS_KEY);
    json_free(saved);
}
static void save_hosts(void) {
    if (!g_store.cache) return;
    Json *list = json_array();
    for (size_t i = 0; i < host_count; i++) json_array_push(list, json_string(hosts[i]));
    cache_store(g_store.cache, list, HOSTS_KEY);
    json_free(list);
}

static bool host_allowed(const char *url) {
    const ServerAddress *server = g_store.client ? api_client_address(g_store.client) : NULL;
    if (server && remote_image_from_origin(url, server->origin)) return true;
    read_hosts();
    char *host = remote_image_host(url);
    bool allowed = false;
    for (size_t i = 0; host && i < host_count && !allowed; i++) allowed = str_eq(hosts[i], host);
    free(host);
    return allowed;
}
static void allow_host(const char *url) {
    if (host_allowed(url)) return;
    char *host = remote_image_host(url);
    if (!host) return;
    // A full list lets its oldest host go.
    if (host_count == HOSTS) { xfree_kept(hosts[0]); memmove(&hosts[0], &hosts[1], (HOSTS - 1) * sizeof *hosts); host_count--; }
    hosts[host_count++] = kept_string(host);
    free(host);
    save_hosts();
}

// MARK: - Room

/// Makes room for one more picture's entry by dropping one no layout asked for lately, oldest first; `forced` drops
/// the oldest in use as well, as a picture the user asked for needs. False when the cache stays full.
static bool make_room(bool forced) {
    if (entry_count < ENTRIES) return true;
    size_t oldest = entry_count;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i]->queued || entries[i]->fetching) continue;
        if (oldest == entry_count || entries[i]->used < entries[oldest]->used) oldest = i;
    }
    if (oldest == entry_count) return false;
    if (!forced && GetTickCount64() - entries[oldest]->used < IN_USE_MS) return false;
    entry_free(entries[oldest]);
    entries[oldest] = entries[--entry_count];
    return true;
}

static void trim(const Entry *keep);
static void CALLBACK trim_later(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    KillTimer(NULL, id);
    if (id == trim_timer) trim_timer = 0;
    trim(NULL);
}

/// Over the budget, the pictures longest out of view give up their file and preview; `keep` just arrived. Pictures
/// still counted as in view are trimmed again once they no longer are: a burst of arrivals is otherwise held for good.
static void trim(const Entry *keep) {
    ULONGLONG now = GetTickCount64();
    for (;;) {
        size_t held = 0; Entry *oldest = NULL; ULONGLONG wait = 0;
        for (size_t i = 0; i < entry_count; i++) {
            Entry *e = entries[i];
            held += entry_size(e);
            if (e->state != IMAGE_READY || e->shed) continue;
            ULONGLONG since = now - e->painted;
            if (e == keep || since < in_view_ms) {
                ULONGLONG left = since < in_view_ms ? in_view_ms - since : 0;
                if (left && (!wait || left < wait)) wait = left;
                continue;
            }
            if (!oldest || e->painted < oldest->painted) oldest = e;
        }
        if (held <= budget) return;
        if (!oldest) {
            if (!wait) return;
            if (trim_timer) KillTimer(NULL, trim_timer);
            trim_timer = SetTimer(NULL, 0, (UINT)wait, trim_later);
            return;
        }
        entry_unload(oldest); oldest->shed = true;
    }
}

// MARK: - Downloads

static void job_free(Job *job) {
    xfree_kept(job->bytes); xfree_kept(job->preview.pixels); free(job->error); free(job->url); free(job);
}

/// On a worker: the file, then its preview, moved into kept memory for the cache to take.
static void job_work(void *p) {
    Job *job = p;
    char *bytes = NULL; Picture preview;
    if (!remote_image_download(job->url, &bytes, &job->len, &job->error)) return;
    if (!picture_decode(bytes, job->len, job->max_w, job->max_h, &preview, &job->width, &job->height)) {
        job->error = xstrdup("Windows could not read the picture.");
        free(bytes); return;
    }
    job->bytes = kept_copy(bytes, job->len); free(bytes);
    job->preview = preview;
    job->preview.pixels = kept_copy(preview.pixels, (size_t)preview.width * preview.height * 4);
    picture_free(&preview);
}

static void job_done(void *p) {
    Job *job = p;
    Entry *e = job->generation == generation ? find(job->url) : NULL;
    if (job->generation == generation) downloads--;
    if (e && e->fetching) {
        entry_clear(e);
        e->fetching = false;
        if (job->error) { e->state = IMAGE_FAILED; e->error = kept_string(job->error); }
        else {
            e->state = IMAGE_READY; e->bytes = job->bytes; e->len = job->len; e->preview = job->preview;
            e->preview_w = job->preview.width; e->preview_h = job->preview.height;
            e->width = job->width; e->height = job->height; e->painted = GetTickCount64();
            job->bytes = NULL; job->preview.pixels = NULL;
            trim(e);
        }
    }
    job_free(job);
    start_queued();
    notify();
}

/// Starts downloads while there is room for them, the pictures asked for last first: in a transcript those are the
/// ones nearest its end, where the view is.
static void start_queued(void) {
    for (size_t i = entry_count; i-- > 0 && downloads < DOWNLOADS;) {
        Entry *e = entries[i];
        if (!e->queued) continue;
        Job *job = xcalloc(1, sizeof *job);
        job->url = xstrdup(e->url); job->generation = generation; job->max_w = px(PREVIEW_W); job->max_h = px(PREVIEW_H);
        e->queued = false;
        if (async_try_run(job_work, job_done, job)) { downloads++; e->fetching = true; continue; }
        job_free(job);
        entry_clear(e);
        e->state = IMAGE_FAILED; e->error = kept_string("The picture could not be fetched: Windows started no worker.");
    }
}

/// A picture not fetched yet, or one that failed, starts over; one that gave way keeps its place while it comes back.
static void fetch(Entry *e) {
    if (e->queued || e->fetching) return;
    if (!e->shed) { entry_clear(e); e->state = IMAGE_LOADING; }
    e->queued = true;
    start_queued();
}

// MARK: - The cache

static ImageInfo info_of(const Entry *e, const char *url) {
    ImageInfo info; memset(&info, 0, sizeof info);
    if (!e) { info.state = IMAGE_ASK; info.host_allowed = host_allowed(url); return info; }
    info.state = e->state; info.host_allowed = true;
    if (e->state == IMAGE_FAILED) info.error = e->error;
    if (e->state != IMAGE_READY) return info;
    info.preview = e->shed ? NULL : &e->preview; info.preview_w = e->preview_w; info.preview_h = e->preview_h;
    info.width = e->width; info.height = e->height; info.bytes = e->len;
    return info;
}

static Entry *add(const char *url) {
    Entry *e = xmalloc_kept(sizeof *e); memset(e, 0, sizeof *e);
    e->url = kept_string(url); e->used = GetTickCount64();
    entries[entry_count++] = e;
    return e;
}

ImageInfo image_cache_get(const char *url) {
    Entry *e = find(url);
    if (e) { e->used = GetTickCount64(); return info_of(e, url); }
    if (!remote_image_address(url) || !host_allowed(url) || !make_room(false)) return info_of(NULL, url);
    e = add(url);
    fetch(e);
    return info_of(e, url);
}

const Picture *image_cache_shown(const char *url) {
    Entry *e = find(url);
    if (!e || e->state != IMAGE_READY) return NULL;
    e->painted = GetTickCount64();
    if (!e->shed) return &e->preview;
    fetch(e);
    return NULL;
}

void image_cache_allow(const char *url) {
    if (!remote_image_address(url)) return;
    allow_host(url);
    Entry *e = find(url);
    if (!e && make_room(true)) e = add(url);
    if (e && e->state != IMAGE_LOADING && e->state != IMAGE_READY) fetch(e);
    notify();
}

void image_cache_retry(const char *url) {
    Entry *e = find(url);
    if (!e || e->state != IMAGE_FAILED) return;
    fetch(e);
    notify();
}

void image_cache_retry_failed(void) {
    bool any = false;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i]->state != IMAGE_FAILED) continue;
        fetch(entries[i]); any = true;
    }
    if (any) notify();
}

void *image_cache_bytes(const char *url, size_t *len) {
    Entry *e = find(url);
    *len = 0;
    if (!e || e->state != IMAGE_READY || e->shed) return NULL;
    void *copy = xmalloc(e->len); memcpy(copy, e->bytes, e->len);
    *len = e->len;
    return copy;
}

void image_cache_watch(ImageCacheChanged changed, void *ctx) {
    watchers = xrealloc(watchers, (watcher_count + 1) * sizeof *watchers);
    watchers[watcher_count].changed = changed; watchers[watcher_count].ctx = ctx; watcher_count++;
}
void image_cache_unwatch(void *ctx) {
    for (size_t i = 0; i < watcher_count; i++) {
        if (watchers[i].ctx != ctx) continue;
        memmove(&watchers[i], &watchers[i + 1], (watcher_count - i - 1) * sizeof *watchers);
        watcher_count--;
        break;
    }
    if (!watcher_count) { free(watchers); watchers = NULL; }
}

void image_cache_set_limits(size_t bytes, unsigned view_ms) {
    budget = bytes ? bytes : BUDGET;
    in_view_ms = bytes ? view_ms : IN_VIEW_MS;
}

void image_cache_reset(void) {
    bool had = entry_count || host_count;
    for (size_t i = 0; i < entry_count; i++) entry_free(entries[i]);
    forget_hosts();
    if (trim_timer) { KillTimer(NULL, trim_timer); trim_timer = 0; }
    entry_count = 0; downloads = 0; generation++;
    if (had) notify();
}
