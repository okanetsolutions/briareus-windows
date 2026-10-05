// update.c: reading GitHub's latest release, comparing versions, the hosts a download may use, its checks, and the
// executable swapped in beside a backup.
#include "str.h"
#include "suites.h"
#include "test.h"
#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define BUILD "MZ the new build"
#define BUILD_SHA "c80137d000014876bbcccd9e5751c407ba713995fdadc10ced290d585800ed03"
#define BUILD_URL "https://github.com/okanetsolutions/briareus-windows/releases/download/v1.85.0/Briareus.exe"
#define ASSET_URL "https://release-assets.githubusercontent.com/github-production-release-asset/1?sig=abc"

static const char *release_json(void) {
    return "{\"tag_name\":\"v1.85.0\",\"html_url\":\"https://github.com/okanetsolutions/briareus-windows/releases/tag/v1.85.0\","
           "\"assets\":[{\"name\":\"Briareus-windows-x64.zip\",\"size\":9,\"digest\":\"sha256:" BUILD_SHA "\",\"browser_download_url\":\"https://github.com/z.zip\"},"
           "{\"name\":\"Briareus.exe\",\"size\":16,\"digest\":\"sha256:" "C80137D000014876BBCCCD9E5751C407BA713995FDADC10CED290D585800ED03" "\","
           "\"browser_download_url\":\"" BUILD_URL "\"}]}";
}

// MARK: - Stub

typedef struct { const char *url; int status; const char *location; const char *body; size_t len; bool fail; } Answer;
typedef struct { const Answer *answers; size_t count; int calls; char *last_accept; char *urls[8]; } Stub;

static bool stub_fetch(void *ctx, const char *url, const char *accept, int *status, char **location, char **body, size_t *len, char **error) {
    Stub *s = ctx;
    if (s->calls < 8) s->urls[s->calls] = xstrdup(url);
    s->calls++;
    free(s->last_accept); s->last_accept = xstrdup(accept);
    *status = 0; *location = NULL; *body = NULL; *len = 0; *error = NULL;
    for (size_t i = 0; i < s->count; i++) {
        const Answer *a = &s->answers[i];
        if (!str_eq(a->url, url)) continue;
        if (a->fail) { *error = xstrdup("The request timed out."); return false; }
        *status = a->status;
        *location = a->location ? xstrdup(a->location) : NULL;
        size_t n = a->body ? (a->len ? a->len : strlen(a->body)) : 0;
        *body = xmalloc(n + 1); if (n) memcpy(*body, a->body, n); (*body)[n] = 0; *len = n;
        return true;
    }
    *status = 404; *body = xstrdup("");
    return true;
}
static void stub_free(Stub *s) {
    free(s->last_accept);
    for (int i = 0; i < 8; i++) free(s->urls[i]);
    update_set_fetch(NULL, NULL);
}
static UpdateRelease parsed_release(void) {
    UpdateRelease r; CHECK(update_release_parse(release_json(), strlen(release_json()), &r));
    return r;
}

// MARK: - Releases and versions

static void test_a_release_reads_its_executable_with_size_and_digest(void) {
    UpdateRelease r = parsed_release();
    CHECK_STR(r.version, "1.85.0");
    CHECK_STR(r.page, "https://github.com/okanetsolutions/briareus-windows/releases/tag/v1.85.0");
    CHECK_STR(r.url, BUILD_URL);
    CHECK_STR(r.sha256, BUILD_SHA);
    CHECK_INT(r.size, 16);
    update_release_free(&r);
    CHECK(r.version == NULL);
}

