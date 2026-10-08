# Slack JSON inbox contract and release checks

This client implements issue #125 under epic #120 against **nadinyamaui/briareus PR #121 at `54ba5c987e7fae8685766b38459632879ff577fd`**, merged and rechecked on 2026-10-08.
The fixtures in `tests/slack_fixtures.h` are synthetic examples derived from the actual handlers, not a live provider capture.

Inspected sources at that exact SHA:

- [API reference](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/docs/api-v1-reference.md).
- [Catalog registrations](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/api-v1-catalog.js).
- [Inbox handler](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack-inbox.js).
- [JSON status/error mapping](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack-routes.js).
- [Workspace shapes, scopes and provider errors](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/lib/slack.js).
- [Core provider simulations](https://github.com/nadinyamaui/briareus/blob/54ba5c987e7fae8685766b38459632879ff577fd/test/slack-inbox.test.js).

All nine JSON operations require `admin`; paths below are relative to `/api/v1`.
The inbox checks the deployed catalog before loading private data and each request, and independently requires an Admin device token even if a catalog entry is weaker.
The sidebar always opens the native inbox; servers without these routes or devices without Admin access see its unavailable state.
The embedded Slack web screen is retired; the inbox’s Slack web button opens the default browser, and native message/file links remain available.

| Client operation | Method/path | Contract |
| --- | --- | --- |
| `slack_workspaces` | `GET /slack/workspaces` | `{workspaces: SlackWorkspace[]}`, numeric workspace IDs, `projects: []` supported |
| `slack_conversations` | `GET /slack/workspaces/{id}/conversations` | `{conversations: object[], nextCursor: string}`, all four conversation types |
| `slack_conversation` | `GET /slack/workspaces/{id}/conversations/{channel}` | `{conversation: object}`, Slack topic/purpose and flags |
| `slack_people` | `GET /slack/workspaces/{id}/people` | `{people: object[], nextCursor: string}`, users/profiles for authors and Open DM |
| `slack_open_dm` | `POST /slack/workspaces/{id}/direct-messages` | `{userId: string}` → 201 `{conversation: object}` |
| `slack_history` | `GET /slack/workspaces/{id}/conversations/{channel}/messages` | `{messages: object[], nextCursor: string, hasMore: boolean}` |
| `slack_thread` | `GET /slack/workspaces/{id}/conversations/{channel}/threads/{ts}` | Same page shape, parent and replies in chronological order |
| `slack_send` | `POST /slack/workspaces/{id}/conversations/{channel}/messages` | `{text: string, threadTs?: string}` → 201 `{channel, ts, message}` or confirmed `{channel, ts, workspaceChanged: true}` |
| `slack_read` | `POST /slack/workspaces/{id}/conversations/{channel}/read` | `{ts: string}` → 200 `{ok: true}` |

Directory queries accept `cursor` and `limit` (1–200, default 100); conversations also accept `types` (`public_channel,private_channel,im,mpim`).
History/thread queries accept `cursor`, `limit` (default 15), and exclusive string `oldest`/`latest` bounds.
History reads newest first; replies read oldest first, so cursor-free `hasMore` advances `latest` for history and `oldest` for replies.
A nonempty cursor is followed even if its page has no messages/people/conversations; a repeated cursor or an empty cursor-free page with `hasMore` stops visibly instead of looping.
Thread and conversation timestamps stay decimal strings, including sub-microsecond fractions in fixtures; no floating point identity or bound comparisons are used.

Messages merge by `(workspace, channel, ts)`; partial updates preserve other fields and deletion removes only that identity, for reuse by issue #126.
Drafts live in memory keyed by `(workspace, channel, threadTs)` while navigating this inbox, including when moved into another window or while Slack is open in the default browser.
Leaving the screen warns before discarding drafts.
The composer names its workspace/channel/thread destination and validates 1–8000 UTF-16 units to match the inspected JavaScript handler's limit, including emoji.
A confirmed receipt clears only the submitted draft; `workspaceChanged: true` clears private state and reloads workspaces without resending.
Network, malformed success and server failures retain an uncertain draft; only explicit recovery after inspecting history enables another send.
Cancellation discards pending completions and treats an in-flight send as uncertain; it does not promise to revoke a request already received by Slack.

Read marks debounce for one second to the newest message whose bottom entered the visible viewport while the app was active.
Hidden screens/background activity never generate read marks; failures stop automatic marks until refresh/navigation.
429 exposes Retry-After and blocks actions until the cooldown expires; no send is automatically retried.
401 uses the existing credential invalidation flow, and 403 clears private state even if the local device record still says Admin.
Read failures indicating removed/changed workspaces or refused workspace credentials clear private inbox data; ambiguous send failures retain their drafts until explicit recovery or an account/access change.
Slack scope/credential errors arrive as 502 with explanatory provider text; they remain visible, and sends conservatively remain uncertain.
Every completion checks account identity, current catalog/admin access and the navigation generation before touching state.
Account or access changes clear private state in all registered inbox screens, including detached screens.
Inbox contents and drafts never enter the disk response cache or an agent session.

Slack mrkdwn renders user/channel mentions, labeled HTTPS links, emphasis, quotes and code through the existing Markdown renderer, with block/attachment text and file name/type/size/permalink fallbacks.
File bytes and `url_private_download` are never fetched; browser links may require a Slack login.
Live SSE, reconnect recovery and cross-client read synchronization are described in [slack-live-contract.md](slack-live-contract.md).

## Verification scope

- The final merged core handlers, catalog and documentation are pinned above; synthetic tests do not establish deployed-provider behavior.
- Required Windows GCC/MSVC Werror, core/app suites, coverage, ASan, leaks, UBSan/fortify, lint and editorconfig must pass.
- Per user instruction for #126, do not run QA sessions, native UI QA or live human/provider sends; retain code review and required CI.
