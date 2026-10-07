// The disk cache: file names for keys, values and logs, rewrites only when the bytes change, removal and pruning by age.
#include "cache.h"
#include "json.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// A directory of its own under %TEMP%, not created yet.
static wchar_t *fresh_directory(void) {
    static LONG counter;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    size_t n = wcslen(temp) + 96;
    wchar_t *dir = xmalloc(n * sizeof(wchar_t));
    swprintf(dir, n, L"%lsbriareus-cache-tests-%lu-%llu-%ld", temp, (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
             (long)InterlockedIncrement(&counter));
    return dir;
}
static void remove_tree(const wchar_t *dir) {
    size_t n = wcslen(dir) + 3;
    wchar_t *pattern = xmalloc(n * sizeof(wchar_t));
    swprintf(pattern, n, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            size_t m = wcslen(dir) + wcslen(f.cFileName) + 2;
            wchar_t *path = xmalloc(m * sizeof(wchar_t));
            swprintf(path, m, L"%ls\\%ls", dir, f.cFileName);
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path); else DeleteFileW(path);
            free(path);
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
static bool exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static wchar_t *entry_path(const wchar_t *dir, const char *key) {
    char *name = cache_file_name(key);
    wchar_t *wname = utf8_to_wide(name);
    size_t n = wcslen(dir) + wcslen(wname) + 2;
    wchar_t *path = xmalloc(n * sizeof(wchar_t));
    swprintf(path, n, L"%ls\\%ls", dir, wname);
    free(name); free(wname);
    return path;
}
static char *read_entry(const wchar_t *dir, const char *key) {
    wchar_t *path = entry_path(dir, key);
    FILE *f = _wfopen(path, L"rb");
    free(path);
    if (!f) return NULL;
    Str s; str_init(&s);
    char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) str_append(&s, buf, got);
    fclose(f);
    if (!s.data) str_appendz(&s, "");
    return str_detach(&s);
}
static void write_entry(const wchar_t *dir, const char *key, const char *bytes, size_t len) {
    wchar_t *path = entry_path(dir, key);
    FILE *f = _wfopen(path, L"wb");
    CHECK(f != NULL);
    if (f) { fwrite(bytes, 1, len, f); fclose(f); }
    free(path);
}
static int count_entries(const wchar_t *dir) {
    size_t n = wcslen(dir) + 3;
    wchar_t *pattern = xmalloc(n * sizeof(wchar_t));
    swprintf(pattern, n, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    free(pattern);
    int count = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++; } while (FindNextFileW(h, &f));
    FindClose(h);
    return count;
}
static FILETIME unix_to_filetime(time_t t) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)t * 10000000ULL + 116444736000000000ULL;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    return ft;
}
static void set_modified(const wchar_t *dir, const char *key, time_t when) {
    wchar_t *path = entry_path(dir, key);
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    FILETIME ft = unix_to_filetime(when);
    CHECK(SetFileTime(h, NULL, NULL, &ft));
    CloseHandle(h); free(path);
}
static ULONGLONG modified(const wchar_t *dir, const char *key) {
    wchar_t *path = entry_path(dir, key);
    WIN32_FILE_ATTRIBUTE_DATA data;
    ULONGLONG t = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data)) t = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    free(path);
    return t;
}

// MARK: - File names