static void test_a_release_without_a_checked_executable_is_refused(void) {
    UpdateRelease r;
    const char *cases[] = {
        "{\"tag_name\":\"v1.85.0\",\"assets\":[]}",
        "{\"tag_name\":\"v1.85.0\",\"assets\":[{\"name\":\"Briareus.exe\",\"size\":16,\"browser_download_url\":\"" BUILD_URL "\"}]}",
        "{\"tag_name\":\"v1.85.0\",\"assets\":[{\"name\":\"Briareus.exe\",\"size\":16,\"digest\":\"sha1:abc\",\"browser_download_url\":\"" BUILD_URL "\"}]}",
        "{\"tag_name\":\"v1.85.0\",\"assets\":[{\"name\":\"Briareus.exe\",\"size\":16,\"digest\":\"sha256:xyz\",\"browser_download_url\":\"" BUILD_URL "\"}]}",
        "{\"tag_name\":\"v1.85.0\",\"assets\":[{\"name\":\"Briareus.exe\",\"size\":0,\"digest\":\"sha256:" BUILD_SHA "\",\"browser_download_url\":\"" BUILD_URL "\"}]}",
        "{\"tag_name\":\"v1.85.0\",\"assets\":[{\"name\":\"Briareus.exe\",\"size\":16,\"digest\":\"sha256:" BUILD_SHA "\"}]}",
        "{\"assets\":[{\"name\":\"Briareus.exe\",\"size\":16,\"digest\":\"sha256:" BUILD_SHA "\",\"browser_download_url\":\"" BUILD_URL "\"}]}",
        "not json",
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK(!update_release_parse(cases[i], strlen(cases[i]), &r));
        CHECK(r.version == NULL && r.url == NULL);
    }
}

static void test_versions_compare_by_number_not_by_text(void) {
    CHECK(update_newer("v1.85.0", "1.84.0"));
    CHECK(update_newer("1.100.0", "1.99.0"));
    CHECK(update_newer("2.0.0", "1.99.9"));
    CHECK(update_newer("v1.84.1", "1.84.0"));
    CHECK(update_newer("1.84.0.1", "1.84.0"));
    CHECK(!update_newer("v1.84.0", "1.84.0"));
    CHECK(!update_newer("1.83.0", "1.84.0"));
    CHECK(!update_newer("1.85.0-beta", "1.84.0"));
    CHECK(!update_newer("", "1.84.0"));
    CHECK(!update_newer(NULL, "1.84.0"));
    CHECK(!update_newer("1.85.0", "nightly"));
    CHECK(!update_newer("1..2", "1.0"));
    CHECK(!update_newer("1.2.3.4.5", "1.0"));
}

static void test_downloads_stay_on_githubs_own_hosts(void) {
    CHECK(update_url_allowed(UPDATE_LATEST_URL));
    CHECK(update_url_allowed(BUILD_URL));
    CHECK(update_url_allowed(ASSET_URL));
    CHECK(update_url_allowed("https://objects.githubusercontent.com/x"));
    CHECK(update_url_allowed("HTTPS://GitHub.com/x"));
    CHECK(!update_url_allowed("http://github.com/x"));
    CHECK(!update_url_allowed("https://github.com.evil.example/x"));
    CHECK(!update_url_allowed("https://evilgithubusercontent.com/x"));
    CHECK(!update_url_allowed("https://githubusercontent.com/x"));
    CHECK(!update_url_allowed("https://github.com@evil.example/x"));
    CHECK(!update_url_allowed("https://github.com:8443/x"));
    CHECK(!update_url_allowed("https:///x"));
    CHECK(!update_url_allowed("/relative"));
    CHECK(!update_url_allowed(NULL));
}

