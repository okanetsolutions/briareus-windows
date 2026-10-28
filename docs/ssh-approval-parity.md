# SSH approval parity

This prerequisite adds only SSH approvals to Findings, including the “as root” label from [Swift #140](https://github.com/okanetsolutions/briareus-swift/pull/140). Main had SSH settings and held findings, but no attention parser, SSH request listing, or decision route.

Sources inspected:

- Swift at `949fa74165e2084f6135a69149fe6e737a162745`: `Mac/Core/FindingsLogic.swift` and `Mac/App/Findings/FindingsScreen.swift`; current main `9ce0628dfc6c3210b37f56a3380a552b60d00735` retains attention/elevation behavior.
- Server at `a469a237957520f29e8d58eae8ab049280ea49eb`: [API reference](https://github.com/nadinyamaui/briareus/blob/a469a237957520f29e8d58eae8ab049280ea49eb/docs/api-v1-reference.md#get-sshrequests), `lib/api-v1-catalog.js`, `lib/ssh-routes.js`, `lib/ssh.js`, `lib/attention.js` and `server.js`.

`GET /api/v1/ssh/requests` requires manage and returns `{requests:[...]}`; `POST /api/v1/ssh/requests/{id}/decision` requires manage and sends only `{decision:"approve"|"deny"}`, returning `{request:...}`. Admin users rule on all commands, members on their own sessions, further restricted to token projects; the server performs this user filtering. Read tokens cannot list or decide these requests. The attention endpoint wraps the same request records but also includes unrelated attention categories, so this change uses the focused request endpoint.

The service lists pending requests after checking expiry, active session turn, project/server access and cancellation. Decision validates the current request again, refuses already-decided/deleted/expired requests, and checks the approved server settings snapshot. Uploads carry source path, destination, size and SHA256 and use atomic replacement with mode 0600. Elevated commands are already wrapped by the server; the client only displays them and the `sudo` flag. It never builds a sudo command or handles a password. Pending request records omit permission mode, so the client explicitly reports it as unavailable rather than inventing ask/allow from unattended status.

Decisions require an explicit per-item click bound to the current displayed request snapshot and generation. Shared request-ID tombstones prevent duplicate or uncertain resubmission across views, refreshes, access refusals and reopen until disconnect. Full device/project scopes separate queues within a connection. Retry deadlines only grow, and finalizers own metadata independently of screen lifetime. Refusals redact inaccessible metadata without erasing tombstones; all errors require a fresh read and submitted items stay interlocked for server-side recovery.

Tests use fake transports and event gates only. No registered-server tools, real approvals, SSH commands, uploads or server setting changes are used. Core tests cover public metadata/elevation, eligibility, outcomes, refusals and cooldowns; API tests check exact encoded routes/bodies and lack of write retries; linked app tests exercise multiple views, stale actions/completions, expiry, navigation, teardown and refusal races.

Limits: SSH counts are discovered when Findings loads and refreshes approvals; this change does not add global attention polling or other attention categories. Metadata is memory-only. Unknown outcomes require checking the request on the server, with no in-app resubmit. No new Windows APIs or Switch shim additions are needed. Native Windows execution, sanitizers and coverage are verified by PR CI; local cross-compilation alone does not establish test execution.
