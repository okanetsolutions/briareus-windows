# Briareus for Windows

A native Win32 client for [Briareus](https://github.com/nadinyamaui/briareus), the dashboard for running coding agents against your projects, written in C11 with no third-party dependencies. It is a port of [Briareus for iPhone and iPad](https://github.com/okanetsolutions/briareus-ios): it talks to the same versioned mobile API (`/api/mobile/v1`) and works with any Briareus server you can reach over HTTPS. Requires Windows 10 version 1809 or later.

## What it does

**Conversations**

- Lists the projects and conversations the device token permits, with search and status updates.
- Shows incremental transcripts with the time of each message, agent questions, tool activity (collapsed into clusters, expandable) and queued messages. Workspace setup steps are left out.
- Renders agent replies as Markdown: headings, paragraphs, bullet and numbered lists, task lists, quotes, code blocks with a copy button, tables, rules, bold, italic, strikethrough, inline code and links.
- Starts conversations on a chosen branch, provider, model and effort, or on the project default.
- Sends follow-ups (Enter sends, Shift+Enter breaks a line), renames, stops, closes, reopens and deletes sessions.
- Turns the review loop on or off and completes the triage of held findings from inside a conversation.
- Records voice notes with the microphone (AAC through Media Foundation, WAV as a fallback) and has the server transcribe them into the message box. On a server that cannot transcribe, the microphone says what the server is missing.
- Keeps the projects and conversations in a column on the left and the chosen conversation on the right, as the dashboard does. A window too narrow for both falls back to a single column with a back button.

**Project board**

- Shows open pull requests as the dashboard does: labels, whether they conflict with their base, the state of their checks, author, assignees, reviewers, linked issues and stack position, narrowed by author, reviewer or label.
- Opens a pull request on its description, file changes with diffs (wrapped or scrolled), checks, reviews, commits, the issues it closes, findings and the conversations already run on it.
- Records fix, optional or dismiss decisions on findings, and merges when the server offers it, saying first what stands in the way.
- Starts the board's errands on a pull request: run, code review, solve conflicts, fix failing checks, implement feedback, feedback in your own words, test sheet, QA, PR body and delete my comments. The one the pull request's state asks for is marked as suggested.
- Lists the repository's open issues, sub-issues nested under their epic, with the pull requests answering each, and starts a session on an issue.

**Connection**

- Pairs with a per-device token stored in Windows Credential Manager (this device only), and revokes it remotely or forgets the local connection.
- Hides write controls on a Read-only token. A capability catalog read from the server keeps operations it does not offer unavailable, so the app adapts to older and newer servers.
- Saves projects, conversations, transcripts and pull requests on the computer. A screen opens on what it last showed and then asks the server only for what changed; a saved transcript resumes from its last event, and F5 reads it again in full.
- Pauses polling while the window is minimized or in the background, with exponential backoff and `Retry-After` after failures.
- Follows the system light or dark theme, including the title bar, and scales with the monitor's DPI.

## Build

Any of these produces `build\Briareus.exe`:

- **MinGW-w64 GCC** (for example [WinLibs](https://winlibs.com)): `mingw32-make` builds the app and runs the tests; `mingw32-make app` builds only the app.
- **Visual Studio Build Tools**: from a Developer Command Prompt, `build.bat`.

There is nothing to install besides the compiler. The executable is statically linked and has no runtime dependencies beyond Windows.

## Validation

```sh
mingw32-make test
```

The core (JSON, models, API client, cache, diff, Markdown and board logic) has no UI code and is exercised by `tests\core_tests.c`, a port of the iOS app's core tests plus the Windows renderer's Markdown extensions: origin validation, credential headers, operation bodies, redirect rejection, non-JSON responses, expiry, rate limiting, write timeouts without retry, revocation, response compatibility, transcript cursor and deduplication, runtime selection, pull request file paging, diff line numbering, board rows, filters, errands, issue nesting, merge warnings and the saved-response cache. HTTP is stubbed through the client's pluggable transport.

## Project layout

| Path | Contents |
| --- | --- |
| `core/` | The portable core: JSON, models, API client over WinHTTP, saved-response cache, board, diff and Markdown parsing, Credential Manager and registry access. No UI. |
| `app/` | The Win32 app: theme and drawing helpers, the item-based layout toolkit (`doc.c`), the screen stack container (`pane.c`), the connection store with UI-thread requests (`store.c`), the screens, the dialogs and voice notes. |
| `tests/` | Core tests. |
| `res/` | Icon, manifest, dialogs and version resources. |

## Pairing

1. Sign in to the web dashboard and open **Settings → Mobile devices**.
2. Create a token: give the device a name, choose the projects it may see, **Read only** or **Manage**, and an expiry.
3. In the app, enter the public HTTPS server address (or its `/api/mobile/v1` URL) and the one-time token.

A revoked or expired token returns the app to pairing. Forgetting the connection removes local credentials and saved conversations only. It does not revoke the server token or stop running agents.

## Security and privacy

- HTTPS is required. The transport has no cookies or HTTP cache, refuses all redirects, and never automatically retries a write. TLS 1.2 or later.
- No shared secret is built into the app. Each device holds its own token in Credential Manager, scoped by canonical server origin and persisted for this machine only. Only the server origin is saved in the registry (`HKCU\Software\Okanet\Briareus`).
- Saved responses live under `%LOCALAPPDATA%\Okanet\Briareus\Responses`, encrypted with EFS where the volume allows it. They are erased when the connection is forgotten, revoked, expired or replaced by another device token, and entries untouched for 30 days are dropped.
- Voice notes are sent to your server for transcription and nowhere else.
- No analytics or telemetry.
