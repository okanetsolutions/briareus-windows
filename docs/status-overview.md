# Status overview

Windows parity for Swift [#108](https://github.com/okanetsolutions/briareus-swift/pull/108)
(56ab3922ecf8e46a9f724848e96ba1435fdd8162) and
[#110](https://github.com/okanetsolutions/briareus-swift/pull/110)
(b3203687aa6198db3b2a387090a22195769093c5).

The sidebar's ◉ Status opens a detachable overview of top-level conversations.
Need you includes unanswered questions and held findings; Working and Ready exclude
failed/closed sessions. Questions take precedence over held findings. Waiting rows
are oldest first; working rows sort by title. Findings keeps its separate badge.
Held findings open their conversation for decisions.

The existing Windows client uses Poller and aggregate `sessions` reads, rather than
Swift's SessionFeed SSE updates. Status uses that same store/poller lifecycle and
per-project cache through shared session_cache helpers. One global refresh runs
while Status is visible, with background/backoff handling; there are no per-card
timers. Question reads are sequential and restricted to top-level waiting sessions,
using saved transcripts and their cursors. Hidden/destroyed screens cancel reads;
connection identity rejects old-server results. Missing cached lists and failed
reads remain explicit, and unconfirmed cached questions cannot be answered.

Inline answers require both the catalog route and token permission for `message`.
Before sending an option or trimmed text, Status reads the session again and
compares the ask's full event identity and turn identity. Rebuilt lists and question
reads invalidate rendered actions immediately, before deferred paint. Keyboard
submission uses the rendered Send action and the same invalidation guard. Changed asks
retain the draft and require a new action. Successful sends suppress repeats of
that ask, while a new ask resets suppression even without optional timestamps.

Writes are never retried automatically. Refusals retain drafts/errors; other errors
and local cancellation of a send block further sends until the user checks the
conversation and explicitly requests a refresh. Per-session send interlocks live
with the connection, surviving Status destruction and reopening; replacing the
connection clears them. The current server message route
accepts text, without an atomic expected-question argument, so a server-side change
between the confirmation read and write remains a transport limitation.

Core and linked app tests cover state precedence/counts/order, pending asks,
question identity replacement, stale actions before repaint, catalog/token gates,
message bodies, duplicate suppression, refusal/uncertain/cancellation paths,
count refresh, project/server isolation, held navigation and teardown. Windows CI
executes these tests; a local MinGW cross-build alone is compilation evidence.
No new Win32 APIs or dependencies were introduced; existing edit/control and region
APIs are reused, including the APIs already consumed by the Switch shim.

The overview centers a capped two-column layout on wide panes and stacks Working
below questions on narrow panes. Totals appear once; unavailable totals use an
unknown marker rather than zero. Titles and option values wrap without clipping.
The native reply field has a preceding label, with no repeated cue banner, and
keeps the same control and draft through refresh, scroll and resize.
