// WebView2 without its SDK: the few interface methods this app calls, by their slot in the vtable, and the completion
// handlers it implements. The slots follow WebView2.h (ICoreWebView2Environment, ICoreWebView2Controller, ICoreWebView2),
// whose published interfaces never change order.
#include "webview.h"
#include "api.h"
#include "str.h"
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef void (*AnyFn)(void);
/// Method `slot` of a COM object, as the function type `type`.
#define COM(obj, slot, type) ((type)((*(AnyFn **)(obj))[slot]))
#define COM_ADDREF(obj) COM(obj, 1, ULONG (WINAPI *)(void *))(obj)
#define COM_RELEASE(obj) COM(obj, 2, ULONG (WINAPI *)(void *))(obj)

// ICoreWebView2Environment
enum { ENV_CREATE_CONTROLLER = 3 };
// ICoreWebView2Controller
enum { CTRL_PUT_IS_VISIBLE = 4, CTRL_PUT_BOUNDS = 6, CTRL_CLOSE = 24, CTRL_GET_CORE = 25 };
// ICoreWebView2
enum { CORE_GET_SOURCE = 4, CORE_NAVIGATE = 5, CORE_ADD_SOURCE_CHANGED = 11, CORE_RELOAD = 31, CORE_ADD_TITLE_CHANGED = 46, CORE_GET_TITLE = 48,
       CORE_ADD_WEB_RESOURCE_REQUESTED = 55, CORE_ADD_WEB_RESOURCE_REQUESTED_FILTER = 57 };
// ICoreWebView2WebResourceRequestedEventArgs, ICoreWebView2WebResourceRequest, ICoreWebView2HttpRequestHeaders
enum { ARGS_GET_REQUEST = 3 };
enum { REQUEST_GET_URI = 3, REQUEST_GET_HEADERS = 9 };
enum { HEADERS_SET_HEADER = 6 };
// COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL
enum { RESOURCE_CONTEXT_ALL = 0 };

struct WebView {
    LONG refs; bool closed;
    HWND parent; wchar_t *url; RECT bounds; bool shown;
    void *controller, *core;    // ICoreWebView2Controller, ICoreWebView2
    char *error, *title, *source;
    char *access_suffix; wchar_t *access_id, *access_secret;   // the preview's service token, or NULL
    void (*changed)(void *ctx); void *ctx;
};

static void wv_release(WebView *wv) {
    if (--wv->refs) return;
    free(wv->url); free(wv->error); free(wv->title); free(wv->source);
    free(wv->access_suffix); free(wv->access_id); free(wv->access_secret);
    free(wv);
}
static void notify(WebView *wv) { if (!wv->closed && wv->changed) wv->changed(wv->ctx); }
static void fail(WebView *wv, const char *what, HRESULT hr) {
    if (wv->error) return;
    wv->error = xstrfmt("%s (0x%08lX).", what, (unsigned long)hr);
    notify(wv);
}

// MARK: - Handlers

/// A completion or event handler: every interface it answers to has QueryInterface, AddRef, Release and one Invoke.
typedef struct { const AnyFn *vtbl; LONG refs; WebView *wv; } Handler;

static HRESULT WINAPI handler_qi(Handler *h, REFIID iid, void **out) {
    (void)iid;
    if (!out) return E_POINTER;
    *out = h; h->refs++;
    return S_OK;
}
static ULONG WINAPI handler_add_ref(Handler *h) { return (ULONG)++h->refs; }
static ULONG WINAPI handler_release(Handler *h) {
    LONG n = --h->refs;
    if (!n) { wv_release(h->wv); free(h); }
    return (ULONG)n;
}
static Handler *handler_new(const AnyFn *vtbl, WebView *wv) {
    Handler *h = xcalloc(1, sizeof *h);
    h->vtbl = vtbl; h->refs = 1; h->wv = wv; wv->refs++;
    return h;
}

/// Reads a string property of the ICoreWebView2 into `*slot`.
static void read_string(WebView *wv, int slot_index, char **slot) {
    LPWSTR w = NULL;
    if (SUCCEEDED(COM(wv->core, slot_index, HRESULT (WINAPI *)(void *, LPWSTR *))(wv->core, &w)) && w) {
        free(*slot); *slot = wide_to_utf8(w);
        CoTaskMemFree(w);
    }
}
/// DocumentTitleChanged and SourceChanged: (sender, args).
static HRESULT WINAPI page_changed(Handler *h, void *sender, void *args) {
    (void)sender; (void)args;
    WebView *wv = h->wv;
    if (wv->closed || !wv->core) return S_OK;
    read_string(wv, CORE_GET_TITLE, &wv->title);
    read_string(wv, CORE_GET_SOURCE, &wv->source);
    notify(wv);
    return S_OK;
}
static const AnyFn page_changed_vtbl[] = { (AnyFn)handler_qi, (AnyFn)handler_add_ref, (AnyFn)handler_release, (AnyFn)page_changed };

