---
paths:
  - 'tests/**'
---

# Tests

Read [testing-best-practices](../../.agents/skills/testing-best-practices/SKILL.md) before writing or reviewing tests.

- Test through the public function and assert literal expected results, state changes or recorded transport arguments. Avoid implementation-text assertions.
- Reuse `tests/test.h` and the owning suite. New suites need a declaration in `tests/suites.h` and a call from the matching test program's `main`.
- Stub external transports instead of calling live servers. Free fixtures, results, errors and retained clients so the debug CRT leak job stays meaningful.
- Do not pin UI or prompt wording. Technical identifiers, serialized contracts, parser output and secret-leak checks are valid assertions.
- Run the affected executable; do not add a separate QA worker. CI retains required Windows builds, sanitizer runs and coverage.
