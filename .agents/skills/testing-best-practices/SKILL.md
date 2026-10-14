---
name: testing-best-practices
description: Design or review Briareus C harness tests and structure-check regressions, including transport stubs, request ownership, permissions, contracts and memory cleanup.
---

# Testing best practices

Read the owning implementation and nearby tests before selecting coverage. Extend the smallest existing suite that exercises the changed decision.

- Call public helpers and assert their output or state transition against literal expected values. Do not mirror the implementation to calculate the expected result.
- Use `CHECK`, `CHECK_INT`, `CHECK_STR` and `CHECK_OWNED_STR` from `tests/test.h`, and register each case with `test_run` in the owning suite.
- Core suites are `tests/core_*tests.c`, called by `tests/core_tests.c`; app suites are `tests/app_*tests.c`, called by `tests/app_tests.c`. New suites need declarations in `tests/suites.h` and runner calls. App tests link the app without opening a window.
- Stub HTTP with `api_client_set_transport`. Assert method, encoded path, query/body, capability or permission decisions, and applicable failure cases. Never depend on a live server.
- Cover empty or malformed external data, rejected permissions, cancellation and uncertain write outcomes when the change touches them. Select cases that detect distinct defects.
- Free JSON, strings, model arrays, errors and clients with the owning API. The debug CRT detects leaks per test; ordinary fixture leaks are not process caches.
- Avoid tests that pin wording, layout coordinates, private functions or source text. Contract identifiers, parser output and negative secret-leak assertions remain valid.
- Static copy and layout changes need no new tests. Core token changes need a meaningful changed suite under `make rules`; that file-delta check does not prove coverage.

Run the affected executable after changing C tests. The harness runs suites through its two executables and has no test-name filter. Use `make BUILD=<directory> <directory>/core_tests.exe` or the app equivalent to build a targeted executable, then run it on Windows. Required CI still runs both toolchains and memory checks. Report a build-only check accurately when the host cannot execute Windows binaries.

For tooling behavior, use the Python standard-library tests in `tests/test_structure.py`. Exercise actual temporary Git repositories and assert exit status and diagnostics; do not replace Git calls with canned success responses.
