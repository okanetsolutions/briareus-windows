# Briareus for Windows

[![CI](https://github.com/okanetsolutions/briareus-windows/actions/workflows/ci.yml/badge.svg)](https://github.com/okanetsolutions/briareus-windows/actions/workflows/ci.yml) [![Release](https://img.shields.io/github/v/release/okanetsolutions/briareus-windows)](https://github.com/okanetsolutions/briareus-windows/releases/latest) [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A native Win32 client for [Briareus](https://github.com/nadinyamaui/briareus), the dashboard for running coding agents against your projects, written in C11 with no third-party dependencies. It talks to the server's client API (`/api/v1`) with a per-device token and works with any Briareus server you can reach over HTTPS. Requires Windows 10 version 1809 or later.

## What it does

**Conversations**

- Lists the projects and conversations the device token permits, with search and status updates.
- Shows incremental transcripts with the time of each message, agent questions, tool activity (collapsed into clusters, expandable) and queued messages. Workspace setup steps are left out.
- Renders agent replies as Markdown: headings, paragraphs, bullet and numbered lists, task lists, quotes, code blocks with a copy button, tables, rules, bold, italic, strikethrough, inline code and links.
- Selects text as a browser does: drag across messages, double-click a word, Ctrl+A for everything; Ctrl+C or the right-click menu copies it. The menu also copies the paragraph under the pointer.
- Starts conversations on a chosen branch, provider, model and effort, or on the project default.
- Sends follow-ups (Enter sends, Shift+Enter breaks a line), renames, stops, closes, reopens and deletes sessions.
- Attaches files to a message as the dashboard's composer does: an image pasted into the message box goes as a PNG, and files copied in Explorer paste or drop into it. Each is stored on the server as it is attached, shows above the text with its size until sent, and the agent gets its path. On a server whose API does not take uploads, the paste says so.
- Turns the review loop on or off and completes the triage of held findings from inside a conversation.
- Keeps a Findings screen, as the dashboard does: every review round waiting for a decision across the projects, grouped by pull request, with a count beside the projects. A round on your own pull request takes a verdict (fix, optional, dismiss) and a comment on each finding, saves them to the pull request, and completes into the fix session; a review of somebody else's takes replies on its findings' threads, deletes a finding from the review, and is taken off the queue. What a completion led to stays on the screen until dismissed.
- Records voice notes with the microphone (AAC through Media Foundation, WAV as a fallback) and has the server transcribe them into the message box. On a server that cannot transcribe, the microphone says what the server is missing.
- Keeps the dashboard's 📊 Dashboard: what every project spent over a window (this month, last month, all time, today, the last 7 or 30 days), with cost, tokens, sessions and agent time, tokens and cost per day or month, the averages and the comparison with the window before, the ten most expensive sessions (a click opens one), and the spend by project, activity, provider and model with a ring of each one's share of the tokens. Six pickers narrow every number to some projects, models, an activity, a provider, an account or a session, and a row of a breakdown narrows to itself. It reads `GET /usage/all`, so it needs an Admin token; any other token is told so.
- Keeps the dashboard's ⚙ Settings for projects: the sidebar turns into the settings page's own (← Back to sessions, Devices and clients, the projects with ＋ New), and a project opens across the whole pane with the dashboard's Project (with its setup), Database, Code review, Orchestrator, Checkout .env and Run sections as tabs, as the pull request page has its own (a dot marks a tab with unsaved changes), with the provider, model and effort pickers for the code review, each errand step and the orchestrator's workers. Save (Ctrl+S), ⧉ clone into a new project and 🗑 delete work as on the web; right-click a project to move it up or down the list. A form with unsaved changes asks before another screen replaces it, a provider the server no longer lists stays picked rather than being swapped for another, and a setting the server's projects do not carry is not shown. It reads and writes `/settings/projects`, so it needs an Admin token; any other token is told so.
- Manages the providers sessions start on from the same ⚙ Settings: a Providers section under the projects (each with its dot and its CLI, inactive, own login and custom endpoint tags, and ＋ New) opens a provider across the whole pane as tabs: Provider (active, label, binary, and its login or API token: Log in opens the browser, with the code claude.ai shows pasted back or codex's device code shown, and an endpoint's Test fills in its models), Models (models, efforts and their defaults) and, once saved, Status (the account, plan, binary, login dir and each quota window, with Check usage). Save, ⧉ clone and 🗑 delete work as for projects. It reads and writes `/settings/providers`, so it needs an Admin token as well.
- Keeps the dashboard's SSH servers under ⚙ Settings, below the projects with their own ＋ New: each with its dot (available or not), its project and an `unasked` tag when its commands run without approval. A server opens across the whole pane with Server (available to sessions, label, project), Connection (host and port, username, private key path) and Permissions (ask for all commands, or don't ask anything) as tabs, in the same form as a project's: Save (Ctrl+S), ⧉ clone and 🗑 delete, a dot on a tab with unsaved changes, and a refusal opens the tab holding the field at fault. It reads and writes `/settings/ssh/servers`, so it also needs an Admin token.
- Keeps the projects and conversations in a column on the left and the chosen conversation on the right, as the dashboard does: each project with its session count and a dot while one works, and inside it the conversations with their provider, branch, state and age; ☑ Select ticks several to close or delete at once. A window narrower than the dashboard's `lg` breakpoint falls back to a single column with a back button.

**Project board**

- Shows open pull requests as the dashboard does: labels, whether they conflict with their base, the state of their checks, author, assignees, reviewers, linked issues and stack position, narrowed by author, reviewer or label.
- Opens a pull request on its description, file changes with diffs (wrapped or scrolled), checks, reviews, commits, the issues it closes, findings and the conversations already run on it. One built on other branches shows its stack position beside its state, as GitHub does, and opens the stack overview from it: the pull requests top first with their branches, this one marked, and the branch the bottom merges into. The overview is there however the pull request was opened, from the board, a conversation or a saved copy.
- Records fix, optional or dismiss decisions on findings, starts the fix session from them with Solve findings, and merges when the server offers it, saying first what stands in the way.
- Starts the board's errands from the buttons under each pull request, as the dashboard has them, or from the pull request itself: view on GitHub, run, code review, solve conflicts, fix failing checks, implement feedback, feedback in your own words, PR body and delete my comments. The one the pull request's state asks for is highlighted.
- Lists the repository's open issues, sub-issues nested under their epic, with the pull requests answering each, and starts a session on an issue.

**Connection**

- Pairs with a per-device token stored in Windows Credential Manager (this device only), and revokes it remotely or forgets the local connection.
- Hides write controls on a Read-only token. The server's own route catalog (`GET /api/v1/openapi.json`) is read at pairing and on every launch, and a control whose route the server lacks, or that the token's permission may not call, is kept unavailable, so the app adapts to older and newer servers.
- Saves projects, conversations, transcripts and pull requests on the computer. A screen opens on what it last showed and then asks the server only for what changed; a saved transcript resumes from its last event, and F5 reads it again in full.
- Pauses polling while the window is minimized or in the background, with exponential backoff and `Retry-After` after failures.
- Opens filling the screen in the dashboard's own look: its dark palette, Segoe UI at its pixel sizes, Cascadia Code for code, its 268px sidebar with the ＋ New session strip, and its Welcome back composer with the project, branch, provider, model, effort and loop chips. Scales with the monitor's DPI.
- A conversation's actions (stop, close, reopen, delete, and ✎ to rename) sit in its header as the dashboard's buttons, the review loop as a chip above the composer, and its pull request, commits, reviews and findings in the 272px panel on the right, as on the dashboard. A wide window lays a pull request out in two columns.

## Build

Any of these produces `build\Briareus.exe`:

- **MinGW-w64 GCC** (for example [WinLibs](https://winlibs.com)): `mingw32-make` builds the app and runs the tests; `mingw32-make app` builds only the app.
- **Visual Studio Build Tools**: from a Developer Command Prompt, `build.bat`.

There is nothing to install besides the compiler. The executable is statically linked and has no runtime dependencies beyond Windows.

## Validation

```sh
mingw32-make test
```

The core (JSON, models, API client, cache, diff, Markdown and board logic) has no UI code and is exercised by `tests\core_tests.c`: origin validation, credential headers, the route and arguments of every call, the route catalog and its permissions, redirect rejection, non-JSON responses, expiry, rate limiting, write timeouts without retry, revocation, response compatibility, transcript cursor and deduplication, runtime selection, pull request file paging, diff line numbering, board rows, filters, errands, issue nesting, merge warnings and the saved-response cache. HTTP is stubbed through the client's pluggable transport.

## Project layout

| Path | Contents |
| --- | --- |
| `core/` | The portable core: JSON, models, the `/api/v1` client over WinHTTP (one table of the calls the app makes and their routes), saved-response cache, board, diff and Markdown parsing, Credential Manager and registry access. No UI. |
| `app/` | The Win32 app: theme and drawing helpers, the item-based layout toolkit (`doc.c`), the screen stack container (`pane.c`), the connection store with UI-thread requests (`store.c`), the screens, the dialogs, voice notes and attachments (`attach.c`: the clipboard's image as PNG through Windows Imaging Component, dropped or pasted files read from disk). |
| `tests/` | Core tests. |
| `res/` | Icon, manifest, dialogs and version resources. |

## Pairing

1. Sign in to the web dashboard and open **Settings → Devices and clients**.
2. Create a token: give the device a name, choose the projects it may see, **Read only** or **Manage**, and an expiry. An **Admin** token works too and is shown as such; it is the operator's own and is held to no project list.
3. In the app, enter the public HTTPS server address (or its `/api/v1` URL) and the one-time token.

Behind Cloudflare Access, the server's `/api/v1` and `/api/v1/*` paths need the Bypass application described in the server's [client API guide](https://github.com/nadinyamaui/briareus/blob/main/docs/api-v1.md#deploying-behind-cloudflare-access); the app refuses the login page it is otherwise redirected to.

A revoked or expired token returns the app to pairing. Forgetting the connection removes local credentials and saved conversations only. It does not revoke the server token or stop running agents.

## Security and privacy

- HTTPS is required. The transport has no cookies or HTTP cache, refuses all redirects, and never automatically retries a write. TLS 1.2 or later.
- No shared secret is built into the app. Each device holds its own token in Credential Manager, scoped by canonical server origin and persisted for this machine only. Only the server origin is saved in the registry (`HKCU\Software\Okanet\Briareus`).
- Saved responses live under `%LOCALAPPDATA%\Okanet\Briareus\Responses`, encrypted with EFS where the volume allows it. They are erased when the connection is forgotten, revoked, expired or replaced by another device token, and entries untouched for 30 days are dropped.
- Voice notes are sent to your server for transcription and nowhere else.
- No analytics or telemetry.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to build, test and send a change, and [SECURITY.md](SECURITY.md) for reporting a vulnerability. Every pull request is built and tested with GCC and MSVC by the CI workflow, and every pull request merged into `main` is published as a release with the next minor version. Licensed under the [MIT License](LICENSE).
