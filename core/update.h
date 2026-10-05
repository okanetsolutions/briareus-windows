// Updates from the repository's GitHub releases: the latest one is read and compared with this build, and its Briareus.exe
// is downloaded, matched against the size and SHA-256 GitHub records for it, and moved in place of the running executable.
#ifndef BRIAREUS_UPDATE_H
#define BRIAREUS_UPDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define UPDATE_LATEST_URL "https://api.github.com/repos/okanetsolutions/briareus-windows/releases/latest"
#define UPDATE_ASSET "Briareus.exe"

/// A release: its version without the `v`, its page, and the executable's address, size and SHA-256 in lower-case hex.
typedef struct { char *version, *page, *url, *sha256; size_t size; } UpdateRelease;
void update_release_free(UpdateRelease *release);
/// Reads GitHub's release JSON. False without a version, or without a Briareus.exe asset that has a size and a SHA-256.
bool update_release_parse(const char *json, size_t len, UpdateRelease *out);

/// Whether `candidate` ("v1.85.0" or "1.85.0") is a later version than `current`. False when either cannot be read.
bool update_newer(const char *candidate, const char *current);
/// Whether an update request may go to `url`, or be redirected there: HTTPS to github.com, api.github.com or a host
/// under githubusercontent.com, where release downloads are served, on the default port.
bool update_url_allowed(const char *url);
/// The SHA-256 of `data` as 64 lower-case hex digits and a NUL.
bool update_sha256(const void *data, size_t len, char hex[65]);

/// One GET without following redirects; `*location` is the Location header of a redirect. Every output is malloc'd. False
/// only when nothing was received, with `*error` saying why.
typedef bool (*UpdateFetch)(void *ctx, const char *url, const char *accept, int *status, char **location, char **body,
                            size_t *len, char **error);
bool update_winhttp_fetch(void *ctx, const char *url, const char *accept, int *status, char **location, char **body,
                          size_t *len, char **error);
/// Tests swap the network for a stub; NULL puts WinHTTP back.
void update_set_fetch(UpdateFetch fetch, void *ctx);

/// Reads the latest release into `*out`, whether or not it is newer than this build.
bool update_check(UpdateRelease *out, char **error);
/// The release's executable, through at most five redirects that all stay on the allowed hosts, checked against its size
/// and SHA-256 and for an executable's `MZ` start.
bool update_download(const UpdateRelease *release, char **bytes, size_t *len, char **error);
/// Puts `bytes` in place of `exe`: written beside it as `.new`, the current file moved aside to `.old` (a running
/// executable may be renamed, not overwritten), then the new one moved in. Puts the old one back when that last step fails.
bool update_install(const wchar_t *exe, const void *bytes, size_t len, char **error);
/// Deletes the `.old` a previous update left beside `exe`. True when none is left.
bool update_cleanup(const wchar_t *exe);

#endif
