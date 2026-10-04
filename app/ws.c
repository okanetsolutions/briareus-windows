#include "ws.h"
#include "json.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

struct WebSocket {
    HINTERNET session, connection, socket;
    SRWLOCK send_lock;   // WinHTTP takes one send at a time
    volatile LONG aborted;
};

static char *last_error(const char *what) {
    DWORD code = GetLastError();
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT) return xstrfmt("%s: could not reach the server (%lu).", what, (unsigned long)code);
    if (code == ERROR_WINHTTP_TIMEOUT) return xstrfmt("%s: the server did not answer in time.", what);
    return xstrfmt("%s failed (%lu).", what, (unsigned long)code);
}

/// The refusal's body, read for the server's `error.message` when it sent JSON.
static char *refusal(HINTERNET request, DWORD status) {
    Str body; str_init(&body);
    char buffer[4096]; DWORD n = 0;
    while (body.len < 65536 && WinHttpReadData(request, buffer, sizeof buffer, &n) && n) str_append(&body, buffer, n);
    Json *j = body.len ? json_parse(body.data, body.len) : NULL;
    // OpenAI says why in error.message, ElevenLabs in detail.message or detail.
    const Json *detail = json_get(j, "detail");
    const char *message = json_str_nonempty(json_get(json_get(j, "error"), "message"));
    if (!message) message = json_str_nonempty(json_get(detail, "message"));
    if (!message) message = json_str_nonempty(detail);
    char *text = message ? xstrfmt("HTTP %lu: %s", (unsigned long)status, message)
                         : status == 401 ? xstrdup("HTTP 401: the API key was refused.")
                         : xstrfmt("HTTP %lu: the server refused the connection.", (unsigned long)status);
    json_free(j); str_free(&body);
    return text;
}

WebSocket *ws_connect(const char *host, const char *path, const char *const *headers, char **error) {
    *error = NULL;
    WebSocket *ws = xcalloc(1, sizeof *ws);
    InitializeSRWLock(&ws->send_lock);
    HINTERNET request = NULL;
    wchar_t *whost = utf8_to_wide(host), *wpath = utf8_to_wide(path);
    ws->session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ws->session) ws->session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ws->session) { *error = last_error("Opening the connection"); goto fail; }
    WinHttpSetTimeouts(ws->session, 15000, 15000, 15000, 30000);
    ws->connection = WinHttpConnect(ws->session, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!ws->connection) { *error = last_error("Connecting"); goto fail; }
    request = WinHttpOpenRequest(ws->connection, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) { *error = last_error("Preparing the request"); goto fail; }
    Str h; str_init(&h);
    for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) str_appendf(&h, "%s: %s\r\n", headers[i], headers[i + 1]);
    wchar_t *wh = utf8_to_wide(h.len ? h.data : "");
    str_free(&h);
    BOOL sent = WinHttpSendRequest(request, wh, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, NULL);
    SecureZeroMemory(wh, wcslen(wh) * sizeof *wh);
    free(wh);
    if (!sent) { *error = last_error("Connecting"); goto fail; }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) { *error = refusal(request, status); goto fail; }
    ws->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!ws->socket) { *error = last_error("Opening the WebSocket"); goto fail; }
    WinHttpCloseHandle(request);
    free(whost); free(wpath);
    return ws;
fail:
    if (request) WinHttpCloseHandle(request);
    free(whost); free(wpath);
    ws_free(ws);
    return NULL;
}

bool ws_send(WebSocket *ws, const char *text) {
    if (!ws || ws->aborted) return false;
    AcquireSRWLockExclusive(&ws->send_lock);
    DWORD rc = ws->socket ? WinHttpWebSocketSend(ws->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text, (DWORD)strlen(text)) : ERROR_INVALID_HANDLE;
    ReleaseSRWLockExclusive(&ws->send_lock);
    return rc == NO_ERROR;
}

int ws_receive(WebSocket *ws, char **text, size_t *len) {
    *text = NULL; *len = 0;
    Str message; str_init(&message);
    char buffer[16384];
    for (;;) {
        DWORD n = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        HINTERNET socket = ws->socket;
        if (!socket) { str_free(&message); return -1; }
        DWORD rc = WinHttpWebSocketReceive(socket, buffer, sizeof buffer, &n, &type);
        if (rc != NO_ERROR || ws->aborted) { str_free(&message); return -1; }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { str_free(&message); return 0; }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) message.len = 0;
            continue;
        }
        str_append(&message, buffer, n);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
    }
    *len = message.len;
    *text = str_detach(&message);
    return 1;
}

void ws_abort(WebSocket *ws) {
    if (!ws) return;
    if (InterlockedExchange(&ws->aborted, 1)) return;
    // Closing the handle cancels the receive under way; sends see it gone.
    AcquireSRWLockExclusive(&ws->send_lock);
    HINTERNET socket = ws->socket;
    ws->socket = NULL;
    ReleaseSRWLockExclusive(&ws->send_lock);
    if (socket) WinHttpCloseHandle(socket);
}

void ws_free(WebSocket *ws) {
    if (!ws) return;
    ws_abort(ws);
    if (ws->connection) WinHttpCloseHandle(ws->connection);
    if (ws->session) WinHttpCloseHandle(ws->session);
    free(ws);
}
