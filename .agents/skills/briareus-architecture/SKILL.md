---
name: briareus-architecture
description: Locate Briareus Windows code and choose module ownership when adding a feature, changing request lifecycles, or reviewing core/app boundaries.
---

# Briareus architecture

Read `CLAUDE.md` and `.ai/rules/index.md` first. This map helps locate the existing implementation; confirm symbols and ownership in the headers before editing.

| Responsibility | Owner |
| --- | --- |
| JSON and UTF-8 strings | `core/json.*`, `core/str.*` |
| Domain models and server capability records | `core/models.*` |
| Route catalog, HTTP validation, request encoding and transport | `core/api.*` |
| Origin-scoped tokens and saved responses | `core/credentials.*`, `core/cache.*` |
| Board, repository files/indexing, diff and Markdown | `core/board.*`, `core/repo*`, `core/diff.*`, `core/markdown.*`, `core/highlight.c` |
| Usage comparisons and pricing coverage | `core/usage.*` |
| Pull conversation roots, review association and timeline ordering | `core/pull_conversation.*` |
| Mail thread grouping and notification classification | `core/mail_group.*` |
| Mail, MCP, browser, meeting, terminal and SFTP logic | Corresponding `core/` modules |
| Release parsing, download checks and executable swap | `core/update.*`; app scheduling and feedback in `app/updater.*` |
| Connection state, request ownership and UI-thread completion | `app/store.*` |
| Window and screen lifecycle | `app/main.c`, `app/pane.*`, `app/detached.c`, `app/screens.h` |
| Layout, drawing and shared visual state | `app/doc.*`, `app/canvas.*`, `app/theme.*`, generated `app/briareus_tokens.h` |
| Screens and project tabs | `app/screen_*.c`, `app/project_*.c`, focused app helpers |
| WebSocket transport | `app/ws.*` |
| Audio and attachments | `app/voice.*`, `app/meet_audio.*`, `app/loopback.*`, `app/attach*`, `app/miniaudio.c` |
| Crash handling and reports | `app/crash.*`, `tests/crash_probe.c`, `tests/crash_smoke.ps1` |

## Follow one request

1. A screen checks `store_supports(operation)` and submits arguments through `store_call`, retaining its pending `Request *` slot.
2. Store checks permissions and retains the client. Its worker calls `api_call` with a named operation from the route table.
3. Core validates and encodes the request, calls its replaceable transport, and returns JSON or a typed `ApiError`.
4. Store delivers completion on the UI thread. The screen updates owned state and invalidates layout as needed.
5. Screen destruction calls `request_cancel` on pending slots before freeing the owner; store discards cancelled results.

Use the related store helpers for uploads, transcription and streaming. `store_stream_new` checks capability on the UI thread and retains the connection; `store_stream_read` runs on the caller's worker and `store_stream_cancel` may stop it from another thread. Free the stream only after its read has returned. Check their ownership contracts in `app/store.h`; they do not all take identical argument ownership.

## Choose the change location

Search callers and sibling modules before adding another file. Keep portable decisions independently callable in core, and keep controls and asynchronous view ownership in app. Screen families now have focused controller, request, view and control components, sharing only family-private declarations through `*_internal.h`; keep drawing payload types local when one component owns them. A helper used once can stay local unless it belongs to an existing shared domain module. Do not add generic client/connector classes to imitate the PHP source repository.

`Makefile` discovers core/app C sources and tests by wildcard, while `build.bat` compiles the same groups explicitly with wildcards. `canvas.cpp` is the only C++ file wired into the MSVC build. Add new test suites to `tests/suites.h` and the corresponding runner. Changes to Windows API usage also affect the Switch shim described in `CLAUDE.md`.
