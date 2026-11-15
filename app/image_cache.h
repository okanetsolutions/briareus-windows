// The pictures conversations link to, fetched once and kept while the app runs. One is fetched by itself only from the
// paired server, where published session files are; any other host waits for the user's word, since an address written
// by an agent can carry what it read to whoever serves it. Nothing here is written to disk. UI thread only.
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
    bool host_allowed;        // IMAGE_ASK: the host may be fetched from, and only the cache's room held it back
    const Picture *preview;   // IMAGE_READY: scaled for a transcript
    int width, height;        // IMAGE_READY: the file's own size in pixels
    size_t bytes;             // IMAGE_READY: the file's size
    const char *error;        // IMAGE_FAILED
} ImageInfo;

/// The picture at `url`, starting its download when it comes from the paired server or an allowed host.
ImageInfo image_cache_get(const char *url);
/// The preview of a picture that is ready, or NULL; starts nothing. Borrowed until the next call here.
const Picture *image_cache_preview(const char *url);
/// The user asked for this picture: its host is fetched from until the app closes or the connection changes.
void image_cache_allow(const char *url);
/// Fetches a picture that failed again.
void image_cache_retry(const char *url);
/// A copy of a ready picture's file, malloc'd, for a viewer or to save; NULL when it is not ready.
void *image_cache_bytes(const char *url, size_t *len);

/// `changed` is called whenever a picture's state changes, until image_cache_unwatch with the same `ctx`.
typedef void (*ImageCacheChanged)(void *ctx);
void image_cache_watch(ImageCacheChanged changed, void *ctx);
void image_cache_unwatch(void *ctx);
/// Forgets every picture and allowed host, as the store does when the connection goes; downloads under way end in nothing.
void image_cache_reset(void);

#endif
