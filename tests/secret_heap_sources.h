// Compile the unchanged owning sources with a relocating, observing fixture allocator.
// No allocator hook is added to production code. See secret_heap.h for the observer.
#include "secret_heap.h"
#include "api.h"
#include "ssh_settings.h"
#define str_init heap_str_init
#define str_free heap_str_free
#define str_reserve heap_str_reserve
#define str_append heap_str_append
#define str_appendz heap_str_appendz
#define str_appendc heap_str_appendc
#define str_appendf heap_str_appendf
#define str_vappendf heap_str_vappendf
#define str_detach heap_str_detach
#define xmalloc heap_xmalloc
#define xcalloc heap_xcalloc
#define xrealloc heap_xrealloc
#define xmalloc_kept heap_xmalloc_kept
#define xfree_kept heap_xfree_kept
#define xstrdup heap_xstrdup
#define xstrndup heap_xstrndup
#define xstrfmt heap_xstrfmt
#define str_empty heap_str_empty
#define str_trim heap_str_trim
#define str_fold heap_str_fold
#define str_eq heap_str_eq
#define str_ieq heap_str_ieq
#define str_has_prefix heap_str_has_prefix
#define str_has_suffix heap_str_has_suffix
#define str_icontains heap_str_icontains
#define str_replace heap_str_replace
#define str_capitalized heap_str_capitalized
#define str_split heap_str_split
#define str_array_free heap_str_array_free
#define utf8_to_wide heap_utf8_to_wide
#define wide_to_utf8 heap_wide_to_utf8
void str_init(Str *s);
void str_free(Str *s);
void str_reserve(Str *s, size_t extra);
void str_append(Str *s, const char *bytes, size_t n);
void str_appendz(Str *s, const char *z);
void str_appendc(Str *s, char c);
void str_appendf(Str *s, const char *fmt, ...);
void str_vappendf(Str *s, const char *fmt, va_list ap);
char *str_detach(Str *s);
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
void *xmalloc_kept(size_t n);
void xfree_kept(void *p);
char *xstrdup(const char *z);
char *xstrndup(const char *z, size_t n);
char *xstrfmt(const char *fmt, ...);
bool str_empty(const char *z);
char *str_trim(const char *z);
char *str_fold(const char *z);
bool str_eq(const char *a, const char *b);
bool str_ieq(const char *a, const char *b);
bool str_has_prefix(const char *z, const char *prefix);
bool str_has_suffix(const char *z, const char *suffix);
bool str_icontains(const char *haystack, const char *needle);
char *str_replace(const char *z, const char *from, const char *to);
char *str_capitalized(const char *z);
char **str_split(const char *z, char sep, size_t *count);
void str_array_free(char **items, size_t count);
wchar_t *utf8_to_wide(const char *z);
char *wide_to_utf8(const wchar_t *w);
#define json_parse heap_json_parse
#define json_parsez heap_json_parsez
#define json_null heap_json_null
#define json_bool heap_json_bool
#define json_number heap_json_number
#define json_string heap_json_string
#define json_string_or_null heap_json_string_or_null
#define json_array heap_json_array
#define json_object heap_json_object
#define json_clone heap_json_clone
#define json_free heap_json_free
#define json_array_push heap_json_array_push
#define json_object_set heap_json_object_set
#define json_object_remove heap_json_object_remove
#define json_object_merge heap_json_object_merge
#define json_get heap_json_get
#define json_at heap_json_at
#define json_count heap_json_count
#define json_key heap_json_key
#define json_is_null heap_json_is_null
#define json_is_object heap_json_is_object
#define json_is_array heap_json_is_array
#define json_str heap_json_str
#define json_str_nonempty heap_json_str_nonempty
#define json_str_or heap_json_str_or
#define json_dup_str heap_json_dup_str
#define json_dup_strings heap_json_dup_strings
#define json_num heap_json_num
#define json_num_or heap_json_num_or
#define json_bool_is heap_json_bool_is
#define json_bool_tristate heap_json_bool_tristate
#define json_is_set heap_json_is_set
#define json_equal heap_json_equal
#define json_serialize heap_json_serialize
#define json_serialize_bounded heap_json_serialize_bounded
#define json_pretty heap_json_pretty
#define json_int_or heap_json_int_or
#define json_set_str heap_json_set_str
#define json_set_num heap_json_set_num
#define json_set_bool heap_json_set_bool
Json *json_parse(const char *text, size_t len);
Json *json_parsez(const char *text);
Json *json_null(void);
Json *json_bool(bool value);
Json *json_number(double value);
Json *json_string(const char *value);
Json *json_string_or_null(const char *value);
Json *json_array(void);
Json *json_object(void);
Json *json_clone(const Json *value);
void json_free(Json *value);
void json_array_push(Json *array, Json *value);
void json_object_set(Json *object, const char *key, Json *value);
void json_object_remove(Json *object, const char *key);
void json_object_merge(Json *into, const Json *from);
const Json *json_get(const Json *object, const char *key);
const Json *json_at(const Json *array, size_t index);
size_t json_count(const Json *array_or_object);
const char *json_key(const Json *object, size_t index);
bool json_is_null(const Json *value);
bool json_is_object(const Json *value);
bool json_is_array(const Json *value);
const char *json_str(const Json *value);
const char *json_str_nonempty(const Json *value);
const char *json_str_or(const Json *value, const char *fallback);
char *json_dup_str(const Json *value);
char **json_dup_strings(const Json *array, size_t *count);
bool json_num(const Json *value, double *out);
double json_num_or(const Json *value, double fallback);
bool json_bool_is(const Json *value, bool expected);
int json_bool_tristate(const Json *value);
bool json_is_set(const Json *value);
bool json_equal(const Json *a, const Json *b);
char *json_serialize(const Json *value, bool sorted);
char *json_serialize_bounded(const Json *value, bool sorted, size_t max_bytes);
char *json_pretty(const Json *value);
int json_int_or(const Json *value, int fallback);
void json_set_str(Json *object, const char *key, const char *value);
void json_set_num(Json *object, const char *key, double value);
void json_set_bool(Json *object, const char *key, bool value);
#define ssh_secrets_init heap_ssh_secrets_init
#define ssh_secrets_free heap_ssh_secrets_free
#define ssh_secrets_set heap_ssh_secrets_set
#define ssh_secrets_toggle heap_ssh_secrets_toggle
#define ssh_secrets_changed heap_ssh_secrets_changed
#define ssh_secrets_body heap_ssh_secrets_body
#define ssh_secrets_saved heap_ssh_secrets_saved
#define ssh_secrets_clone heap_ssh_secrets_clone
#define ssh_secret_free heap_ssh_secret_free
#define ssh_secrets_scrub heap_ssh_secrets_scrub
void ssh_secrets_init(SshSecrets *s, const Json *row, const Json *rows, bool fresh);
void ssh_secrets_free(SshSecrets *s);
void ssh_secrets_set(SshSecrets *s, int field, const char *text, size_t bytes);
void ssh_secrets_toggle(SshSecrets *s, int field);
bool ssh_secrets_changed(const SshSecrets *s);
bool ssh_secrets_body(const SshSecrets *s, Json *body, char **error);
bool ssh_secrets_saved(SshSecrets *s, const Json *row, const Json *sent, const unsigned long long revisions[SSH_SECRET_COUNT]);
void ssh_secrets_clone(SshSecrets *copy, const SshSecrets *s);
void ssh_secret_free(char *text, size_t bytes);
void ssh_secrets_scrub(Json *body);
#ifdef SECRET_HEAP_API
#define api_error_init heap_api_error_init
#define api_error_clear heap_api_error_clear
#define api_error_set heap_api_error_set
#define api_error_copy heap_api_error_copy
#define api_error_description heap_api_error_description
#define api_error_unauthorized heap_api_error_unauthorized
#define api_error_is_refusal heap_api_error_is_refusal
#define server_address_parse heap_server_address_parse
#define server_address_free heap_server_address_free
#define server_address_copy heap_server_address_copy
#define preview_access_applies heap_preview_access_applies
#define api_client_new heap_api_client_new
#define api_client_retain heap_api_client_retain
#define api_client_release heap_api_client_release
#define api_client_set_transport heap_api_client_set_transport
#define api_client_address heap_api_client_address
#define api_token_valid heap_api_token_valid
#define api_route heap_api_route
#define api_discovery heap_api_discovery
#define api_catalog heap_api_catalog
#define api_call heap_api_call
#define api_stream_cancel_init heap_api_stream_cancel_init
#define api_stream_cancel_free heap_api_stream_cancel_free
#define api_stream_cancel heap_api_stream_cancel
#define api_stream heap_api_stream
#define api_transcribe heap_api_transcribe
#define api_upload heap_api_upload
#define api_upload_cancellable heap_api_upload_cancellable
#define api_retry_after heap_api_retry_after
#define api_winhttp_transport heap_api_winhttp_transport
#define api_winhttp_error_text heap_api_winhttp_error_text
#define api_winhttp_header heap_api_winhttp_header
void api_error_init(ApiError *e);
void api_error_clear(ApiError *e);
void api_error_set(ApiError *e, ApiErrorKind kind, int status, const char *message, double retry_after);
void api_error_copy(ApiError *into, const ApiError *from);
char *api_error_description(const ApiError *e);
bool api_error_unauthorized(const ApiError *e);
bool api_error_is_refusal(const ApiError *e);
bool server_address_parse(const char *input, ServerAddress *out);
void server_address_free(ServerAddress *address);
void server_address_copy(ServerAddress *into, const ServerAddress *from);
bool preview_access_applies(const char *url, const char *host_suffix);
ApiClient *api_client_new(const ServerAddress *address, const char *token, ApiError *error);
ApiClient *api_client_retain(ApiClient *client);
void api_client_release(ApiClient *client);
void api_client_set_transport(ApiClient *client, ApiTransport transport, void *ctx);
const ServerAddress *api_client_address(const ApiClient *client);
bool api_token_valid(const char *token);
const ApiRoute *api_route(const char *name);
bool api_discovery(ApiClient *client, Discovery *out, ApiError *error);
bool api_catalog(ApiClient *client, Route **routes, size_t *count, ApiError *error);
Json *api_call(ApiClient *client, const char *name, const Json *arguments, int timeout_ms, ApiError *error);
void api_stream_cancel_init(ApiStreamCancel *cancel);
void api_stream_cancel_free(ApiStreamCancel *cancel);
void api_stream_cancel(ApiStreamCancel *cancel);
bool api_stream(ApiClient *client, const char *name, const Json *arguments, ApiStreamCancel *cancel, SseEmit emit, void *ctx, ApiError *error);
char *api_transcribe(ApiClient *client, const void *audio, size_t len, const char *content_type, ApiError *error);
char *api_upload(ApiClient *client, const char *name, const void *bytes, size_t len, ApiError *error);
char *api_upload_cancellable(ApiClient *client, const char *name, const void *bytes, size_t len, ApiStreamCancel *cancel, ApiError *error);
double api_retry_after(const char *value, time_t now);
bool api_winhttp_transport(void *ctx, const char *method, const char *url, const char *const *headers,
                           const void *body, size_t body_len, int timeout_ms,
                           int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                           char **error_message);
char *api_winhttp_error_text(DWORD code);
char *api_winhttp_header(void *request, DWORD info);
#endif
#define SSH_SECRET_KEYS heap_SSH_SECRET_KEYS
#define SSH_SECRET_FLAGS heap_SSH_SECRET_FLAGS
extern const char *const heap_SSH_SECRET_KEYS[SSH_SECRET_COUNT];
extern const char *const heap_SSH_SECRET_FLAGS[SSH_SECRET_COUNT];
#define malloc heap_malloc
#define calloc heap_calloc
#define realloc heap_realloc
#define free heap_free
#include "../core/str.c"
#include "../core/json.c"
#include "../core/ssh_settings.c"
#ifdef SECRET_HEAP_API
#include "../core/api.c"
#endif
#undef malloc
#undef calloc
#undef realloc
#undef free
