// The client API (/api/v1) over HTTPS: one token, no cookies, no cache, no redirects, no automatic retries.
#ifndef BRIAREUS_API_H
#define BRIAREUS_API_H
#include "browser.h"
#include "json.h"
#include "models.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <windows.h>

typedef enum {
    API_OK = 0,
    API_INVALID_ADDRESS, API_INVALID_TOKEN, API_REDIRECTED, API_NON_JSON, API_INCOMPATIBLE_VERSION, API_OVERSIZED_REQUEST,
    API_HTTP,       // status and message set
    API_NETWORK,    // could not reach or finish talking to the server; message says why
    API_CANCELLED,  // the screen that asked went away
} ApiErrorKind;

typedef struct {
    ApiErrorKind kind;
    int status;           // for API_HTTP
    char *message;        // server's `error` or the transport's words
    double retry_after;   // seconds; < 0 without a Retry-After header
} ApiError;

void api_error_init(ApiError *e);
void api_error_clear(ApiError *e);
void api_error_set(ApiError *e, ApiErrorKind kind, int status, const char *message, double retry_after);
void api_error_copy(ApiError *into, const ApiError *from);
/// What the user is told, as a new string.
char *api_error_description(const ApiError *e);
bool api_error_unauthorized(const ApiError *e);
/// A 4xx is a definite refusal; anything else may have gone through.
bool api_error_is_refusal(const ApiError *e);

/// An HTTPS origin and the client API base beneath it.
typedef struct { char *base_url; char *origin; char *host; int port; } ServerAddress;
bool server_address_parse(const char *input, ServerAddress *out);
void server_address_free(ServerAddress *address);
void server_address_copy(ServerAddress *into, const ServerAddress *from);
/// Whether a ▶ Run preview's Cloudflare Access service token (`GET /preview/access`) may go with a request to `url`: only
/// over HTTPS, to a host ending in `.` + `host_suffix`, so the secret never reaches any other site.
bool preview_access_applies(const char *url, const char *host_suffix);

/// One round trip. Headers come as NULL-terminated name/value pairs. Returns false only when nothing was received;
/// then `*error_message` says why. Every output string is malloc'd.
typedef bool (*ApiTransport)(void *ctx, const char *method, const char *url, const char *const *headers,
                             const void *body, size_t body_len, int timeout_ms,
                             int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                             char **error_message);

typedef struct ApiClient ApiClient;
/// Fails with API_INVALID_TOKEN unless the token has the token shape.
ApiClient *api_client_new(const ServerAddress *address, const char *token, ApiError *error);
/// The client is shared by the screens and the threads finishing their requests; the last release frees it.
ApiClient *api_client_retain(ApiClient *client);
void api_client_release(ApiClient *client);
/// Tests swap the network for a stub. The default is WinHTTP.
void api_client_set_transport(ApiClient *client, ApiTransport transport, void *ctx);
const ServerAddress *api_client_address(const ApiClient *client);
bool api_token_valid(const char *token);

/// One of the calls this app makes, and the route that answers it. A `{name}` in `path` is filled with the argument of
/// that name, URL-encoded; the other arguments go in the query of a GET or DELETE and in the JSON body otherwise. `set`
/// names a body flag the call always sends as true. `filter` names an argument the route has no parameter for: it is
/// kept back, and the answer's `list` is cut to the rows whose field of that name equals it.
typedef struct { const char *name, *method, *path, *set, *filter, *list; } ApiRoute;
/// The table entry for a call name, or NULL for a name this app does not make.
const ApiRoute *api_route(const char *name);

/// `GET /`: the token's own record and what the server can do.
bool api_discovery(ApiClient *client, Discovery *out, ApiError *error);
/// `GET /openapi.json`, read into the routes the server has and who may call each.
bool api_catalog(ApiClient *client, Route **routes, size_t *count, ApiError *error);
/// One call by name, on its route. `timeout_ms` is for a call that answers only once its work is done; 0 leaves the
/// 30-second default. A name not in the table, or a missing path argument, is refused with a 400 before any network call.
Json *api_call(ApiClient *client, const char *name, const Json *arguments, int timeout_ms, ApiError *error);
/// Stops a stream from another thread: the read under way ends at once with API_CANCELLED, and one not yet started never
/// starts. Initialised before the stream and freed once it returned.
typedef struct { CRITICAL_SECTION lock; void *request; bool cancelled; } ApiStreamCancel;
void api_stream_cancel_init(ApiStreamCancel *cancel);
void api_stream_cancel_free(ApiStreamCancel *cancel);
void api_stream_cancel(ApiStreamCancel *cancel);
/// Reads a GET call's server-sent event stream on the calling thread, handing each event to `emit` as it completes, until
/// the server ends it (true), or it fails or is cancelled (false, with `error`). Always over WinHTTP, never the test stub.
bool api_stream(ApiClient *client, const char *name, const Json *arguments, ApiStreamCancel *cancel, SseEmit emit, void *ctx, ApiError *error);
/// The text of a recorded voice note, sent with the content type it was recorded in.
char *api_transcribe(ApiClient *client, const void *audio, size_t len, const char *content_type, ApiError *error);
/// Stores a file to attach to a message, as the dashboard's composer does: the bytes are the body and the name rides in the
/// query. The answer is the id a message takes in `attachments`. Files larger than API_UPLOAD_LIMIT are refused here.
#define API_UPLOAD_LIMIT (25 * 1024 * 1024)
char *api_upload(ApiClient *client, const char *name, const void *bytes, size_t len, ApiError *error);

/// Seconds to wait from a Retry-After header, or -1 when it cannot be read.
double api_retry_after(const char *value, time_t now);

/// The WinHTTP transport, exposed so the app can reuse it for nothing else than tests of its own.
bool api_winhttp_transport(void *ctx, const char *method, const char *url, const char *const *headers,
                           const void *body, size_t body_len, int timeout_ms,
                           int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                           char **error_message);

#endif
