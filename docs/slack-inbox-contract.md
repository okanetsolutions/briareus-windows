# Slack JSON inbox contract and release checks

This client implements issue #125 under epic #120 against **nadinyamaui/briareus PR #121 at `9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89`**, inspected on 2026-10-07; that PR was still open at inspection.
The fixtures in `tests/slack_fixtures.h` are synthetic examples derived from the actual handlers, not a live provider capture.

Inspected sources at that exact SHA:

- [API reference](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/docs/api-v1-reference.md).
- [Catalog registrations](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/lib/api-v1-catalog.js).
- [Inbox handler](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/lib/slack-inbox.js).
- [JSON status/error mapping](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/lib/slack-routes.js).
- [Workspace shapes, scopes and provider errors](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/lib/slack.js).
- [Core provider simulations](https://github.com/nadinyamaui/briareus/blob/9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89/test/slack-inbox.test.js).

All nine JSON operations require `admin`; paths below are relative to `/api/v1`.
The app also checks the deployed catalog before navigation and each request, and independently requires an Admin device token even if a catalog entry is weaker.
Servers without these routes retain the existing Slack web screen.

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
Drafts live in memory keyed by `(workspace, channel, threadTs)` while navigating this inbox, including when hidden under Slack web or moved into another window.
Leaving the screen warns before discarding drafts.
The composer names its workspace/channel/thread destination and validates 1–8000 UTF-16 units to match the inspected JavaScript handler's limit, including emoji.
A confirmed receipt clears only the submitted draft; `workspaceChanged: true` clears private state and reloads workspaces without resending.
Network, malformed success and server failures retain an uncertain draft; only explicit recovery after inspecting history enables another send.
Cancellation discards pending completions and treats an in-flight send as uncertain; it does not promise to revoke a request already received by Slack.

Read marks debounce for one second to the newest message whose bottom entered the visible viewport while the app was active.
Hidden screens/background activity never generate read marks; failures stop automatic marks until refresh/navigation.
429 exposes Retry-After and blocks actions until the cooldown expires; no send is automatically retried.
401 uses the existing credential invalidation flow; 403, removed/changed workspaces and refused workspace credentials clear private inbox data.
Slack scope/credential errors arrive as 502 with explanatory provider text; they remain visible, and sends conservatively remain uncertain.
Every completion checks account identity, current catalog/admin access and the navigation generation before touching state.
Account or access changes clear private state in all registered inbox screens, including detached screens.
Inbox contents and drafts never enter the disk response cache or an agent session.

Slack mrkdwn renders user/channel mentions, labeled HTTPS links, emphasis, quotes and code through the existing Markdown renderer, with block/attachment text and file name/type/size/permalink fallbacks.
File bytes and `url_private_download` are never fetched; browser links may require a Slack login.
Live SSE, reconnect recovery and cross-client read synchronization belong to dependent issue #126.

## Required release gates

- Recheck PR #121 after merge, compare the final handlers/catalog/reference to this SHA, update the pinned fixtures as needed, and verify all nine deployed method/path/permission entries.
- Complete native Windows QA against that deployed core: workspace/public/private/DM/group-DM navigation, people paging/Open DM, history/thread paging, scoped drafts and destination-correct sends, read positions, mrkdwn, file metadata and recovery from 429/missing scopes/refused tokens.
- Complete real Slack validation with the workspace's installed user token, including confirmed credential rotation during send and private-access/revocation checks; simulated transports do not satisfy this gate.
- Pass Windows GCC/MSVC warnings as errors, core/app suites, coverage at the repository floor, ASan, leaks, UBSan/fortify, lint and editorconfig.
- Leave armed dashboard review/QA and held findings for user triage; this PR does not waive or merge them.
