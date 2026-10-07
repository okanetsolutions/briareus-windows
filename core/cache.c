#include "cache.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct DiskCache {
    wchar_t *directory;
    CRITICAL_SECTION lock;
};

DiskCache *cache_new(const wchar_t *directory) {
    DiskCache *c = xcalloc(1, sizeof *c);
    size_t n = wcslen(directory);
    c->directory = xmalloc((n + 1) * sizeof(wchar_t));
    memcpy(c->directory, directory, (n + 1) * sizeof(wchar_t));
    InitializeCriticalSection(&c->lock);
    return c;
}
void cache_free(DiskCache *c) {
    if (!c) return;
    DeleteCriticalSection(&c->lock);
    free(c->directory); free(c);
}

char *cache_file_name(const char *key) {
    // Keys carry repository names and server ids; encoding leaves no path separators or dots.
    Str s; str_init(&s);
    for (const unsigned char *p = (const unsigned char *)(key ? key : ""); *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) str_appendc(&s, (char)*p);
        else str_appendf(&s, "%%%02X", *p);
    }
    if (!s.len) str_appendc(&s, '_');
    return str_detach(&s);
}

static wchar_t *path_for(DiskCache *c, const char *key) {
    char *name = cache_file_name(key);
    wchar_t *wname = utf8_to_wide(name);
    free(name);
    size_t n = wcslen(c->directory) + wcslen(wname) + 2;
    wchar_t *path = xmalloc(n * sizeof(wchar_t));
    swprintf(path, n, L"%ls\\%ls", c->directory, wname);
    free(wname);
    return path;
}

static bool read_file(const wchar_t *path, char **data, size_t *len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256LL * 1024 * 1024) { CloseHandle(h); return false; }
    char *buf = xmalloc((size_t)size.QuadPart + 1);
    size_t total = 0;
    while (total < (size_t)size.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(h, buf + total, (DWORD)((size_t)size.QuadPart - total), &got, NULL) || !got) break;
        total += got;
    }
    CloseHandle(h);
    buf[total] = 0;
    *data = buf; *len = total;
    return true;
}

static bool ensure_directory(DiskCache *c) {
    // Create every missing component, then keep the directory's files encrypted for this user, where the volume allows it.
    wchar_t *copy = xmalloc((wcslen(c->directory) + 1) * sizeof(wchar_t));
    wcscpy(copy, c->directory);
    for (wchar_t *p = copy + 3; *p; p++) {
        if (*p == L'\\' || *p == L'/') { *p = 0; CreateDirectoryW(copy, NULL); *p = L'\\'; }
    }
    BOOL created = CreateDirectoryW(copy, NULL);
    bool ok = created || GetLastError() == ERROR_ALREADY_EXISTS;
    if (created) EncryptFileW(copy);
    free(copy);
    return ok;
}

static bool write_atomic(DiskCache *c, const wchar_t *path, const char *data, size_t len) {
    if (!ensure_directory(c)) return false;
    size_t n = wcslen(path) + 5;
    wchar_t *temp = xmalloc(n * sizeof(wchar_t));
    swprintf(temp, n, L"%ls.tmp", path);
    HANDLE h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = len == 0 || (WriteFile(h, data, (DWORD)len, &written, NULL) && written == len);
        if (ok) ok = FlushFileBuffers(h) != 0;
        CloseHandle(h);
        if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(temp);
    }
    free(temp);
    return ok;
}

Json *cache_value(DiskCache *c, const char *key) {
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    char *data = NULL; size_t len = 0;
    Json *result = read_file(path, &data, &len) ? json_parse(data, len) : NULL;
    free(data); free(path);
    LeaveCriticalSection(&c->lock);
    return result;
}

bool cache_store(DiskCache *c, const Json *value, const char *key) {
    char *bytes = json_serialize(value, true);
    size_t len = strlen(bytes);
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    char *existing = NULL; size_t existing_len = 0;
    bool ok;
    if (read_file(path, &existing, &existing_len) && existing_len == len && memcmp(existing, bytes, len) == 0) ok = true;
    else ok = write_atomic(c, path, bytes, len);
    free(existing); free(path);
    LeaveCriticalSection(&c->lock);
    free(bytes);
    return ok;
}

