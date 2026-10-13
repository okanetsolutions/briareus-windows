# Contributing

Thanks for helping with Briareus for Windows. This page covers how to build, test and send a change.

## Building

The app is C11 on the Win32 API; its one third-party library is miniaudio, for the meeting assistant's audio: `make` and `build.bat` download the version pinned in the Makefile into `third_party/` (not committed), check its SHA-256, and compile it through `app/miniaudio.c`. The first build needs `curl` and a connection. It draws with Direct2D and DirectWrite through `app/canvas.cpp`, the one C++ file, kept to C style, since the Windows SDK declares DirectWrite for C++ only. Either toolchain works:

- **MinGW-w64 GCC** (for example [WinLibs](https://winlibs.com) or MSYS2's UCRT64): `mingw32-make` builds `build\Briareus.exe` and runs the core tests; `mingw32-make app` builds only the app.
- **Visual Studio Build Tools**: from a Developer Command Prompt, `build.bat`.

## Testing

```sh
mingw32-make test
```

Core behavior is covered by `tests/core_*tests.c`, called from `tests/core_tests.c`, with HTTP stubbed through the client's pluggable transport. App helpers are covered by `tests/app_*tests.c`, called from `tests/app_tests.c`. New behavior needs meaningful coverage in the owning suite; static copy, styling and layout changes need no new tests. Read [the testing skill](.agents/skills/testing-best-practices/SKILL.md) for harness registration, assertions and cleanup.

Run `make rules` before delivery. This uses Python 3 and Git to check new structure violations and core changes missing a test delta, without requiring a Windows compiler. Use `make rules RULES_BASE=<parent-ref>` for a stacked branch. The checks include their own regression tests and run in a separate Linux CI job. Normal app builds do not require Python. See [the check reference](.ai/rules/checks.md) for limits and source provenance.

The same build and tests run in CI for every pull request, with both GCC and MSVC, and every check must pass:

- **Warnings are errors.** GCC builds with `WERROR=1` and MSVC with `/W4 /WX`. Run `mingw32-make WERROR=1` before pushing.
- **Coverage.** `mingw32-make coverage` (needs `pip install gcovr`) measures line coverage of `core/` and fails below the floor set by `COVERAGE_MIN` in the Makefile; the HTML report lands in `build-cov/coverage/index.html`. Raise the floor when coverage goes up, never lower it to make a change pass.
- **AddressSanitizer.** The tests also run under MSVC's AddressSanitizer, which stops on buffer overruns, use after free and double frees.
- **Leaks.** CI runs the tests once more against MSVC's debug CRT (`set CLOPT=/Od /MTd /RTC1 /Zi` before `build.bat`): a test that leaves an allocation behind fails with a dump of the blocks, and `/RTC1` stops on reads of uninitialised locals and overrun stack frames. Free what a test allocates; a cache that keeps memory on purpose allocates it with `xmalloc_kept`.
- **Undefined behaviour.** `mingw32-make sanitize` runs the tests under UBSan in trap mode, with `_FORTIFY_SOURCE=2` and the stack protector. A trap prints `CRASH in <test>`; rebuild that test under gdb to see the line.
- **Static analysis.** `mingw32-make lint` runs cppcheck. Silence a false positive on its line with `// cppcheck-suppress <id>`, saying why.
- **Formatting.** Files follow `.editorconfig` (LF, final newline, no trailing spaces), checked by editorconfig-checker.

## Layout

| Path | Contents |
| --- | --- |
| `core/` | Portable logic, no UI. |
| `app/` | The Win32 app: drawing (`canvas.cpp`), theme, layout toolkit (`doc.c`), screen stack (`pane.c`), store, screens, dialogs, voice notes. |
| `tests/` | Core and app harness suites, contract fixtures, crash probes and structure-check regressions. |
| `res/` | Icon, manifest, dialogs, version resources. |

## Style

- Keep the core free of UI and the app free of protocol details; the store is the seam between them.
- Follow the file you are in: 4-space indent, braces on the same line, short comments that say why.
- No new dependencies. The executable stays statically linked with nothing to install.
- Match the dashboard: labels, colours, type and behaviour follow the web app where they overlap.

Agent entrypoints are [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md). Path-specific rules live in [.ai/rules](.ai/rules/index.md); architecture, testing and code-review skills live in `.agents/skills/`, with Claude entrypoints under `.claude/skills/`. They adapt heedly-builder's rule structure to this client's existing modules.

## Sending a change

1. Fork and branch from `main`.
2. Make the change with its tests.
3. Open a pull request describing what changed and why, and how you checked it. CI must pass before it is merged.

Every pull request merged into `main` is released: the release workflow bumps the minor version in `app/resource.h`, commits it as "Version resource x.y.z", tags `vx.y.z`, then builds, strips and publishes the executable. Pushing a `v*` tag by hand still publishes that tag as it is.

## License

Briareus for Windows is released under the [MIT License](LICENSE). By sending a pull request you agree that your contribution is licensed under the same terms.
