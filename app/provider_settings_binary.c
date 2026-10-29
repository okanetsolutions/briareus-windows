#include "screen_provider_settings_internal.h"

// Fallback IDs and known auth behavior, verified against nadinyamaui/briareus main:
// https://github.com/nadinyamaui/briareus/blob/70e3d0c0793fa0f7eec21c8cc194ace0d3150920/lib/providers.js
// BINARIES, probeProviderAuth, testProviderEndpoint; providerstore.js PROVIDER_DEFAULTS.
// Prefer OpenAPI components.schemas.Provider.properties.binary.enum for supported IDs.
// Empty model/effort fields defer to the backend's current defaults.
const screen_provider_settings_BINARIES_type screen_provider_settings_BINARIES[] = {
    { "claude", "claude (Claude Code)", BINARY_CHOICE, NULL },
    { "codex", "codex (Codex)", BINARY_CHOICE, NULL },
    { "grok", "grok (Grok)", BINARY_LOGIN, "grok signs in with its own login; it takes no API token." },
    { "opencode", "opencode", BINARY_KEY, "opencode authenticates with an API key per service and has no login of its own." },
    { "dsh", "dsh (DeepSeek Harness)", BINARY_KEY, "DeepSeek Harness uses an API key and has no provider login." },
    { "qwen", "qwen (Qwen Code)", BINARY_KEY, "Qwen Code uses an OpenAI-compatible endpoint: Model Studio needs a key; a custom endpoint may need none." },
};

static const screen_provider_settings_BINARIES_type *binary_info(const char *binary) {
    for (size_t i = 0; i < BINARY_COUNT; i++) if (str_eq(screen_provider_settings_BINARIES[i].id, binary)) return &screen_provider_settings_BINARIES[i];
    return NULL;
}

bool screen_provider_settings_mode_offered(const char *binary) {
    const screen_provider_settings_BINARIES_type *info = binary_info(binary);
    return info && info->auth == BINARY_CHOICE;
}

bool screen_provider_settings_uses_token(FormScreen *s) {
    const screen_provider_settings_BINARIES_type *info = binary_info(s->binary);
    // Unknown saved IDs retain their endpoint/key, without assuming a login protocol.
    if (!info) return true;
    return info->auth == BINARY_KEY || (info->auth == BINARY_CHOICE && s->token);
}

bool screen_provider_settings_can_log_in(FormScreen *s) {
    const screen_provider_settings_BINARIES_type *info = binary_info(s->binary);
    if (!info || info->auth == BINARY_KEY || screen_provider_settings_uses_token(s)) return false;
    return s->id && str_eq(s->binary, screen_provider_settings_row_binary(s->row));
}

const char *screen_provider_settings_connection_note(const char *binary) {
    const screen_provider_settings_BINARIES_type *info = binary_info(binary);
    return info ? info->note : "Unrecognized binary: existing settings are preserved. This client has no login contract for it.";
}

const char *screen_provider_settings_binary_title(const char *binary) {
    for (size_t i = 0; i < BINARY_COUNT; i++) if (str_eq(screen_provider_settings_BINARIES[i].id, binary)) return screen_provider_settings_BINARIES[i].title;
    return binary && *binary ? binary : "Pick a binary";
}

const char *screen_provider_settings_base_url_hint(const char *binary) {
    if (str_eq(binary, "claude")) return "Passed to the claude CLI as `ANTHROPIC_BASE_URL`, for a proxy or an Anthropic-compatible endpoint. Leave empty for the Anthropic API.";
    if (str_eq(binary, "opencode")) return "The base URL of the service this entry's models name (`anthropic` for `anthropic/claude-sonnet-4-5`), for a proxy or a compatible gateway; include the `/v1` when the service's own URL carries one. Handed to the CLI as an inline config (`OPENCODE_CONFIG_CONTENT`) alongside the key, so the machine's own opencode config is never touched. Leave empty for the service's own endpoint.";
    if (str_eq(binary, "dsh")) return "Anthropic-compatible Messages root, normally `https://api.deepseek.com/anthropic`. Leave empty for DeepSeek. Custom endpoints are probed with the same x-api-key authentication as the harness.";
    if (str_eq(binary, "qwen")) return "OpenAI-compatible endpoint including its version path. Leave empty for Model Studio (`https://dashscope-intl.aliyuncs.com/compatible-mode/v1`).";
    if (!str_eq(binary, "codex")) return "The endpoint stored for this binary; consult its server configuration for the expected protocol.";
    return "A custom API endpoint driven through the codex CLI (Responses wire format). Sessions run with a server-written `CODEX_HOME` pointing codex at it, so the machine's own `~/.codex` login is never touched. Leave empty for the binary's own service.";
}

const char *screen_provider_settings_api_key_hint(const char *binary) {
    if (str_eq(binary, "claude")) return "Passed as `ANTHROPIC_API_KEY`, used instead of a login. With a base URL it also goes out as `ANTHROPIC_AUTH_TOKEN`, so gateways that read `Authorization` instead of `x-api-key` see it too.";
    if (str_eq(binary, "opencode")) return "The key for the service this entry's models name: `anthropic` for `anthropic/claude-sonnet-4-5`. It is handed to the CLI as its whole credential store, so the machine's own opencode credentials stay untouched.";
    if (str_eq(binary, "dsh")) return "DeepSeek API key, sent by the harness as x-api-key; the harness does not use an interactive login.";
    if (str_eq(binary, "qwen")) return "Model Studio API key, or the custom endpoint's key. May be empty for a keyless custom endpoint; the server supplies the CLI's required placeholder.";
    return "Authenticates the base URL above.";
}


size_t screen_provider_settings_binary_count(FormScreen *s) {
    return s->binary_ids ? json_count(s->binary_ids) : BINARY_COUNT;
}

const char *screen_provider_settings_binary_id(FormScreen *s, size_t index) {
    if (s->binary_ids) return json_str(json_at(s->binary_ids, index));
    return index < BINARY_COUNT ? screen_provider_settings_BINARIES[index].id : NULL;
}

static void binaries_done(void *owner, Request *req) {
    FormScreen *s = owner;
    json_free(s->binary_ids);
    s->binary_ids = req->ok ? provider_settings_binary_ids(req->result) : NULL;
    // Catalog arrival never fills the form or replaces its current binary.
    pane_relayout(s->base.pane);
}

void screen_provider_settings_load_binaries(FormScreen *s) {
    if (s->binaries_loaded || !store_supports("provider_binary_catalog")) return;
    s->binaries_loaded = true;
    store_call("provider_binary_catalog", json_object(), 0, s, binaries_done, 0, &s->req_binaries);
}
