// A WebSocket client over WinHTTP's own WebSocket support (Windows 8 and later): text messages both ways, sends from any
// thread, receives on one.
#ifndef BRIAREUS_WS_H
#define BRIAREUS_WS_H
#include <stdbool.h>
#include <stddef.h>

typedef struct WebSocket WebSocket;

/// Opens wss://host:443/path with the headers (NULL-terminated name/value pairs). NULL with `*error` set when the
/// server cannot be reached or refuses the upgrade; its own words are in the error when it gave any.
WebSocket *ws_connect(const char *host, const char *path, const char *const *headers, char **error);
/// Sends one text message; false once the socket is closed or broken. Safe from any thread.
bool ws_send(WebSocket *ws, const char *text);
/// Waits for the next whole text message (binary ones are skipped). 1 with `*text` set, 0 when the server closed the
/// socket, -1 when it broke or was aborted.
int ws_receive(WebSocket *ws, char **text, size_t *len);
/// Ends a ws_receive waiting on another thread, which then returns -1.
void ws_abort(WebSocket *ws);
/// Closes and frees; no receive may be running.
void ws_free(WebSocket *ws);

#endif