bool cache_bytes(DiskCache *c, const char *key, char **data, size_t *len) {
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    bool ok = read_file(path, data, len);
    free(path);
    LeaveCriticalSection(&c->lock);
    return ok;
}
bool cache_store_bytes(DiskCache *c, const char *data, size_t len, const char *key) {
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    bool ok = write_atomic(c, path, data, len);
    free(path);
    LeaveCriticalSection(&c->lock);
    return ok;
}

Json *cache_lines(DiskCache *c, const char *key) {
    Json *result = json_array();
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    char *data = NULL; size_t len = 0;
    if (read_file(path, &data, &len)) {
        char *p = data, *end = data + len;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (n) { Json *line = json_parse(p, n); if (line) json_array_push(result, line); }
            p += n + 1;
        }
    }
    free(data); free(path);
    LeaveCriticalSection(&c->lock);
    return result;
}

static char *log_bytes(const Json *values, size_t *len) {
    Str s; str_init(&s);
    size_t n = json_count(values);
    for (size_t i = 0; i < n; i++) {
        char *line = json_serialize(json_at(values, i), false);
        // The leading newline keeps a line cut short by an interrupted write from swallowing the next one.
        str_appendc(&s, '\n'); str_appendz(&s, line);
        free(line);
    }
    *len = s.len;
    return str_detach(&s);
}

bool cache_append(DiskCache *c, const Json *values, const char *key) {
    size_t len = 0;
    char *bytes = log_bytes(values, &len);
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    bool ok = false;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        ok = write_atomic(c, path, bytes, len);
    } else {
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ok = len == 0 || (WriteFile(h, bytes, (DWORD)len, &written, NULL) && written == len);
            CloseHandle(h);
        }
    }
    free(path);
    LeaveCriticalSection(&c->lock);
    free(bytes);
    return ok;
}

bool cache_replace(DiskCache *c, const Json *values, const char *key) {
    size_t len = 0;
    char *bytes = log_bytes(values, &len);
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    bool ok = write_atomic(c, path, bytes, len);
    free(path);
    LeaveCriticalSection(&c->lock);
    free(bytes);
    return ok;
}

void cache_remove(DiskCache *c, const char *key) {
    EnterCriticalSection(&c->lock);
    wchar_t *path = path_for(c, key);
    DeleteFileW(path);
    free(path);
    LeaveCriticalSection(&c->lock);
}

static void for_each_entry(DiskCache *c, void (*fn)(DiskCache *, const wchar_t *path, const FILETIME *modified, void *), void *arg) {
    size_t n = wcslen(c->directory) + 3;
    wchar_t *pattern = xmalloc(n * sizeof(wchar_t));
    swprintf(pattern, n, L"%ls\\*", c->directory);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t m = wcslen(c->directory) + wcslen(found.cFileName) + 2;
        wchar_t *path = xmalloc(m * sizeof(wchar_t));
        swprintf(path, m, L"%ls\\%ls", c->directory, found.cFileName);
        fn(c, path, &found.ftLastWriteTime, arg);
        free(path);
    } while (FindNextFileW(h, &found));
    FindClose(h);
}

static void delete_entry(DiskCache *c, const wchar_t *path, const FILETIME *modified, void *arg) { (void)c; (void)modified; (void)arg; DeleteFileW(path); }

void cache_remove_all(DiskCache *c) {
    EnterCriticalSection(&c->lock);
    for_each_entry(c, delete_entry, NULL);
    RemoveDirectoryW(c->directory);
    LeaveCriticalSection(&c->lock);
}

typedef struct { double age; time_t now; } PruneArgs;
static time_t filetime_to_unix(const FILETIME *ft) {
    ULARGE_INTEGER u; u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    return (time_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
static void prune_entry(DiskCache *c, const wchar_t *path, const FILETIME *modified, void *arg) {
    (void)c;
    PruneArgs *p = arg;
    if (difftime(p->now, filetime_to_unix(modified)) > p->age) DeleteFileW(path);
}
void cache_prune(DiskCache *c, double age_seconds, time_t now) {
    PruneArgs args = { age_seconds, now };
    EnterCriticalSection(&c->lock);
    for_each_entry(c, prune_entry, &args);
    LeaveCriticalSection(&c->lock);
}
