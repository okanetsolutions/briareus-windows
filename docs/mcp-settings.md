# MCP server settings

Settings lists MCP servers for an **Admin** token when the deployed OpenAPI catalog advertises `GET /settings/mcp/servers`; each button also checks its own route and permission, so older servers and read/manage tokens do not expose these controls.

Create an HTTP endpoint or a stdio command in the dedicated form, save it, then check its connection or choose **Sign in again** to replace an OAuth login. Commands, their arguments and environment, and HTTP loopback endpoints execute on the **core machine**, not this PC. Core mounts these tools into Claude/Codex turns; Windows manages only the registry.

The Repositories tab assigns all repositories (`repos: []`, including future repositories) or selected repositories. Existing assignments to inactive or unavailable projects remain visible; remove unavailable assignments explicitly. Disable the server to mount it nowhere without deleting its assignments.

Arguments are a JSON array of strings: `["--option", "value with spaces", ""]`. Headers/environment are JSON objects of string values. Their secret controls cycle through **Keep stored**, **Replace whole set**, and **Clear stored**:

- Keep omits `headers`, `env` or `oauthClientSecret` from the request; stored values are never read into the form.
- Replace supplies the entire header/environment set, including entries you want to retain, or the client secret exactly as typed.
- Clear supplies `{}` for headers/environment or `""` for the client secret.

Stored indicators show `headerNames`, `envNames` and `hasOAuthClientSecret`. Supplying an Authorization header bypasses OAuth. Changing transport removes the old transport's secrets on core. Advanced OAuth includes client ID, client secret, scope, client name, and callback/loopback redirect mode.

OAuth URLs open in the system browser after saving/checking or starting a new sign-in. Callback mode finishes on core; status refreshes every five seconds while the form and account remain active, with backoff and Retry-After on failed reads. Loopback mode requires pasting the **complete** callback URL, even if its page cannot load, then choosing Finish sign-in. The callback field clears when the attempt changes or finishes. Failed callbacks are never automatically resubmitted; start a fresh sign-in if needed.

Core expires sign-ins after 15 minutes and can invalidate them on configuration changes or restarts. This contract has **no sign-in expiry timestamp**. The client uses fresh `signInUrl` presence, never a countdown restarted on reopening the form. A null URL invalidates the attempt even when `status` remains `needs-sign-in`; failed or uncertain status reads disable the existing attempt until a fresh sign-in. A failed finish keeps that same URL blocked.

Reads are cancelled/discarded on screen changes and before writes. A retained connection identity prevents late responses from another account changing the form or opening a browser. Cancelling a write locally cannot undo a request already received by core; refresh Settings before repeating it, especially a create or single-use callback. Writes are not automatically retried. UI request errors contain status and recovery advice without echoing request secrets or pasted OAuth URLs.

## Pinned contract and release gates

Implementation and fixtures use **nadinyamaui/briareus PR #124 head `8585d12e20e57097bae291659765f9cfe9e14691`**, rechecked through `gh api`; base SHA was `f0ccdf6fda9c06be41db0b5a2ff7eab018306edf`. Sources inspected at that exact head: `docs/api-v1-reference.md` (six routes and McpServer), `lib/api-v1-catalog.js` (admin catalog entries), `lib/mcp-routes.js` (request/response wrappers), and `lib/mcp-servers.js` (validation, secret replacement, millisecond IDs, OAuth lifetime and single-use handling). The Windows starting SHA was `447993765584f7e1d292ca2b9f9f0d2136fbca0d`.

| Call | Method and path under `/api/v1` |
| --- | --- |
| settings_mcp_servers | GET /settings/mcp/servers |
| create_mcp_server | POST /settings/mcp/servers |
| update_mcp_server | PUT /settings/mcp/servers/{id} |
| delete_mcp_server | DELETE /settings/mcp/servers/{id} |
| connect_mcp_server | POST /settings/mcp/servers/{id}/connect (`signIn: true` starts a fresh attempt) |
| finish_mcp_sign_in | POST /settings/mcp/servers/{id}/finish-sign-in (`url` is the full callback URL) |

Core PR #124 was **open**, not merged, when inspected. No upstream core changes or merges were made. **Final merged-contract verification, deployed catalog/provider verification, native Windows UI QA, real ordinary and paste-back MCP OAuth sign-in/token exchange, and Claude/Codex mounting verification remain pending** for release. Automated fixtures and cross-compilation are not evidence of provider or deployed validation. Coordination scope: this change adds only MCP routes, helpers, form and a Settings list; mail and other epic features remain separate work.
