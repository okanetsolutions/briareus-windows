#include "remote_image.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static const char PNG[] = "\x89PNG\r\n\x1A\n\0\0\0\rIHDR";

typedef struct {
    int status; const char *body; size_t body_len; const char *failure;
    int calls; char *method, *url, *header_names; size_t sent;
} Stub;

static bool stub_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body,
                           size_t body_len, int timeout_ms, int *status, char **content_type, char **retry_after,
                           char **response, size_t *response_len, char **error_message) {
    (void)body; (void)timeout_ms;
    Stub *s = ctx;
    s->calls++; s->sent = body_len;
    free(s->method); free(s->url); free(s->header_names);
    s->method = xstrdup(method); s->url = xstrdup(url);
    Str names; str_init(&names);
    for (size_t i = 0; headers && headers[i]; i += 2) str_appendf(&names, "%s;", headers[i]);
    s->header_names = str_detach(&names);
    *content_type = NULL; *retry_after = NULL; *response = NULL; *response_len = 0; *error_message = NULL; *status = 0;
    if (s->failure) { *error_message = xstrdup(s->failure); return false; }
    *status = s->status; *content_type = xstrdup("application/octet-stream");
    *response = xmalloc(s->body_len + 1); memcpy(*response, s->body, s->body_len); (*response)[s->body_len] = 0;
    *response_len = s->body_len;
    return true;
}

static void stub_free(Stub *s) { free(s->method); free(s->url); free(s->header_names); remote_image_set_transport(NULL, NULL); }

static void test_only_https_addresses_without_credentials_are_fetched(void) {
    CHECK(remote_image_address("https://files.example/files/abc/shot.png"));
    CHECK(remote_image_address("HTTPS://Files.Example/x"));
    CHECK(remote_image_address("https://files.example:8443/x"));
    CHECK(!remote_image_address("http://files.example/shot.png"));
    CHECK(!remote_image_address("file:///C:/shot.png"));
    CHECK(!remote_image_address("https://"));
    CHECK(!remote_image_address("https:///shot.png"));
    CHECK(!remote_image_address("https://:443/shot.png"));
    CHECK(!remote_image_address("https://user:secret@files.example/shot.png"));
    CHECK(!remote_image_address("https://files.example%2Fevil/shot.png"));
    // Nothing between the host and the path that another reader of the address could take for the host.
    CHECK(!remote_image_address("https://files.example?@evil.test/shot.png"));
    CHECK(!remote_image_address("https://files.example#@evil.test/shot.png"));
    CHECK(!remote_image_address("https://files.example?shot.png"));
    CHECK(!remote_image_address("https://files.example;evil.test/shot.png"));
    CHECK(!remote_image_address("https://files.example,evil.test/shot.png"));
    CHECK(remote_image_address("https://[2001:db8::1]:8443/shot.png"));
    CHECK(remote_image_address("https://files.example"));
    CHECK(!remote_image_address("https://files.example/a b.png"));
    CHECK(!remote_image_address("https://files.example\\shot.png"));
    CHECK(!remote_image_address("https://files.example/shot.png\n"));
    CHECK(!remote_image_address(NULL));
}

static void test_a_picture_link_is_told_by_its_path(void) {
    CHECK(remote_image_url("https://files.example/files/abc/shot.png"));
    CHECK(remote_image_url("https://files.example/a/Photo.JPG"));
    CHECK(remote_image_url("https://files.example/a.jpeg?size=large"));
    CHECK(remote_image_url("https://files.example/a.gif#top"));
    CHECK(remote_image_url("https://files.example/a.webp"));
    CHECK(remote_image_url("https://files.example/a.bmp"));
    CHECK(!remote_image_url("https://files.example/a.svg"));
    CHECK(!remote_image_url("https://files.example/report.pdf"));
    CHECK(!remote_image_url("https://files.example/a.png/download"));
    CHECK(!remote_image_url("https://files.example/page?file=a.png"));
    CHECK(!remote_image_url("https://files.example.png"));
    CHECK(!remote_image_url("https://files.example/png"));
    CHECK(!remote_image_url("http://files.example/a.png"));
}

static void test_names_and_hosts_are_read_from_the_address(void) {
    CHECK_OWNED_STR(remote_image_name("https://files.example/files/abc/shot.png?x=1#f"), "shot.png");
    CHECK_OWNED_STR(remote_image_name("https://files.example/files/abc/before%20after.png"), "before after.png");
    CHECK_OWNED_STR(remote_image_name("https://files.example/a%2Fb%0A.png"), "a_b_.png");
    CHECK_OWNED_STR(remote_image_name("https://files.example/100%.png"), "100%.png");
    CHECK_OWNED_STR(remote_image_name("https://Files.Example/"), "files.example");
    CHECK_OWNED_STR(remote_image_name("http://files.example/a.png"), "");
    CHECK_OWNED_STR(remote_image_host("https://Files.Example:443/a.png"), "files.example");
    CHECK_OWNED_STR(remote_image_host("https://files.example:8443/a.png"), "files.example:8443");
    CHECK(remote_image_host("ftp://files.example/a.png") == NULL);
}

static void test_only_the_paired_origin_is_its_own(void) {
    CHECK(remote_image_from_origin("https://briareus.example/files/abc/shot.png", "https://briareus.example"));
    CHECK(remote_image_from_origin("https://BRIAREUS.example:443/files/abc/shot.png", "https://briareus.example"));
    CHECK(remote_image_from_origin("https://briareus.example:8443/shot.png", "https://briareus.example:8443"));
    CHECK(!remote_image_from_origin("https://briareus.example:8443/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://briareus.example.evil.test/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://evil.test/briareus.example/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://briareus.example?@evil.test/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://briareus.example#@evil.test/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://briareus.example@evil.test/shot.png", "https://briareus.example"));
    CHECK(!remote_image_from_origin("https://briareus.example/shot.png", NULL));
    CHECK(!remote_image_from_origin("http://briareus.example/shot.png", "https://briareus.example"));
}

