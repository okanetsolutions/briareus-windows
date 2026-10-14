# Structure checks and source adaptation

Adapted from [heedly-builder](https://github.com/okanetsolutions/heedly-builder/tree/7dc3df7765dca5efbe9b5d3dc139c3c21b516600), inspected at commit `7dc3df7765dca5efbe9b5d3dc139c3c21b516600`.

| Heedly-builder mechanism | Windows equivalent |
| --- | --- |
| `AGENTS.md`, `CLAUDE.md`, `.ai/rules/index.md` | Shared entrypoints and path-specific rules for core, app, UI, boundaries and tests |
| Architecture, testing and review skills | Repository skills under `.agents/skills/`, with Claude entrypoints |
| Presentation calls services, services call clients/connectors | Screens call store, store calls core API/transport; domain logic stays in core |
| Architecture tests | New core-to-app includes and UI imports, direct API calls outside store, and WinHTTP outside its transport adapters fail |
| Presentation files over 300 lines cannot grow | Same limit for `app/screen_*.c`, `app/project_*.c` and `app/forge_site*.c` |
| PHPStan limits `if` conditions to three lines and five operands | A C token check applies the same limits to new or changed conditions |
| Escape hatch checker | New source warning suppressions, new Makefile cppcheck suppressions, and lower `COVERAGE_MIN` fail |
| Test-delta checker | Core C/header token changes need a changed C test suite; app behavior coverage is reviewed |
| Enum predicates | Reuse existing semantic predicates; keep idiomatic C comparisons and switches |

Laravel packages, database migrations, tenancy, query budgets and package-specific skills have no equivalent in this client and are not copied.

## Run

```sh
make rules
make rules RULES_BASE=origin/main
make audit
python3 scripts/check_structure.py <parent-ref>
python3 -m unittest discover -s tests -p 'test_structure.py'
```

The checker requires Python 3 and Git, not Windows SDK headers. It resolves the merge base of `HEAD` and the supplied ref, defaulting to `origin/main`, then compares that snapshot with tracked and untracked working files. CI fetches full history and supplies the PR base SHA. Main pushes compare with their previous commit. Missing refs or Git errors fail with exit code 2; violations fail with exit code 1.

The change-based check compares findings per file and token expression against the base. Moving an unchanged condition within a file does not make it new; new copies do. An existing long condition cannot span more lines. Existing oversized files may stay the same size or shrink. Deleted files do not create structure findings, but core removals still need a test delta. No committed exemption list or suppression option is provided.

`make rules` also runs the full-tree audit used by `make audit`. It reports all existing layer, condition and presentation-size violations without baseline allowances. After the structural cleanup, both checks must pass; new presentation components retain the 300-line ceiling. Warning suppression and core test-delta checks still compare against the merge base.

The cleanup extraction is recorded in `scripts/refactors/presentation.json`. Run `python3 scripts/extract_presentation.py scripts/refactors/presentation.json --check` to reproduce it from its pinned source and explicit pointer-marker and Settings selection ownership fixes. This proves that migration only; later intentional changes to those components need normal behavior tests and `make rules`, not a rewritten extraction manifest.

## Limits and review

This is a lexical C/C++ check, not a compiler or a coverage measurement. It skips comments, string contents and preprocessor directives for conditions, and counts `&&`/`||` operands outside function calls, subscripts and braced initializers. It does not expand macros, infer types, inspect multiline inline expressions or prove that a changed test exercises the changed behavior. Review those aspects with the compiler checks and behavior tests.

The test-delta gate ignores comment/whitespace-only core changes and requires a changed, non-deleted `tests/core_*tests.c` suite. It deliberately does not guess whether arbitrary C UI code is static presentation. App behavior needs tests when they provide meaningful coverage; pure presentation changes remain exempt in review.

Warning suppression checks count source `cppcheck-suppress` comments and diagnostic-ignore pragmas per file, and Makefile `--suppress` options by value. Existing suppressions cannot increase in count. Review changes to CI, build flags, this checker and its tests for weakened enforcement, even when the script passes. Do not lower limits, add exclusions, hide logic in macros, or move code solely to bypass a check.
