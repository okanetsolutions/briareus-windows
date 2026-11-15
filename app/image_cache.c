// Remote pictures for the transcripts: a few downloads at a time on worker threads, their results kept by address.
#include "image_cache.h"
#include "picture.h"
#include "remote_image.h"
#include "store.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

enum { ENTRIES = 256, HOSTS = 32, DOWNLOADS = 4, PREVIEW_W = 720, PREVIEW_H = 480 };
// Room for the files and previews together; pictures no layout asked for in the last while give way first.
static const size_t BUDGET = 256u * 1024 * 1024;
static const ULONGLONG IN_USE_MS = 120000;

typedef struct {
    char *url; ImageState state;
    bool waiting;             // IMAGE_LOADING without a download of its own yet
    Picture preview; int width, height;
    void *bytes; size_t len;
    char *error;
    ULONGLONG used;           // when a layout last asked for it
} Entry;

typedef struct {
    char *url; size_t generation; int max_w, max_h;
    void *bytes; size_t len; Picture preview; int width, height; char *error;
} Job;

typedef struct { ImageCacheChanged changed; void *ctx; } Watcher;

// Everything a picture keeps is a kept allocation: it outlives the screen that asked for it.
static Entry *entries[ENTRIES]; static size_t entry_count;
static char *hosts[HOSTS]; static size_t host_count;
static Watcher *watchers; static size_t watcher_count;
static size_t generation; static int downloads;

static void start_waiting(void);

static char *kept_string(const char *z) {
    if (!z) return NULL;
    size_t n = strlen(z) + 1;
    char *copy = xmalloc_kept(n); memcpy(copy, z, n);
    return copy;
}
static void *kept_copy(const void *bytes, size_t len) { void *copy = xmalloc_kept(len); memcpy(copy, bytes, len); return copy; }

static size_t entry_size(const Entry *e) { return e->len + (size_t)e->preview.width * e->preview.height * 4; }
static void entry_clear(Entry *e) {
    xfree_kept(e->preview.pixels); xfree_kept(e->bytes); xfree_kept(e->error);
    memset(&e->preview, 0, sizeof e->preview); e->bytes = NULL; e->len = 0; e->error = NULL; e->width = e->height = 0;
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

static bool host_allowed(const char *url) {
    const ServerAddress *server = g_store.client ? api_client_address(g_store.client) : NULL;
    if (server && remote_image_from_origin(url, server->origin)) return true;
    char *host = remote_image_host(url);
    bool allowed = false;
    for (size_t i = 0; host && i < host_count && !allowed; i++) allowed = str_eq(hosts[i], host);
    free(host);
    return allowed;
}
static void allow_host(const char *url) {
    if (host_allowed(url) || host_count == HOSTS) return;
    char *host = remote_image_host(url);
    if (host) hosts[host_count++] = kept_string(host);
    free(host);
}

// MARK: - Room

/// Makes room for one more picture by dropping those no layout asked for lately, oldest first; `forced` drops the
/// oldest in use as well, as a picture the user asked for needs. False when the cache stays full.
static bool make_room(bool forced) {
    ULONGLONG now = GetTickCount64();
    for (;;) {
        size_t held = 0, oldest = entry_count;
        for (size_t i = 0; i < entry_count; i++) {
            held += entry_size(entries[i]);
            if (entries[i]->state == IMAGE_LOADING) continue;
            if (oldest == entry_count || entries[i]->used < entries[oldest]->used) oldest = i;
        }
        if (entry_count < ENTRIES && held < BUDGET) return true;
        if (oldest == entry_count) return false;
        if (!forced && now - entries[oldest]->used < IN_USE_MS) return false;
        entry_free(entries[oldest]);
        entries[oldest] = entries[--entry_count];
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
    if (e && e->state == IMAGE_LOADING && !e->waiting) {
        entry_clear(e);
        if (job->error) { e->state = IMAGE_FAILED; e->error = kept_string(job->error); }
        else {
            e->state = IMAGE_READY; e->bytes = job->bytes; e->len = job->len; e->preview = job->preview;
            e->width = job->width; e->height = job->height;
            job->bytes = NULL; job->preview.pixels = NULL;
        }
    }
    job_free(job);
    start_waiting();
    notify();
}

static void start(Entry *e) {
    e->state = IMAGE_LOADING; e->waiting = true;
    start_waiting();
}

static void start_waiting(void) {
    for (size_t i = 0; i < entry_count && downloads < DOWNLOADS; i++) {
        Entry *e = entries[i];
        if (e->state != IMAGE_LOADING || !e->waiting) continue;
        Job *job = xcalloc(1, sizeof *job);
        job->url = xstrdup(e->url); job->generation = generation; job->max_w = px(PREVIEW_W); job->max_h = px(PREVIEW_H);
        e->waiting = false;
        if (async_try_run(job_work, job_done, job)) { downloads++; continue; }
        job_free(job);
        e->state = IMAGE_FAILED; e->error = kept_string("The picture could not be fetched: Windows started no worker.");
    }
}

// MARK: - The cache

static ImageInfo info_of(const Entry *e, const char *url) {
    ImageInfo info; memset(&info, 0, sizeof info);
    if (!e) { info.state = IMAGE_ASK; info.host_allowed = host_allowed(url); return info; }
    info.state = e->state; info.host_allowed = true;
    if (e->state == IMAGE_READY) { info.preview = &e->preview; info.width = e->width; info.height = e->height; info.bytes = e->len; }
    if (e->state == IMAGE_FAILED) info.error = e->error;
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
    start(e);
    return info_of(e, url);
}

const Picture *image_cache_preview(const char *url) {
    Entry *e = find(url);
    return e && e->state == IMAGE_READY ? &e->preview : NULL;
}

void image_cache_allow(const char *url) {
    if (!remote_image_address(url)) return;
    allow_host(url);
    if (!host_allowed(url)) return;
    Entry *e = find(url);
    if (!e && make_room(true)) e = add(url);
    if (e && e->state != IMAGE_LOADING && e->state != IMAGE_READY) start(e);
    notify();
}

void image_cache_retry(const char *url) {
    Entry *e = find(url);
    if (!e || e->state != IMAGE_FAILED) return;
    entry_clear(e);
    start(e);
    notify();
}

void *image_cache_bytes(const char *url, size_t *len) {
    Entry *e = find(url);
    *len = 0;
    if (!e || e->state != IMAGE_READY) return NULL;
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

void image_cache_reset(void) {
    bool had = entry_count || host_count;
    for (size_t i = 0; i < entry_count; i++) entry_free(entries[i]);
    for (size_t i = 0; i < host_count; i++) xfree_kept(hosts[i]);
    entry_count = 0; host_count = 0; downloads = 0; generation++;
    if (had) notify();
}
