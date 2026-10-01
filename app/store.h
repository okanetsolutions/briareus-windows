// The connection: one token, the server's route catalog, the saved-response cache, and requests answered on the UI thread.
#ifndef BRIAREUS_STORE_H
#define BRIAREUS_STORE_H
#include "api.h"
#include "cache.h"
#include "models.h"
#include <windows.h>
#include <stdbool.h>

#define WM_APP_STORE_CHANGED (WM_APP + 1)
#define WM_APP_REQUEST_DONE  (WM_APP + 2)
#define WM_APP_ASYNC_DONE    (WM_APP + 3)

typedef struct {
    ApiClient *client;                 // NULL until paired
    bool has_device; Device device;
    Route *routes; size_t route_count;   // what the server lists in its OpenAPI document
    int transcribes;                   // -1 from a server that predates voice notes
    bool connecting;
    char *connection_error;
    char *server;                      // the origin shown in the pairing field
    DiskCache *cache;
    HWND hwnd;                         // the main window, told about changes
    bool active;                       // the app is in the foreground and not minimized
} Store;

extern Store g_store;

void store_init(HWND main_window);
void store_shutdown(void);
bool store_connected(void);
bool store_can_manage(void);
/// The microphone shows for any device allowed to write the message a note becomes.
bool store_can_transcribe(void);
/// Whether the server has the route a call is made on and this token may call it.
bool store_supports(const char *call);
/// Files can go with a message: the server takes uploads and messages, and this token may write.
bool store_supports_attachments(void);
/// The same for the call the files go with: `message`, or `start_session` for a session's first prompt.
bool store_supports_attachments_on(const char *call);
/// A device that paired before opens on what it saved; the server confirms the token meanwhile.
void store_restore(void);
void store_connect(const char *server, const char *token);
/// Removes local credentials and saved conversations only.
void store_forget(void);
/// Asks the server again before refusing voice notes; `done` gets NULL when they work, else what is missing.
void store_voice_notes_off(void (*done)(void *ctx, const char *reason), void *ctx);
/// Handles a 401 from any request: the token was revoked or expired.
void store_invalidate_credentials(const ApiError *error);

typedef struct Request Request;
typedef void (*RequestDone)(void *owner, Request *req);
struct Request {
    char *operation;                                   // the call's name, as api_route knows it
    Json *args;
    int timeout_ms;
    void *audio; size_t audio_len; char *audio_type;   // for a transcription
    void *file; size_t file_len; char *file_name;      // for an upload
    Json *result; char *text;                          // the answer: a transcription's text, or an upload's id
    ApiError error;
    bool ok;
    void *owner; RequestDone done; int tag; intptr_t arg;
    Request **slot;                                    // cleared when the request finishes or is cancelled
    bool cancelled;
    ApiClient *client;
};

/// Makes a call (api_call) on a worker thread; `done` is called on the UI thread unless the request is cancelled first.
/// A token that cannot make the call gets a 403 without a network call. Takes ownership of `args`.
Request *store_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone done, int tag, Request **slot);
Request *store_transcribe(const void *audio, size_t len, const char *content_type, void *owner, RequestDone done, int tag, Request **slot);
/// Stores a file on the server for the next message; the answer's `text` is the id to send. Takes ownership of `bytes`.
Request *store_upload(const char *name, void *bytes, size_t len, void *owner, RequestDone done, int tag, Request **slot);
/// Drops the request in the slot: its answer is discarded, and the slot is cleared.
void request_cancel(Request **slot);
/// The user-facing text of a failed request.
char *request_error_text(const Request *req);
/// Replaces the string in `slot` with the request's error text.
void request_error_into(char **slot, const Request *req);
/// The error text, or "unexpected response" for a request that succeeded with an answer the caller could not read.
char *request_error_or_unexpected(const Request *req);

/// Any work on a thread with a UI-thread completion.
typedef void (*AsyncWork)(void *ctx);
void async_run(AsyncWork work, AsyncWork done, void *ctx);

/// Called by the main window for WM_APP_REQUEST_DONE and WM_APP_ASYNC_DONE.
void store_handle_message(UINT msg, WPARAM wp, LPARAM lp);

/// The delay before the next poll after `failures` consecutive failures: exponential up to a minute, or the server's Retry-After.
int poll_delay_ms(int base_ms, int failures, double retry_after_seconds);

#endif