static void test_picture_files_are_told_by_their_first_bytes(void) {
    CHECK(remote_image_recognized(PNG, sizeof PNG - 1));
    CHECK(remote_image_recognized("\xFF\xD8\xFF\xE0\0\x10JFIF\0\1", 12));
    CHECK(remote_image_recognized("GIF89a\1\0\1\0\0\0", 12));
    CHECK(remote_image_recognized("RIFF\x24\0\0\0WEBPVP8 ", 16));
    CHECK(remote_image_recognized("BM\x46\0\0\0\0\0\0\0\x36\0", 12));
    CHECK(!remote_image_recognized("<!doctype html><html>", 21));
    CHECK(!remote_image_recognized("<svg xmlns=\"http://www.w3.org/2000/svg\"/>", 41));
    CHECK(!remote_image_recognized("RIFF\x24\0\0\0WAVEfmt ", 16));
    CHECK(!remote_image_recognized("\x89PNG", 4));
    CHECK(!remote_image_recognized(NULL, 0));
}

static void test_a_download_is_one_get_without_the_token(void) {
    Stub s = { .status = 200, .body = PNG, .body_len = sizeof PNG - 1 };
    remote_image_set_transport(stub_transport, &s);
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(remote_image_download("https://briareus.example/files/abc/shot.png?v=2#frag", &bytes, &len, &error));
    CHECK_INT(s.calls, 1); CHECK_STR(s.method, "GET"); CHECK_INT(s.sent, 0);
    CHECK_STR(s.url, "https://briareus.example/files/abc/shot.png?v=2");
    CHECK_STR(s.header_names, "Accept;");
    CHECK_INT(len, sizeof PNG - 1); CHECK(bytes && memcmp(bytes, PNG, sizeof PNG - 1) == 0); CHECK(error == NULL);
    free(bytes); stub_free(&s);
}

static void test_addresses_that_are_not_fetched_never_reach_the_network(void) {
    Stub s = { .status = 200, .body = PNG, .body_len = sizeof PNG - 1 };
    remote_image_set_transport(stub_transport, &s);
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(!remote_image_download("http://briareus.example/shot.png", &bytes, &len, &error));
    CHECK(error != NULL); free(error); error = NULL;
    CHECK(!remote_image_download("https://token@briareus.example/shot.png", &bytes, &len, &error));
    CHECK(error != NULL); free(error);
    CHECK_INT(s.calls, 0); CHECK(bytes == NULL); CHECK_INT(len, 0);
    stub_free(&s);
}

static void check_refused(Stub *s) {
    remote_image_set_transport(stub_transport, s);
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(!remote_image_download("https://briareus.example/shot.png", &bytes, &len, &error));
    CHECK_INT(s->calls, 1); CHECK(bytes == NULL); CHECK_INT(len, 0); CHECK(error && *error);
    free(error); stub_free(s);
}

static void test_redirects_errors_and_other_files_are_refused(void) {
    Stub redirect = { .status = 302, .body = PNG, .body_len = sizeof PNG - 1 }; check_refused(&redirect);
    Stub missing = { .status = 404, .body = PNG, .body_len = sizeof PNG - 1 }; check_refused(&missing);
    Stub partial = { .status = 206, .body = PNG, .body_len = sizeof PNG - 1 }; check_refused(&partial);
    Stub page = { .status = 200, .body = "<!doctype html><html>", .body_len = 21 }; check_refused(&page);
    Stub empty = { .status = 200, .body = "", .body_len = 0 }; check_refused(&empty);
    Stub offline = { .failure = "The server name could not be resolved." }; check_refused(&offline);
}

static void test_a_file_over_the_limit_is_refused(void) {
    size_t big = (size_t)REMOTE_IMAGE_MAX + 1;
    char *body = xcalloc(big, 1); memcpy(body, PNG, sizeof PNG - 1);
    Stub over = { .status = 200, .body = body, .body_len = big }; check_refused(&over);
    Stub s = { .status = 200, .body = body, .body_len = REMOTE_IMAGE_MAX };
    remote_image_set_transport(stub_transport, &s);
    char *bytes = NULL, *error = NULL; size_t len = 0;
    CHECK(remote_image_download("https://briareus.example/shot.png", &bytes, &len, &error));
    CHECK_INT(len, REMOTE_IMAGE_MAX); CHECK(error == NULL);
    free(bytes); free(body); stub_free(&s);
}

void remote_image_tests(void) {
    test_run("only HTTPS addresses without credentials are fetched", test_only_https_addresses_without_credentials_are_fetched);
    test_run("a picture link is told by its path", test_a_picture_link_is_told_by_its_path);
    test_run("names and hosts are read from the address", test_names_and_hosts_are_read_from_the_address);
    test_run("only the paired origin is its own", test_only_the_paired_origin_is_its_own);
    test_run("picture files are told by their first bytes", test_picture_files_are_told_by_their_first_bytes);
    test_run("a picture download is one GET without the token", test_a_download_is_one_get_without_the_token);
    test_run("addresses that are not fetched never reach the network", test_addresses_that_are_not_fetched_never_reach_the_network);
    test_run("redirects, errors and other files are refused", test_redirects_errors_and_other_files_are_refused);
    test_run("a file over the limit is refused", test_a_file_over_the_limit_is_refused);
}
