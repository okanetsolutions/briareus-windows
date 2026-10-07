// Server responses kept on disk so a screen opens on what it last showed while the server is asked for changes.
// Every failure is silent: a missing or unreadable entry only means the screen waits for the network.
#ifndef BRIAREUS_CACHE_H
#define BRIAREUS_CACHE_H
#include "json.h"
#include <stdbool.h>
#include <time.h>
#include <wchar.h>

typedef struct DiskCache DiskCache;

DiskCache *cache_new(const wchar_t *directory);
void cache_free(DiskCache *cache);

/// The saved document, or NULL.
Json *cache_value(DiskCache *cache, const char *key);
/// Sorted keys make equal values equal bytes, so an unchanged poll result costs no write.
bool cache_store(DiskCache *cache, const Json *value, const char *key);

/// Bytes kept as they are, such as a repository's index; `*data` ends in a NUL past `*len`.
bool cache_bytes(DiskCache *cache, const char *key, char **data, size_t *len);
bool cache_store_bytes(DiskCache *cache, const char *data, size_t len, const char *key);
/// Marks the entry written now, so pruning keeps one still read but left unchanged; false when there is none.
bool cache_touch(DiskCache *cache, const char *key);

/// A log of values, one JSON document per line, that grows without being rewritten. Returns an array.
Json *cache_lines(DiskCache *cache, const char *key);
bool cache_append(DiskCache *cache, const Json *values, const char *key);
bool cache_replace(DiskCache *cache, const Json *values, const char *key);

void cache_remove(DiskCache *cache, const char *key);
void cache_remove_all(DiskCache *cache);
/// Drops entries nothing has written for a while, such as transcripts of conversations never reopened.
void cache_prune(DiskCache *cache, double age_seconds, time_t now);

/// The file name a key maps to, for tests; no path separators or dots.
char *cache_file_name(const char *key);

#endif
