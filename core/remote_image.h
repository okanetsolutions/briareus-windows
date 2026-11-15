// Pictures an agent's reply links to, fetched for the conversation: one GET over HTTPS with no token, no cookies and no
// redirects, checked to be a picture before anything decodes it.
#ifndef BRIAREUS_REMOTE_IMAGE_H
#define BRIAREUS_REMOTE_IMAGE_H
#include "api.h"
#include <stdbool.h>
#include <stddef.h>

/// The largest file taken as a picture.
#define REMOTE_IMAGE_MAX (32u * 1024 * 1024)

/// Whether a picture may be fetched from `url`: HTTPS, with a host and no credentials in it.
bool remote_image_address(const char *url);
/// The same, for a link whose path ends in a picture file's extension (.png, .jpg, .jpeg, .gif, .bmp, .webp), whatever
/// its query: what tells a link to a picture from any other link.
bool remote_image_url(const char *url);
/// The file name at the end of the address's path, percent-decoded, or its host without one; a new string.
char *remote_image_name(const char *url);
/// The address's host, lower-cased, with its port when that is not 443; a new string, NULL for an address not fetched.
char *remote_image_host(const char *url);
/// Whether the picture is served by `origin` itself ("https://host" or "https://host:port", as a ServerAddress has it):
/// the paired server, where published session files are.
bool remote_image_from_origin(const char *url, const char *origin);
/// Whether the bytes start as a PNG, JPEG, GIF, BMP or WebP file does.
bool remote_image_recognized(const void *bytes, size_t len);

/// Tests swap the network for a stub; NULL puts WinHTTP back.
void remote_image_set_transport(ApiTransport transport, void *ctx);
/// Fetches the picture, without its fragment. False with `*error`, a new string, when the address is not one to fetch,
/// the server answers anything but 200 (a redirect is not followed), or the body is empty, larger than REMOTE_IMAGE_MAX
/// or not a picture. `*bytes` is malloc'd.
bool remote_image_download(const char *url, char **bytes, size_t *len, char **error);

#endif
