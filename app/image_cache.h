// The pictures conversations link to, fetched once and kept in memory while the app runs. One is fetched by itself only
// from the paired server, or from a host the user allowed (a server may publish its session files on another, as on
// R2): an address written by an agent can carry what it read to whoever serves it. The allowed hosts are saved with
// the connection's responses (g_store.cache); no picture is written to disk. UI thread only.
#ifndef BRIAREUS_IMAGE_CACHE_H
#define BRIAREUS_IMAGE_CACHE_H
#include "theme.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    IMAGE_ASK,       // not fetched: its host is not allowed yet, or the cache is full of pictures in use
    IMAGE_LOADING,
    IMAGE_READY,
    IMAGE_FAILED,
} ImageState;

/// Borrowed until the next call here.
typedef struct {
    ImageState state;
    bool host_allowed;           // IMAGE_ASK: the host may be fetched from, and only the cache's room held it back
    const Picture *preview;      // IMAGE_READY: scaled for a transcript; NULL while it gave way to others (image_cache_shown)
    int preview_w, preview_h;    // IMAGE_READY: the preview's size, known whether or not it is held now
    int width, height;           // IMAGE_READY: the file's own size in pixels
    size_t bytes;                // IMAGE_READY: the file's size, 0 while it gave way
    const char *error;           // IMAGE_FAILED
} ImageInfo;

/// The picture at `url`, starting its download when it comes from the paired server or an allowed host.
ImageInfo image_cache_get(const char *url);
/// What a paint draws: the preview of a ready picture, or NULL. The picture counts as in view, which keeps it when the
/// cache is over its budget; one that gave way is fetched again. Borrowed until the next call here.
const Picture *image_cache_shown(const char *url);
/// The user asked for this picture: its host is fetched from, from now on, for as long as this connection is kept.
void image_cache_allow(const char *url);
/// Fetches a picture that failed again.
void image_cache_retry(const char *url);
/// The same for every picture that failed, as a screen read again asks.
void image_cache_retry_failed(void);
/// A copy of a ready picture's file, malloc'd, for a viewer or to save; NULL when it is not held.
void *image_cache_bytes(const char *url, size_t *len);

/// `changed` is called whenever a picture's state changes, until image_cache_unwatch with the same `ctx`.
typedef void (*ImageCacheChanged)(void *ctx);
void image_cache_watch(ImageCacheChanged changed, void *ctx);
void image_cache_unwatch(void *ctx);
/// The room the files and previews may take before pictures out of view give theirs up, and how long after it was
/// painted a picture counts as in view. Tests make both small; 0 bytes puts back 256 MB and 15 seconds.
void image_cache_set_limits(size_t bytes, unsigned view_ms);
/// Forgets every picture, and the allowed hosts until they are read from the next connection's saved responses, as the
/// store does when the connection goes; downloads under way end in nothing.
void image_cache_reset(void);

#endif