static void test_file_names_keep_only_letters_and_digits(void) {
    CHECK_OWNED_STR(cache_file_name("projects"), "projects");
    CHECK_OWNED_STR(cache_file_name("AZaz09"), "AZaz09");
    CHECK_OWNED_STR(cache_file_name(""), "_");
    CHECK_OWNED_STR(cache_file_name(NULL), "_");
    CHECK_OWNED_STR(cache_file_name("sessions:o/r"), "sessions%3Ao%2Fr");
    CHECK_OWNED_STR(cache_file_name("."), "%2E");
    CHECK_OWNED_STR(cache_file_name(".."), "%2E%2E");
    CHECK_OWNED_STR(cache_file_name("../../etc/passwd"), "%2E%2E%2F%2E%2E%2Fetc%2Fpasswd");
    CHECK_OWNED_STR(cache_file_name("C:\\Windows"), "C%3A%5CWindows");
    CHECK_OWNED_STR(cache_file_name("a b"), "a%20b");
    CHECK_OWNED_STR(cache_file_name("_"), "%5F");
    CHECK_OWNED_STR(cache_file_name("-"), "%2D");
    CHECK_OWNED_STR(cache_file_name("%"), "%25");
    CHECK_OWNED_STR(cache_file_name("*?\"<>|"), "%2A%3F%22%3C%3E%7C");
    CHECK_OWNED_STR(cache_file_name("\t\n"), "%09%0A");
    CHECK_OWNED_STR(cache_file_name("\xC3\xA9"), "%C3%A9");
    CHECK_OWNED_STR(cache_file_name("\xF0\x9F\x98\x80"), "%F0%9F%98%80");
    CHECK_OWNED_STR(cache_file_name("pull_files:o/r#12@abc"), "pull%5Ffiles%3Ao%2Fr%2312%40abc");
    CHECK_OWNED_STR(cache_file_name("\x7F\xFF"), "%7F%FF");
}
static void test_file_names_never_carry_separators_dots_or_reserved_characters(void) {
    Str key; str_init(&key);
    for (int c = 1; c < 256; c++) str_appendc(&key, (char)c);
    char *name = cache_file_name(key.data);
    CHECK_INT(strlen(name), 62 + (255 - 62) * 3);
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '%');
    }
    free(name); str_free(&key);
}
static void test_similar_keys_get_different_file_names(void) {
    const char *keys[] = { "", "_", "%5F", "a/b", "a_b", "a.b", "a%2Fb", "a%252Fb", "a:b", "ab", "a b", "a+b", "a/b/", "/a/b", "a//b", "a\\b",
                           "sessions:o/r", "sessions:o/r ", "sessions:o_r", "sessions%3Ao%2Fr", "\xC3\xA9", "%C3%A9", "e" };
    size_t n = sizeof keys / sizeof *keys;
    char *names[sizeof keys / sizeof *keys];
    for (size_t i = 0; i < n; i++) names[i] = cache_file_name(keys[i]);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            if (str_eq(names[i], names[j])) printf("  \"%s\" and \"%s\" share %s\n", keys[i], keys[j], names[i]);
            CHECK(!str_eq(names[i], names[j]));
        }
    for (size_t i = 0; i < n; i++) free(names[i]);
}

// MARK: - Values