/// WebResourceRequested: every request the page makes, the page itself included. One to a preview host carries the
/// Cloudflare Access service token, so Access lets it through without a sign-in.
static HRESULT WINAPI resource_requested(Handler *h, void *sender, void *args) {
    (void)sender;
    WebView *wv = h->wv;
    if (wv->closed || !wv->access_suffix || !args) return S_OK;
    void *request = NULL, *headers = NULL; LPWSTR uri = NULL;
    if (FAILED(COM(args, ARGS_GET_REQUEST, HRESULT (WINAPI *)(void *, void **))(args, &request)) || !request) return S_OK;
    if (SUCCEEDED(COM(request, REQUEST_GET_URI, HRESULT (WINAPI *)(void *, LPWSTR *))(request, &uri)) && uri) {
        char *url = wide_to_utf8(uri);
        if (preview_access_applies(url, wv->access_suffix)
            && SUCCEEDED(COM(request, REQUEST_GET_HEADERS, HRESULT (WINAPI *)(void *, void **))(request, &headers)) && headers) {
            typedef HRESULT (WINAPI *SetHeaderFn)(void *, LPCWSTR, LPCWSTR);
            COM(headers, HEADERS_SET_HEADER, SetHeaderFn)(headers, L"CF-Access-Client-Id", wv->access_id);
            COM(headers, HEADERS_SET_HEADER, SetHeaderFn)(headers, L"CF-Access-Client-Secret", wv->access_secret);
            COM_RELEASE(headers);
        }
        free(url);
        CoTaskMemFree(uri);
    }
    COM_RELEASE(request);
    return S_OK;
}
static const AnyFn resource_requested_vtbl[] = { (AnyFn)handler_qi, (AnyFn)handler_add_ref, (AnyFn)handler_release, (AnyFn)resource_requested };

static HRESULT WINAPI controller_created(Handler *h, HRESULT hr, void *controller) {
    WebView *wv = h->wv;
    if (FAILED(hr) || !controller) { fail(wv, "The browser could not open its window", hr); return S_OK; }
    if (wv->closed) { COM(controller, CTRL_CLOSE, HRESULT (WINAPI *)(void *))(controller); return S_OK; }
    COM_ADDREF(controller);
    wv->controller = controller;
    void *core = NULL;
    if (FAILED(hr = COM(controller, CTRL_GET_CORE, HRESULT (WINAPI *)(void *, void **))(controller, &core)) || !core) {
        fail(wv, "The browser did not start", hr);
        return S_OK;
    }
    wv->core = core;
    INT64 token;
    Handler *ev = handler_new(page_changed_vtbl, wv);
    COM(core, CORE_ADD_TITLE_CHANGED, HRESULT (WINAPI *)(void *, void *, INT64 *))(core, ev, &token);
    COM(core, CORE_ADD_SOURCE_CHANGED, HRESULT (WINAPI *)(void *, void *, INT64 *))(core, ev, &token);
    handler_release(ev);
    if (wv->access_suffix) {
        Handler *rr = handler_new(resource_requested_vtbl, wv);
        COM(core, CORE_ADD_WEB_RESOURCE_REQUESTED, HRESULT (WINAPI *)(void *, void *, INT64 *))(core, rr, &token);
        handler_release(rr);
        COM(core, CORE_ADD_WEB_RESOURCE_REQUESTED_FILTER, HRESULT (WINAPI *)(void *, LPCWSTR, int))(core, L"*", RESOURCE_CONTEXT_ALL);
    }
    COM(controller, CTRL_PUT_BOUNDS, HRESULT (WINAPI *)(void *, RECT))(controller, wv->bounds);
    COM(controller, CTRL_PUT_IS_VISIBLE, HRESULT (WINAPI *)(void *, BOOL))(controller, wv->shown);
    if (FAILED(hr = COM(core, CORE_NAVIGATE, HRESULT (WINAPI *)(void *, LPCWSTR))(core, wv->url))) fail(wv, "The browser would not open the address", hr);
    notify(wv);
    return S_OK;
}
static const AnyFn controller_created_vtbl[] = { (AnyFn)handler_qi, (AnyFn)handler_add_ref, (AnyFn)handler_release, (AnyFn)controller_created };

static HRESULT WINAPI environment_created(Handler *h, HRESULT hr, void *env) {
    WebView *wv = h->wv;
    if (wv->closed) return S_OK;
    if (FAILED(hr) || !env) { fail(wv, "The browser runtime did not start", hr); return S_OK; }
    Handler *next = handler_new(controller_created_vtbl, wv);
    hr = COM(env, ENV_CREATE_CONTROLLER, HRESULT (WINAPI *)(void *, HWND, void *))(env, wv->parent, next);
    handler_release(next);
    if (FAILED(hr)) fail(wv, "The browser could not open its window", hr);
    return S_OK;
}
static const AnyFn environment_created_vtbl[] = { (AnyFn)handler_qi, (AnyFn)handler_add_ref, (AnyFn)handler_release, (AnyFn)environment_created };

