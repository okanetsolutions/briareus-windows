# Slack live updates contract

Issues #125/#126 under epic #120 use the final merged core Slack contract at
**nadinyamaui/briareus `54ba5c987e7fae8685766b38459632879ff577fd`** (PR #121).
Inspected pinned sources: [inbox](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack-inbox.js),
[workspace/event access checks](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack.js),
[routes](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack-routes.js),
[catalog](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/api-v1-catalog.js),
[guide](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/docs/api-v1.md), and
[reference](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/docs/api-v1-reference.md).
Fixtures are synthetic contract examples, never evidence of deployed-provider or native UI validation.

`slack_events` registers `GET /slack/workspaces/{id}/events` with Admin/catalog gating.
An older catalog without this route keeps the JSON inbox and manual Refresh; one without the inbox keeps Slack web.
No new dependency, durable cursor, attachment download, disk inbox cache, or agent-session delivery is added.

| Event | Pinned payload | Handling |
| --- | --- | --- |
| `ready` | `{workspaceId, userId, refresh:true}` | Begin a new snapshot generation and buffer events |
| `message` | `{workspaceId, eventId, event:{channel, ts, ...}}` | Merge own, human, bot and thread messages by workspace/channel/string ts |
| `message.changed` | `{workspaceId, eventId, event:{channel, message:{ts, ...}}}` | Apply partial edits and `message_replied` parent metadata |
| `message.deleted` | `{workspaceId, eventId, event:{channel, deleted_ts}}` | Remove identity and retain a tombstone against late receipts/history |
| `conversation.read` | `{workspaceId, channel, ts}` | Advance local confirmed cursor, suppress older automatic marks |
| `workspace.changed` | `{workspaceId}` | Cancel, clear private state and reload workspace identity before reconnect |
| `workspace.removed` | `{workspaceId}` | Cancel, clear private state and reload workspace picker |

One active inbox owns a cancellable WinHTTP stream; opening/refreshing another inbox stops the previous stream.
Navigation, hiding, destruction, sign-out and access/catalog changes cancel it.
The refcounted reader owns an ordered locked mailbox, never a screen/window pointer; late completions cannot touch a destroyed screen.
Stream generations reject old events/errors; navigation generations reject old JSON snapshots/receipts.
Local workspace setting changes invalidate registered inbox screens synchronously, including detached ones.

Every ready replaces the complete paginated conversations snapshot and reloads conversation details,
channel history back through the oldest cached channel message, and all pages of an open thread.
Empty cursor pages are followed; malformed/nonprogressing pagination aborts reconciliation and reconnects.
Events are replayed in arrival order only after all required snapshots complete.
Invisible cached messages are discarded and drafts/read state for conversations absent from the fresh list are removed.
Own receipt overlap does not overwrite a preceding edit or resurrect a deleted message.

Both transport and reconciliation buffers hold at most 256 events and 2 MiB including event names.
Overflow terminates the current stream/reconciliation and reconnects for new snapshots; the parser's 2 MiB overflow is also observable.
Reconnect starts at one second, doubles to a minute, and honors a longer Retry-After (bounded at 30 days).
Only completed reconciliation resets failures, so repeated ready/failure cycles cannot spin.
A stream 409 disables automatic reconnect and gives signing-secret/Event Subscriptions guidance while history, manual Refresh and sending remain available.
A JSON snapshot 409 is credential rotation, not the setup fallback.
401/403/404 and refused workspace credentials clear private state; 401 also enters the shared device-token invalidation flow.

Read synchronization seeds `last_read` from details and snapshots and applies cross-client events.
Only channel messages actually entering an active, visible, nonminimized pane can advance debounced marks;
thread views never mark unrelated channel messages as read.
Reconciliation suppresses automatic marks until snapshots and buffered reads have merged.
Ambiguous sends remain uncertain and require explicit destination/generation-checked recovery after history inspection;
no stream/reconnect/overflow path retries a human send.

Automated regressions exercise state identity/tombstones, bounded queues, SSE overflow signaling,
pre-start transport cancellation, ready ordering, duplicate overlap, multi-page disconnect catch-up,
thread snapshots, rotation/removal, stale generations, read sync, revocation, 409 and backoff.
Required Windows CI covers GCC/MSVC Werror, core/app tests, coverage, ASan, debug-CRT leaks,
UBSan/fortify, lint and editorconfig. No QA sessions or live human/provider sends are performed for this implementation, per user instruction.
