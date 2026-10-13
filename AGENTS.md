# Agent guide

Read [CLAUDE.md](CLAUDE.md) and the matching entries in [.ai/rules/index.md](.ai/rules/index.md) before editing. Search `.ai/rules` for relevant concepts when a change crosses paths.

Repository skills live in `.agents/skills/`. Claude entrypoints in `.claude/skills/` refer to the same instructions.

- Use [briareus-architecture](.agents/skills/briareus-architecture/SKILL.md) when locating code, choosing ownership, or adding a feature.
- Use [testing-best-practices](.agents/skills/testing-best-practices/SKILL.md) when adding or reviewing tests.
- Use [code-review](.agents/skills/code-review/SKILL.md) before opening a PR, or when a review is requested.

Run `make rules` before delivery. It compares the working tree, including untracked files, with the merge base of `HEAD` and `origin/main`. For a stacked branch, use `make rules RULES_BASE=<parent-ref>` as well. Missing refs and Git failures are errors, never skipped passes.

The checks prevent new layer violations, overly complex `if` conditions, growth of presentation files over 300 lines, new warning suppressions, and reductions of the core coverage floor. Core logic changes require a meaningful C test change. Existing debt may stay or shrink; do not widen exclusions to pass. See [the check reference](.ai/rules/checks.md) for exact scope and limitations.

Run the affected implementation tests and required build checks. Report checks that could not run. A cross-compile does not prove Windows tests passed. Do not start separate QA workers unless the user requests them.