static void test_values_round_trip_through_a_directory_made_on_first_write(void) {
    wchar_t *root = fresh_directory();
    size_t n = wcslen(root) + 32;
    wchar_t *dir = xmalloc(n * sizeof(wchar_t));
    swprintf(dir, n, L"%ls\\nested\\deeper", root);
    DiskCache *cache = cache_new(dir);
    CHECK(!exists(root));
    CHECK(cache_value(cache, "projects") == NULL);
    CHECK(!exists(dir));
    Json *value = json_parsez("{\"z\":1,\"a\":[true,false,null,1.5,-2,\"\xC3\xA9\\n\\\"q\\\"\"],\"m\":{\"y\":{},\"x\":[]},\"big\":1727000000000}");
    CHECK(cache_store(cache, value, "projects"));
    CHECK(exists(dir));
    Json *back = cache_value(cache, "projects");
    CHECK(back != NULL); CHECK(json_equal(back, value));
    json_free(back);
    // Keys come back sorted on disk, so equal values are equal bytes.
    char *sorted = json_serialize(value, true);
    CHECK_OWNED_STR(read_entry(dir, "projects"), sorted);
    free(sorted);
    // Values of every JSON shape.
    const char *shapes[] = { "null", "true", "0", "\"text\"", "[]", "{}", "[[[]]]" };
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        Json *v = json_parsez(shapes[i]);
        CHECK(cache_store(cache, v, "shape"));
        Json *got = cache_value(cache, "shape");
        CHECK(got != NULL && json_equal(got, v));
        json_free(got); json_free(v);
    }
    // No temporary files are left behind.
    CHECK_INT(count_entries(dir), 2);
    json_free(value);
    cache_remove_all(cache);
    cache_free(cache);
    remove_tree(root);
    CHECK(!exists(root));
    free(dir); free(root);
}
static void test_bytes_round_trip_as_they_are(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    char *data = NULL; size_t len = 0;
    CHECK(!cache_bytes(cache, "repo-index", &data, &len));
    const char bytes[] = "BRIAREUS-INDEX 1\n\0binary\xFF";
    CHECK(cache_store_bytes(cache, bytes, sizeof bytes - 1, "repo-index"));
    CHECK(cache_bytes(cache, "repo-index", &data, &len));
    CHECK_INT(len, sizeof bytes - 1);
    CHECK(data && memcmp(data, bytes, len) == 0 && data[len] == 0);
    free(data);
    CHECK(cache_store_bytes(cache, "", 0, "repo-index"));
    CHECK(cache_bytes(cache, "repo-index", &data, &len)); CHECK_INT(len, 0);
    free(data);
    cache_remove_all(cache);
    cache_free(cache);
    remove_tree(dir);
    free(dir);
}
static void test_keys_with_odd_characters_are_kept_apart(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    const char *keys[] = { "a/b", "a_b", "a.b", "a%2Fb", "..", ".", "", "\xC3\xA9t\xC3\xA9", "a:b", "a\\b", "x*?<>|\"" };
    size_t n = sizeof keys / sizeof *keys;
    for (size_t i = 0; i < n; i++) {
        Json *v = json_object(); json_set_num(v, "i", (double)i);
        if (!cache_store(cache, v, keys[i])) printf("  could not store \"%s\"\n", keys[i]);
        json_free(v);
    }
    for (size_t i = 0; i < n; i++) {
        Json *v = cache_value(cache, keys[i]);
        if (!v) printf("  lost \"%s\"\n", keys[i]);
        CHECK_INT(json_int_or(json_get(v, "i"), -1), (int)i);
        json_free(v);
    }
    CHECK_INT(count_entries(dir), (int)n);
    // Nothing was written outside the directory.
    wchar_t *parent = xmalloc((wcslen(dir) + 1) * sizeof(wchar_t)); wcscpy(parent, dir);
    *wcsrchr(parent, L'\\') = 0;
    size_t m = wcslen(parent) + 8;
    wchar_t *outside = xmalloc(m * sizeof(wchar_t)); swprintf(outside, m, L"%ls\\b", parent);
    CHECK(!exists(outside));
    free(outside); free(parent);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_long_keys_work_up_to_the_path_limit_and_fail_quietly_beyond(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    char key[300];
    // As long as the path allows where long paths are off (as on CI): the directory, a separator, the key and ".tmp".
    size_t room = MAX_PATH - 1 - wcslen(dir) - 1 - 4, len = room < 200 ? room : 200;
    memset(key, 'k', len); key[len] = 0;
    Json *v = json_parsez("{\"ok\":true}");
    CHECK(cache_store(cache, v, key));
    Json *got = cache_value(cache, key); CHECK(got != NULL && json_equal(got, v)); json_free(got);
    // 100 slashes encode to 300 characters, past what a file name may hold: nothing is kept and nothing breaks.
    memset(key, '/', 100); key[100] = 0;
    CHECK(!cache_store(cache, v, key));
    CHECK(cache_value(cache, key) == NULL);
    Json *lines = cache_lines(cache, key); CHECK(json_count(lines) == 0); json_free(lines);
    cache_remove(cache, key);
    json_free(v);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_an_unchanged_value_is_not_rewritten(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    Json *a = json_parsez("{\"b\":2,\"a\":1,\"list\":[{\"y\":1,\"x\":2}]}");
    Json *same = json_parsez("{\"list\":[{\"x\":2,\"y\":1}],\"a\":1,\"b\":2}");
    Json *changed = json_parsez("{\"a\":1,\"b\":3,\"list\":[{\"x\":2,\"y\":1}]}");
    CHECK(cache_store(cache, a, "k"));
    time_t old = time(NULL) - 86400;
    set_modified(dir, "k", old);
    ULONGLONG before = modified(dir, "k");
    CHECK(before != 0);
    // The same value in another key order costs no write.
    CHECK(cache_store(cache, same, "k"));
    CHECK(modified(dir, "k") == before);
    CHECK(cache_store(cache, a, "k"));
    CHECK(modified(dir, "k") == before);
    CHECK(cache_store(cache, changed, "k"));
    CHECK(modified(dir, "k") > before);
    Json *got = cache_value(cache, "k"); CHECK(json_equal(got, changed)); json_free(got);
    // A value that only grows a prefix of the old bytes is still written.
    set_modified(dir, "k", old);
    Json *longer = json_parsez("{\"a\":1,\"b\":3,\"list\":[{\"x\":2,\"y\":1}],\"z\":0}");
    CHECK(cache_store(cache, longer, "k"));
    got = cache_value(cache, "k"); CHECK(json_equal(got, longer)); json_free(got);
    json_free(longer); json_free(a); json_free(same); json_free(changed);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_a_damaged_entry_reads_as_missing_and_is_replaced(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    Json *v = json_parsez("{\"a\":1}");
    CHECK(cache_store(cache, v, "k"));
    const char *damaged[] = { "", "{", "{\"a\":1", "garbage", "{\"a\":1}{\"b\":2}", "\xFF\xFE{" };
    for (size_t i = 0; i < sizeof damaged / sizeof *damaged; i++) {
        write_entry(dir, "k", damaged[i], strlen(damaged[i]));
        Json *got = cache_value(cache, "k");
        if (got) printf("  read damaged entry %zu\n", i);
        CHECK(got == NULL); json_free(got);
        CHECK(cache_store(cache, v, "k"));
        got = cache_value(cache, "k"); CHECK(got != NULL && json_equal(got, v)); json_free(got);
    }
    // A cut-short write the same length as the value is not mistaken for it.
    char *bytes = json_serialize(v, true);
    size_t len = strlen(bytes);
    char *wrong = xstrdup(bytes); wrong[len - 1] = ' ';
    write_entry(dir, "k", wrong, len);
    CHECK(cache_store(cache, v, "k"));
    CHECK_OWNED_STR(read_entry(dir, "k"), bytes);
    free(wrong); free(bytes); json_free(v);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}

// MARK: - Logs

static void test_lines_append_and_replace(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    Json *none = cache_lines(cache, "log");
    CHECK(none != NULL && json_is_array(none) && json_count(none) == 0); json_free(none);
    CHECK(!exists(dir));
    Json *first = json_parsez("[{\"seq\":1,\"t\":\"a\\nb\"},{\"seq\":2}]");
    Json *second = json_parsez("[{\"seq\":3,\"b\":1,\"a\":2}]");
    CHECK(cache_append(cache, first, "log"));
    // Each value goes on its own line, after a newline, in the order its keys were given.
    CHECK_OWNED_STR(read_entry(dir, "log"), "\n{\"seq\":1,\"t\":\"a\\nb\"}\n{\"seq\":2}");
    CHECK(cache_append(cache, second, "log"));
    CHECK_OWNED_STR(read_entry(dir, "log"), "\n{\"seq\":1,\"t\":\"a\\nb\"}\n{\"seq\":2}\n{\"seq\":3,\"b\":1,\"a\":2}");
    Json *lines = cache_lines(cache, "log");
    CHECK_INT(json_count(lines), 3);
    CHECK_STR(json_str(json_get(json_at(lines, 0), "t")), "a\nb");
    CHECK_INT(json_int_or(json_get(json_at(lines, 2), "seq"), 0), 3);
    json_free(lines);
    // Appending nothing changes nothing.
    Json *empty = json_array();
    CHECK(cache_append(cache, empty, "log"));
    lines = cache_lines(cache, "log"); CHECK_INT(json_count(lines), 3); json_free(lines);
    // Replace starts the log over; replacing with nothing leaves an empty log.
    CHECK(cache_replace(cache, second, "log"));
    CHECK_OWNED_STR(read_entry(dir, "log"), "\n{\"seq\":3,\"b\":1,\"a\":2}");
    CHECK(cache_replace(cache, empty, "log"));
    CHECK(exists(dir));
    CHECK_OWNED_STR(read_entry(dir, "log"), "");
    lines = cache_lines(cache, "log"); CHECK_INT(json_count(lines), 0); json_free(lines);
    CHECK(cache_append(cache, first, "log"));
    lines = cache_lines(cache, "log"); CHECK_INT(json_count(lines), 2); json_free(lines);
    // An empty append to a new key makes an empty log.
    CHECK(cache_append(cache, empty, "new"));
    lines = cache_lines(cache, "new"); CHECK(json_is_array(lines) && json_count(lines) == 0); json_free(lines);
    // Values that are not objects are lines too.
    Json *mixed = json_parsez("[1,\"two\",[3],null]");
    CHECK(cache_replace(cache, mixed, "mixed"));
    lines = cache_lines(cache, "mixed"); CHECK(json_equal(lines, mixed)); json_free(lines);
    json_free(mixed); json_free(empty); json_free(first); json_free(second);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_damaged_lines_cost_only_themselves(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    Json *seed = json_parsez("[{\"seq\":0}]");
    CHECK(cache_replace(cache, seed, "log"));
    const char *text = "{\"seq\":1}\n\n{\"seq\":\ngarbage\n{\"seq\":2}\r\n{\"seq\":3}\n\n\n{\"seq\":4}";
    write_entry(dir, "log", text, strlen(text));
    Json *lines = cache_lines(cache, "log");
    int seen[5] = { 0 };
    for (size_t i = 0; i < json_count(lines); i++) { int s = json_int_or(json_get(json_at(lines, i), "seq"), -1); if (s >= 0 && s < 5) seen[s]++; }
    CHECK(seen[1] == 1 && seen[3] == 1 && seen[4] == 1);
    CHECK(json_count(lines) >= 3 && json_count(lines) <= 4);
    json_free(lines);
    // A log interrupted mid-line keeps what came before and after the cut.
    write_entry(dir, "log", "\n{\"seq\":1}\n{\"se", 15);
    Json *more = json_parsez("[{\"seq\":2}]");
    CHECK(cache_append(cache, more, "log"));
    lines = cache_lines(cache, "log");
    CHECK_INT(json_count(lines), 2);
    CHECK_INT(json_int_or(json_get(json_at(lines, 0), "seq"), 0), 1); CHECK_INT(json_int_or(json_get(json_at(lines, 1), "seq"), 0), 2);
    json_free(lines);
    // A file holding garbage alone reads as an empty log, never NULL.
    write_entry(dir, "log", "\xFF\xFF\xFF", 3);
    lines = cache_lines(cache, "log"); CHECK(lines != NULL && json_count(lines) == 0); json_free(lines);
    json_free(more); json_free(seed);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_values_and_logs_under_the_same_key_share_one_file(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    Json *v = json_parsez("{\"b\":1,\"a\":2}");
    CHECK(cache_store(cache, v, "k"));
    // A stored value reads as a log of one line.
    Json *lines = cache_lines(cache, "k"); CHECK_INT(json_count(lines), 1); CHECK(json_equal(json_at(lines, 0), v)); json_free(lines);
    // A log of one line reads as a value; of two, it does not.
    Json *one = json_parsez("[{\"x\":1}]"), *two = json_parsez("[{\"x\":1},{\"x\":2}]");
    CHECK(cache_replace(cache, one, "k"));
    Json *got = cache_value(cache, "k"); CHECK(got != NULL && json_equal(got, json_at(one, 0))); json_free(got);
    CHECK(cache_replace(cache, two, "k"));
    CHECK(cache_value(cache, "k") == NULL);
    json_free(one); json_free(two); json_free(v);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}

// MARK: - Removal and pruning

static void test_remove_takes_one_key_and_remove_all_the_directory(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    // Nothing to remove yet.
    cache_remove(cache, "missing");
    cache_remove_all(cache);
    CHECK(!exists(dir));
    Json *v = json_parsez("{\"a\":1}"), *log = json_parsez("[1,2]");
    CHECK(cache_store(cache, v, "a")); CHECK(cache_store(cache, v, "b")); CHECK(cache_append(cache, log, "c"));
    CHECK_INT(count_entries(dir), 3);
    cache_remove(cache, "a");
    CHECK(cache_value(cache, "a") == NULL);
    Json *got = cache_value(cache, "b"); CHECK(got != NULL); json_free(got);
    CHECK_INT(count_entries(dir), 2);
    cache_remove(cache, "a");
    cache_remove(cache, "c");
    Json *lines = cache_lines(cache, "c"); CHECK_INT(json_count(lines), 0); json_free(lines);
    // A file the cache did not write is cleared too.
    write_entry(dir, "stray", "x", 1);
    cache_remove_all(cache);
    CHECK(!exists(dir));
    CHECK(cache_value(cache, "b") == NULL);
    // The cache can be written again afterwards.
    CHECK(cache_store(cache, v, "b"));
    got = cache_value(cache, "b"); CHECK(got != NULL); json_free(got);
    json_free(v); json_free(log);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_prune_drops_entries_older_than_the_age(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *cache = cache_new(dir);
    // Pruning a directory that does not exist is a no-op.
    cache_prune(cache, 60, time(NULL));
    Json *v = json_parsez("{\"a\":1}"), *log = json_parsez("[{\"seq\":1}]");
    time_t now = 1790000000;
    const char *keys[] = { "ancient", "old", "edge", "inside", "fresh", "future", "log" };
    time_t ages[] = { 365 * 86400, 3601, 3600, 3599, 0, -86400, 7200 };
    for (size_t i = 0; i < 7; i++) {
        if (str_eq(keys[i], "log")) CHECK(cache_append(cache, log, keys[i])); else CHECK(cache_store(cache, v, keys[i]));
        set_modified(dir, keys[i], now - ages[i]);
    }
    cache_prune(cache, 3600, now);
    CHECK(cache_value(cache, "ancient") == NULL);
    CHECK(cache_value(cache, "old") == NULL);
    Json *lines = cache_lines(cache, "log"); CHECK_INT(json_count(lines), 0); json_free(lines);
    // Exactly the age is kept; only older goes.
    const char *kept[] = { "edge", "inside", "fresh", "future" };
    for (size_t i = 0; i < 4; i++) { Json *got = cache_value(cache, kept[i]); if (!got) printf("  pruned %s\n", kept[i]); CHECK(got != NULL); json_free(got); }
    CHECK_INT(count_entries(dir), 4);
    // An unchanged store does not touch the file, so it does not keep an entry alive.
    set_modified(dir, "edge", now - 7200);
    CHECK(cache_store(cache, v, "edge"));
    cache_prune(cache, 3600, now);
    CHECK(cache_value(cache, "edge") == NULL);
    // An append does.
    CHECK(cache_append(cache, log, "log"));
    set_modified(dir, "log", time(NULL) - 7200);
    CHECK(cache_append(cache, log, "log"));
    cache_prune(cache, 3600, time(NULL));
    lines = cache_lines(cache, "log"); CHECK_INT(json_count(lines), 2); json_free(lines);
    // A touch does, without changing the entry; there is nothing to touch under a key never written.
    CHECK(cache_store(cache, v, "read"));
    set_modified(dir, "read", time(NULL) - 7200);
    CHECK(cache_touch(cache, "read"));
    cache_prune(cache, 3600, time(NULL));
    Json *touched = cache_value(cache, "read"); CHECK(touched != NULL && json_equal(touched, v)); json_free(touched);
    CHECK(!cache_touch(cache, "never"));
    // Pruning leaves the directory in place, even when it empties it.
    cache_prune(cache, 0, now + 10L * 365 * 86400);
    CHECK_INT(count_entries(dir), 0);
    CHECK(exists(dir));
    json_free(v); json_free(log);
    cache_remove_all(cache); cache_free(cache);
    remove_tree(dir); free(dir);
}
static void test_two_caches_on_one_directory_see_each_others_writes(void) {
    wchar_t *dir = fresh_directory();
    DiskCache *a = cache_new(dir), *b = cache_new(dir);
    Json *v = json_parsez("{\"from\":\"a\"}");
    CHECK(cache_store(a, v, "k"));
    Json *got = cache_value(b, "k"); CHECK(got != NULL && json_equal(got, v)); json_free(got);
    cache_remove(b, "k");
    CHECK(cache_value(a, "k") == NULL);
    json_free(v);
    cache_free(b);
    cache_remove_all(a); cache_free(a);
    cache_free(NULL);
    remove_tree(dir); free(dir);
}

void cache_tests(void) {
    test_run("file names keep only letters and digits", test_file_names_keep_only_letters_and_digits);
    test_run("file names never carry separators dots or reserved characters", test_file_names_never_carry_separators_dots_or_reserved_characters);
    test_run("similar keys get different file names", test_similar_keys_get_different_file_names);
    test_run("values round trip through a directory made on first write", test_values_round_trip_through_a_directory_made_on_first_write);
    test_run("keys with odd characters are kept apart", test_keys_with_odd_characters_are_kept_apart);
    test_run("long keys work up to the path limit and fail quietly beyond", test_long_keys_work_up_to_the_path_limit_and_fail_quietly_beyond);
    test_run("an unchanged value is not rewritten", test_an_unchanged_value_is_not_rewritten);
    test_run("a damaged entry reads as missing and is replaced", test_a_damaged_entry_reads_as_missing_and_is_replaced);
    test_run("lines append and replace", test_lines_append_and_replace);
    test_run("damaged lines cost only themselves", test_damaged_lines_cost_only_themselves);
    test_run("values and logs under the same key share one file", test_values_and_logs_under_the_same_key_share_one_file);
    test_run("remove takes one key and remove all the directory", test_remove_takes_one_key_and_remove_all_the_directory);
    test_run("prune drops entries older than the age", test_prune_drops_entries_older_than_the_age);
    test_run("two caches on one directory see each other's writes", test_two_caches_on_one_directory_see_each_others_writes);
    test_run("bytes round trip as they are", test_bytes_round_trip_as_they_are);
}
