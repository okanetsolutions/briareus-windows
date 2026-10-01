// The mobile API over HTTPS: one device token, no cookies, no cache, no redirects, no automatic retries.
#ifndef BRIAREUS_API_H
#define BRIAREUS_API_H
#include "json.h"
#include "models.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

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

/// An HTTPS origin and the mobile API base beneath it.
typedef struct { char *base_url; char *origin; char *host; int port; } ServerAddress;
bool server_address_parse(const char *input, ServerAddress *out);
void server_address_free(ServerAddress *address);
void server_address_copy(ServerAddress *into, const ServerAddress *from);

/// One round trip. Headers come as NULL-terminated name/value pairs. Returns false only when nothing was received;
/// then `*error_message` says why. Every output string is malloc'd.
typedef bool (*ApiTransport)(void *ctx, const char *method, const char *url, const char *const *headers,
                             const void *body, size_t body_len, int timeout_ms,
                             int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                             char **error_message);

typedef struct ApiClient ApiClient;
/// Fails with API_INVALID_TOKEN unless the token has the device token shape.
ApiClient *api_client_new(const ServerAddress *address, const char *token, ApiError *error);
/// The client is shared by the screens and the threads finishing their requests; the last release frees it.
ApiClient *api_client_retain(ApiClient *client);
void api_client_release(ApiClient *client);
/// Tests swap the network for a stub. The default is WinHTTP.
void api_client_set_transport(ApiClient *client, ApiTransport transport, void *ctx);
const ServerAddress *api_client_address(const ApiClient *client);
bool api_token_valid(const char *token);

bool api_discovery(ApiClient *client, Discovery *out, ApiError *error);
bool api_operations(ApiClient *client, Operation **ops, size_t *count, ApiError *error);
bool api_revoke(ApiClient *client, ApiError *error);
/// `timeout_ms` is for an operation that answers only once its work is done; 0 leaves the 30-second default.
Json *api_operation(ApiClient *client, const char *name, const Json *arguments, int timeout_ms, ApiError *error);
/// The text of a recorded voice note. `language` is the spoken one as a BCP 47 tag; NULL or "" lets the server detect it.
char *api_transcribe(ApiClient *client, const void *audio, size_t len, const char *content_type, const char *language, ApiError *error);
/// Stores a file to attach to a message, as the dashboard's composer does: the bytes are the body and the name rides in the
/// query. The answer is the id the `message` operation takes in `attachments`. Files larger than API_UPLOAD_LIMIT are refused here.
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