// MARK: - Runtime

/// EmbeddedBrowserWebView.dll's entry point, which WebView2Loader.dll's CreateCoreWebView2EnvironmentWithOptions calls:
/// (check for a running browser, runtime kind 0 = installed, user data folder, options, completion handler).
typedef HRESULT (WINAPI *CreateEnvironmentFn)(bool, int, PCWSTR, void *, void *);

/// The Evergreen runtime's client DLL, from the stable channel's EdgeUpdate key, machine-wide first.
static CreateEnvironmentFn runtime_entry(void) {
    static bool tried; static CreateEnvironmentFn fn;
    if (tried) return fn;
    tried = true;
    static const wchar_t key[] = L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2 && !fn; i++) {
        wchar_t dir[MAX_PATH]; DWORD size = sizeof dir;
        if (RegGetValueW(roots[i], key, L"EBWebView", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, NULL, dir, &size) != ERROR_SUCCESS) continue;
#if defined(_M_ARM64) || defined(__aarch64__)
        const wchar_t *arch = L"arm64";
#elif defined(_M_X64) || defined(__x86_64__)
        const wchar_t *arch = L"x64";
#else
        const wchar_t *arch = L"x86";
#endif
        wchar_t dll[MAX_PATH + 64];
        size_t n = wcslen(dir);
        swprintf(dll, MAX_PATH + 64, L"%ls%ls\\EBWebView\\%ls\\EmbeddedBrowserWebView.dll", dir, n && dir[n - 1] == L'\\' ? L"" : L"\\", arch);
        HMODULE module = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) fn = (CreateEnvironmentFn)(AnyFn)GetProcAddress(module, "CreateWebViewEnvironmentWithOptionsInternal");
    }
    return fn;
}
bool webview_available(void) { return runtime_entry() != NULL; }

// MARK: - API

WebView *webview_new(HWND parent, const char *url, const WebViewAccess *access, void (*changed)(void *ctx), void *ctx) {
    WebView *wv = xcalloc(1, sizeof *wv);
    wv->refs = 1; wv->parent = parent; wv->url = utf8_to_wide(url); wv->changed = changed; wv->ctx = ctx;
    if (access && access->client_id && access->client_secret && access->host_suffix && *access->host_suffix) {
        wv->access_suffix = xstrdup(access->host_suffix);
        wv->access_id = utf8_to_wide(access->client_id); wv->access_secret = utf8_to_wide(access->client_secret);
    }
    CreateEnvironmentFn create = runtime_entry();
    if (!create) { wv->error = xstrdup("The Microsoft Edge WebView2 Runtime is not installed."); return wv; }
    // Its own profile, so a Cloudflare Access sign-in, where there is no service token, lasts between runs.
    wchar_t *base = NULL, folder[MAX_PATH];
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &base) != S_OK) base = NULL;
    swprintf(folder, MAX_PATH, L"%ls\\Okanet\\Briareus\\WebView2", base ? base : L".");
    if (base) CoTaskMemFree(base);
    Handler *h = handler_new(environment_created_vtbl, wv);
    HRESULT hr = create(true, 0, folder, NULL, h);
    handler_release(h);
    if (FAILED(hr)) fail(wv, "The browser runtime did not start", hr);
    return wv;
}
void webview_free(WebView *wv) {
    if (!wv) return;
    wv->closed = true; wv->changed = NULL;
    if (wv->controller) COM(wv->controller, CTRL_CLOSE, HRESULT (WINAPI *)(void *))(wv->controller);
    if (wv->core) { COM_RELEASE(wv->core); wv->core = NULL; }
    if (wv->controller) { COM_RELEASE(wv->controller); wv->controller = NULL; }
    wv_release(wv);
}
void webview_set_bounds(WebView *wv, const RECT *rc) {
    if (EqualRect(&wv->bounds, rc)) return;
    wv->bounds = *rc;
    if (wv->controller) COM(wv->controller, CTRL_PUT_BOUNDS, HRESULT (WINAPI *)(void *, RECT))(wv->controller, wv->bounds);
}
void webview_show(WebView *wv, bool shown) {
    wv->shown = shown;
    if (wv->controller) COM(wv->controller, CTRL_PUT_IS_VISIBLE, HRESULT (WINAPI *)(void *, BOOL))(wv->controller, shown);
}
void webview_reload(WebView *wv) { if (wv->core) COM(wv->core, CORE_RELOAD, HRESULT (WINAPI *)(void *))(wv->core); }
bool webview_ready(WebView *wv) { return wv->core && !wv->error; }
const char *webview_error(WebView *wv) { return wv->error; }
const char *webview_title(WebView *wv) { return wv->title; }
const char *webview_url(WebView *wv) { return wv->source; }
