# Session Resources parity

The conversation header's Resources action opens a read-only screen for that session when the route catalog and token allow `session_stats`; it uses no command, server-stat, or global Status route.

Sources inspected:

- [Swift #133](https://github.com/okanetsolutions/briareus-swift/pull/133), merge `d86db7d57f64bb3691a0f9b3a1b7f3bea598f24a`: `Mac/Core/StatsLogic.swift`, its tests, session Resources model/view, shared tiles/charts, and conversation header.
- [Server #160](https://github.com/nadinyamaui/briareus/pull/160) and [current API reference](https://github.com/nadinyamaui/briareus/blob/606409a872c3f0d77ddc332029ffdb522768b608/docs/api-v1-reference.md): the route catalog, `lib/session-stats-routes.js` and sampler were checked at `606409a872c3f0d77ddc332029ffdb522768b608`.
- [Current contracts](https://github.com/okanetsolutions/briareus-contracts/tree/c79b0d9f8e96fb9ac227fc67cecc1cbefa9b4435): existing theme/chart tokens are reused; this revision has no Resources feature entry or dedicated wireframe.

## API and display

`GET /sessions/{id}/stats` needs `read` and is held to the session's project; unknown IDs return 404. The response is `{stats, history, host}`. Explicit null `stats` means not open or not yet sampled (including failed/queued sessions); this is not inferred from the conversation's older status. Open idle/stopped agents can report measured zeros. Closed sessions have no history and continue following while visible so reopening can recover.

CPU is a percentage of one core, including in the CPU chart; the gauge divides by `host.cpus * 100`. Memory uses resident bytes and the host memory total for its gauge. Binary byte units and percent precision, process role order (`agent`, `serve`, `browser`), labels, and 75%/90% gauge tones follow Swift. Missing CPU, memory, process counts, host totals and disk remain unavailable, including malformed, negative, nonfinite and unsafe integer metrics. Unknown roles do not invent rows. Disk measurements and all sample timestamps are epoch milliseconds; UTC labels identify their time zone.

Core derives the tiles, normalized shares, tones and two series. Last-hour history is ordered and deduplicated, invalid timestamps are dropped, and missing metrics remain gaps. The charts show the server's history rather than accumulating client samples; fewer than two usable samples show the starting-history note. `stat_components` draws reusable tiles, gauges and lines through Doc/theme and existing chart tokens; Usage is unchanged.

## Request ownership

Each screen retains its originating client, device permission snapshot and session. One pending read coalesces refresh/poll requests. Navigation/hiding/destruction cancels that slot; completions for another connection or device scope cannot update the screen. In-memory readings stay with their screen, never in a shared response cache. Errors preserve last successful data with explicit stale state, except 403/404 clears private figures (the server hides inaccessible sessions with 404). A changed connection/token scope hides old data.

One existing Poller reads only while visible and active: initially every five seconds, then the host interval, at least two seconds. Failures back off; a 429 without Retry-After waits at least a minute. Session/device-scoped absolute deadlines live in Store for the connection lifetime, survive refresh/hide/recreation without relying on disk-cache writes, and are cleared when the client is dropped. Large deadlines are preserved while individual timer waits stay representable. There are no per-chart/process timers or additional requests.

The connection also retains a refusal epoch for each session/device scope. A 403/404 advances it, clears all matching live views (including hidden and detached views), and makes successes that were already pending obsolete. The epoch survives a later authorized recovery and is checked before layout and reveal, so an older response cannot restore private history during a cooldown or after another view recovers. Unrelated session/project scopes keep their figures.

Each Resources request owns its completion metadata and retains its originating client through Store. Its UI-thread finalizer checks both client identity and the device snapshot even when the view was cancelled or destroyed. A matching 401 clears surviving connection views, cancels their reads/polling, and invokes Store's existing credential/cache invalidation; a superseded client or device cannot invalidate its replacement. Store's ordinary 401 fallback defers to that explicit scoped check for these requests.

## Verification scope

Core fixtures cover numeric presence/zero, invalid envelopes/identity, nested roles, units, thresholds, time conversion, history and polling decisions, plus the encoded GET route. Linked screen tests use explicit per-request completion gates for entry-point permissions/navigation, polling and coalescing, cooldown/reopening, malformed/partial/error recovery, closed sessions, background pauses, cancellation, teardown and connection/session/permission isolation, overlapping pre-refusal successes, cross-view refusal epochs through fresh recovery, and cancelled 401 credential/cache invalidation with replacement-client/device immunity.

Windows CI is required for actual executable, memory and sanitizer results. Linux cross-compilation is build evidence only. Live-server/visual verification is not claimed. No new Win32 dependency was introduced; the Switch shim uses the existing drawing, timer and store interfaces. The server Resources dashboard remains a separate follow-up.