static void test_sha256_matches_the_standard_vectors(void) {
    char hex[65];
    CHECK(update_sha256("abc", 3, hex));
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(update_sha256("", 0, hex));
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// MARK: - Checking and downloading

static void test_the_check_reads_the_latest_release_from_the_api(void) {
    Answer answers[] = { { UPDATE_LATEST_URL, 200, NULL, release_json(), 0, false } };
    Stub s = { answers, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &s);
    UpdateRelease r; char *error = NULL;
    CHECK(update_check(&r, &error));
    CHECK(error == NULL);
    CHECK_STR(r.version, "1.85.0");
    CHECK_STR(s.last_accept, "application/vnd.github+json");
    update_release_free(&r);
    stub_free(&s);
}

static void test_a_failed_check_says_why(void) {
    UpdateRelease r; char *error = NULL;
    Answer limited[] = { { UPDATE_LATEST_URL, 403, NULL, "{}", 0, false } };
    Stub s = { limited, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &s);
    CHECK(!update_check(&r, &error));
    CHECK(strstr(error, "limiting") != NULL); free(error); error = NULL;
    stub_free(&s);

    Answer offline[] = { { UPDATE_LATEST_URL, 0, NULL, NULL, 0, true } };
    Stub o = { offline, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &o);
    CHECK(!update_check(&r, &error));
    CHECK_STR(error, "The request timed out."); free(error); error = NULL;
    stub_free(&o);

    Answer missing[] = { { "https://elsewhere", 200, NULL, "", 0, false } };
    Stub m = { missing, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &m);
    CHECK(!update_check(&r, &error));
    CHECK_STR(error, "No release was found on GitHub."); free(error); error = NULL;
    stub_free(&m);

    Answer broken[] = { { UPDATE_LATEST_URL, 500, NULL, "", 0, false } };
    Stub b = { broken, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &b);
    CHECK(!update_check(&r, &error));
    CHECK_STR(error, "GitHub answered the update request with HTTP 500."); free(error); error = NULL;
    stub_free(&b);

    Answer bare[] = { { UPDATE_LATEST_URL, 200, NULL, "{\"tag_name\":\"v2.0.0\",\"assets\":[]}", 0, false } };
    Stub e = { bare, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &e);
    CHECK(!update_check(&r, &error));
    CHECK(strstr(error, "no Briareus.exe") != NULL); free(error);
    stub_free(&e);
}

static void test_the_download_follows_redirects_and_checks_the_bytes(void) {
    Answer answers[] = {
        { BUILD_URL, 302, ASSET_URL, "", 0, false },
        { ASSET_URL, 200, NULL, BUILD, 0, false },
    };
    Stub s = { answers, 2, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &s);
    UpdateRelease r = parsed_release();
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(update_download(&r, &bytes, &len, &error));
    CHECK(error == NULL);
    CHECK_INT(len, 16);
    CHECK(bytes && memcmp(bytes, BUILD, 16) == 0);
    CHECK_INT(s.calls, 2);
    CHECK_STR(s.urls[1], ASSET_URL);
    CHECK_STR(s.last_accept, "application/octet-stream");
    free(bytes);
    update_release_free(&r);
    stub_free(&s);
}

static void test_a_download_that_does_not_match_is_refused(void) {
    UpdateRelease r = parsed_release();
    const char *bodies[] = { "MZ the old build", "MZ short", "PE the new build" };
    for (size_t i = 0; i < sizeof bodies / sizeof *bodies; i++) {
        Answer answers[] = { { BUILD_URL, 200, NULL, bodies[i], 0, false } };
        Stub s = { answers, 1, 0, NULL, { 0 } };
        update_set_fetch(stub_fetch, &s);
        char *bytes = NULL, *error = NULL; size_t len = 0;
        CHECK(!update_download(&r, &bytes, &len, &error));
        CHECK(bytes == NULL && len == 0);
        CHECK(strstr(error, "does not match") != NULL);
        free(error);
        stub_free(&s);
    }
    update_release_free(&r);
}

static void test_a_redirect_off_github_or_in_a_loop_is_not_followed(void) {
    UpdateRelease r = parsed_release();
    Answer away[] = { { BUILD_URL, 302, "https://evil.example/Briareus.exe", "", 0, false } };
    Stub s = { away, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &s);
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(!update_download(&r, &bytes, &len, &error));
    CHECK_INT(s.calls, 1);
    CHECK(strstr(error, "evil.example") != NULL); free(error); error = NULL;
    stub_free(&s);

    Answer loop[] = { { BUILD_URL, 302, BUILD_URL, "", 0, false } };
    Stub l = { loop, 1, 0, NULL, { 0 } };
    update_set_fetch(stub_fetch, &l);
    CHECK(!update_download(&r, &bytes, &len, &error));
    CHECK_INT(l.calls, 6);
    CHECK_STR(error, "The update was redirected too many times."); free(error);
    stub_free(&l);
    update_release_free(&r);
}

// MARK: - Installing

static wchar_t *temp_exe(void) {
    static LONG counter;
    wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
    size_t n = wcslen(temp) + 96;
    wchar_t *path = xmalloc(n * sizeof *path);
    swprintf(path, n, L"%lsbriareus-update-tests-%lu-%ld.exe", temp, (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&counter));
    return path;
}
static wchar_t *with(const wchar_t *path, const wchar_t *suffix) {
    size_t n = wcslen(path) + wcslen(suffix) + 1;
    wchar_t *out = xmalloc(n * sizeof *out);
    swprintf(out, n, L"%ls%ls", path, suffix);
    return out;
}
static void write_file(const wchar_t *path, const char *text) { FILE *f = _wfopen(path, L"wb"); if (f) { fputs(text, f); fclose(f); } }
static char *read_file(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char buf[256]; size_t got = fread(buf, 1, sizeof buf - 1, f); buf[got] = 0;
    fclose(f);
    return xstrdup(buf);
}
static bool exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static void test_the_new_build_replaces_the_old_which_is_kept_until_cleanup(void) {
    wchar_t *exe = temp_exe(), *old = with(exe, L".old"), *fresh = with(exe, L".new");
    write_file(exe, "MZ the old build");
    write_file(old, "MZ an older backup");
    char *error = NULL;
    CHECK(update_install(exe, BUILD, strlen(BUILD), &error));
    CHECK(error == NULL);
    CHECK_OWNED_STR(read_file(exe), BUILD);
    CHECK_OWNED_STR(read_file(old), "MZ the old build");
    CHECK(!exists(fresh));
    CHECK(update_cleanup(exe));
    CHECK(!exists(old));
    CHECK(update_cleanup(exe));
    DeleteFileW(exe);
    free(exe); free(old); free(fresh);
}

static void test_an_install_that_cannot_move_the_executable_leaves_it_alone(void) {
    wchar_t *exe = temp_exe(), *fresh = with(exe, L".new"), *old = with(exe, L".old");
    // Nothing to move aside: the new file must not stay behind.
    char *error = NULL;
    CHECK(!update_install(exe, BUILD, strlen(BUILD), &error));
    CHECK(error && strstr(error, "release page") != NULL); free(error); error = NULL;
    CHECK(!exists(fresh) && !exists(exe));
    // An executable held open without sharing delete cannot be renamed, so it stays where it was.
    write_file(exe, "MZ the old build");
    HANDLE held = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(!update_install(exe, BUILD, strlen(BUILD), &error));
    free(error); error = NULL;
    CloseHandle(held);
    CHECK_OWNED_STR(read_file(exe), "MZ the old build");
    CHECK(!exists(fresh) && !exists(old));
    // A folder it cannot write in.
    wchar_t *nowhere = with(exe, L".missing\\Briareus.exe");
    CHECK(!update_install(nowhere, BUILD, strlen(BUILD), &error));
    CHECK(error && strstr(error, "could not be saved") != NULL); free(error);
    DeleteFileW(exe);
    free(exe); free(fresh); free(old); free(nowhere);
}

void update_tests(void) {
    test_run("a release reads its executable with size and digest", test_a_release_reads_its_executable_with_size_and_digest);
    test_run("a release without a checked executable is refused", test_a_release_without_a_checked_executable_is_refused);
    test_run("versions compare by number not by text", test_versions_compare_by_number_not_by_text);
    test_run("downloads stay on GitHub's own hosts", test_downloads_stay_on_githubs_own_hosts);
    test_run("SHA-256 matches the standard vectors", test_sha256_matches_the_standard_vectors);
    test_run("the check reads the latest release from the API", test_the_check_reads_the_latest_release_from_the_api);
    test_run("a failed check says why", test_a_failed_check_says_why);
    test_run("the download follows redirects and checks the bytes", test_the_download_follows_redirects_and_checks_the_bytes);
    test_run("a download that does not match is refused", test_a_download_that_does_not_match_is_refused);
    test_run("a redirect off GitHub or in a loop is not followed", test_a_redirect_off_github_or_in_a_loop_is_not_followed);
    test_run("the new build replaces the old, which is kept until cleanup", test_the_new_build_replaces_the_old_which_is_kept_until_cleanup);
    test_run("an install that cannot move the executable leaves it alone", test_an_install_that_cannot_move_the_executable_leaves_it_alone);
}
