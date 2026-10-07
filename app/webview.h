// An embedded browser: Microsoft Edge WebView2, driven through its COM interfaces without the SDK or WebView2Loader.dll.
// The installed Evergreen runtime is found in the registry and its EmbeddedBrowserWebView.dll is loaded directly.
#ifndef BRIAREUS_WEBVIEW_H
#define BRIAREUS_WEBVIEW_H
#include <windows.h>
#include <stdbool.h>

typedef struct WebView WebView;

/// The Cloudflare Access service token (`GET /preview/access`): sent as CF-Access-Client-Id and CF-Access-Client-Secret
/// with every request to a host ending in `.` + `host_suffix`, and to no other.
typedef struct { const char *client_id, *client_secret, *host_suffix; } WebViewAccess;

/// Whether the WebView2 runtime is installed.
bool webview_available(void);
/// Starts a browser as a child of `parent` that opens `url` once it is ready. `changed` is called on the UI thread whenever
/// the page's title, address or the browser's state changes. `access` may be NULL; it is copied. Never NULL; a failure
/// shows in `webview_error`.
WebView *webview_new(HWND parent, const char *url, const WebViewAccess *access, void (*changed)(void *ctx), void *ctx);
/// Closes the browser; callbacks still in flight are dropped.
void webview_free(WebView *wv);
/// Where the browser sits, in `parent`'s client coordinates.
void webview_set_bounds(WebView *wv, const RECT *rc);
void webview_show(WebView *wv, bool shown);
/// Moves the browser into another window, as a page moves into a window of its own; its bounds are set again after.
void webview_set_parent(WebView *wv, HWND parent);
void webview_reload(WebView *wv);
/// Goes back or forward in the browser's history, as its buttons do; nothing when there is nowhere to go.
void webview_back(WebView *wv);
void webview_forward(WebView *wv);
bool webview_can_back(WebView *wv);
bool webview_can_forward(WebView *wv);
/// True once the page is up; false while the browser starts or after it failed.
bool webview_ready(WebView *wv);
/// Why the browser could not start, or NULL.
const char *webview_error(WebView *wv);
/// The page's title and address as the browser reports them; NULL until it has.
const char *webview_title(WebView *wv);
const char *webview_url(WebView *wv);

#endif
