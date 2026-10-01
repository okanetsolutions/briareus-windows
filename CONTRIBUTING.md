# Contributing

Thanks for helping with Briareus for Windows. This page covers how to build, test and send a change.

## Building

The app is C11 on the Win32 API with no third-party dependencies. Either toolchain works:

- **MinGW-w64 GCC** (for example [WinLibs](https://winlibs.com) or MSYS2's UCRT64): `mingw32-make` builds `build\Briareus.exe` and runs the core tests; `mingw32-make app` builds only the app.
- **Visual Studio Build Tools**: from a Developer Command Prompt, `build.bat`.

## Testing

```sh
mingw32-make test
```

Everything under `core/` (JSON, models, API client, cache, diff, Markdown, board logic) has no UI code and is covered by `tests/core_tests.c`, with HTTP stubbed through the client's pluggable transport. New core behaviour needs a test there. UI changes in `app/` are checked by running the app against a Briareus server; a screenshot in the pull request helps.

The same build and tests run in CI for every pull request, with both GCC and MSVC.

## Layout

| Path | Contents |
| --- | --- |
| `core/` | Portable logic, no UI. |
| `app/` | The Win32 app: theme, layout toolkit (`doc.c`), screen stack (`pane.c`), store, screens, dialogs, voice notes. |
| `tests/` | Core tests. |
| `res/` | Icon, manifest, dialogs, version resources. |

## Style

- Keep the core free of UI and the app free of protocol details; the store is the seam between them.
- Follow the file you are in: 4-space indent, braces on the same line, short comments that say why.
- No new dependencies. The executable stays statically linked with nothing to install.
- Match the dashboard: labels, colours, type and behaviour follow the web app where they overlap.

## Sending a change

1. Fork and branch from `main`.
2. Make the change with its tests.
3. Open a pull request describing what changed and why, and how you checked it. CI must pass before it is merged.

Releases are cut by pushing a `v*` tag; the release workflow builds, strips and publishes the executable.

## License

Briareus for Windows is released under the [MIT License](LICENSE). By sending a pull request you agree that your contribution is licensed under the same terms.
