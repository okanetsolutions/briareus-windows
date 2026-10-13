# Briareus for Windows

This repository is the native Windows client for Briareus. It uses C11, Win32, and one C++17 drawing file, `app/canvas.cpp`, because DirectWrite's SDK interface requires C++. The only third-party library is the pinned miniaudio download. Keep the executable statically linked and avoid new dependencies.

## Structure

Dependencies flow from screens through `app/store.c` to `core/api.c` and its transport. Domain parsing, filtering and state calculations belong in `core/`; window state, drawing and controls belong in `app/`. The core already has Windows adapters for HTTP, credentials, caching and updates, so "no UI" does not mean "no Windows headers".

Screens orchestrate existing helpers. Extract reusable behavior into the owning domain module rather than adding generic service/repository layers. Reuse existing models, enums and predicates. Prefer a named predicate or early returns when a condition takes more than three lines or five logical operands. Ordinary C enum comparisons and exhaustive switches remain appropriate.

Read [.ai/rules/index.md](.ai/rules/index.md) for rules selected by path, and [the architecture skill](.agents/skills/briareus-architecture/SKILL.md) for the module map and request lifecycle.

## Contracts and boundaries

- `/api/v1` route definitions belong in `core/api.c`'s route table; screens submit named operations with `store_call` and check capability through `store_supports`.
- Preserve HTTPS validation, redirect refusal, per-origin credential storage, and the rule that writes are never automatically retried.
- Keep strings UTF-8 internally, converting to UTF-16 at Windows boundaries. State ownership and matching cleanup in public headers.
- Shared visual tokens come from `okanetsolutions/briareus-contracts`. Regenerate or copy `dist/c/briareus_tokens.h`; do not hand-edit `app/briareus_tokens.h`.
- `okanetsolutions/briareus-switch` compiles `core/` and `app/` unchanged through a Win32 shim. New Windows APIs or modules need a corresponding shim declaration, implementation or replacement there. Identify that follow-up when it is outside the authorized change.

## Verification

`make rules` runs the repository structure checks and their own regression tests using Python 3 and Git. Python is a tooling dependency only; normal app builds do not need it. The check runs separately in CI on Linux, while the existing Windows jobs retain their builds, tests, sanitizers and coverage checks.

For C/C++ behavior changes, add or update meaningful tests in the owning suite. Copy, styling, static layout and documentation changes do not need new behavior tests. The automated test-delta check covers core C/header token changes; app test coverage and presentation exemptions are assessed in review. Do not add dummy test edits to satisfy the check.

Use `make WERROR=1` on MinGW, or `build.bat` from a Visual Studio Developer Command Prompt. `make test` runs both test executables. `make lint`, `make coverage` and `make sanitize` provide the additional checks described in [CONTRIBUTING.md](CONTRIBUTING.md). Do not lower coverage or disable checks to pass a change.

Keep changes within the task. Follow nearby naming and formatting; comments explain ownership or a non-obvious constraint, rather than restating code. Keep human-facing prose concise and avoid dashes as sentence connectors.
