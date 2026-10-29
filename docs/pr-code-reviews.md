# Stored pull request reviews

The Findings and Code reviews detail tabs share the read-only `code_reviews` operation,
`GET /api/v1/pulls/{number}/code-reviews?repo=owner/name`. They require project read
access and the advertised route, without provider admin access or a surviving session.
An older server shows an unsupported notice instead of invoking retired triage APIs.

The backend source of this contract is `nadinyamaui/briareus` at
`59ca50917664ab3ef195ef3b98a665005a0214e0`: `lib/api-v1-catalog.js`,
`lib/code-reviews.js` (`publicReview`), `lib/db.js` (`listPullCodeReviews`,
`rowToCodeReview`, `rowToCodeReviewFinding`), and `docs/api-v1-reference.md`.
The public fields are `summary` and `body`; database column names are not wire fields.

Both tables cover the latest 20 stored reviews, newest first, preserving finding
registration order within each review. Totals include historical duplicates and
all verdicts, including dismissed findings. The reviewed SHA and its relationship
to the currently loaded PR head accompany each row. An absent SHA is unknown.
A different SHA is shown as an older head; the client does not attempt ancestry analysis.

A verification verdict (`shouldFix`) is not a user's triage decision or evidence
that code was fixed. Null verdicts and verification are pending only while a review
is running; completed or failed reviews show no verdict / not verified, retaining
inconclusive rationale. Only running findings count as pending verdicts. Null token and
cost fields remain unknown; partial cost is marked only when a cost value exists.
Empty results, malformed payloads, unsupported capability and HTTP failures have
distinct states. A failed refresh labels the retained snapshot and its totals stale.

Selecting a row shows stored Markdown, rationale and review metadata below the
compact table. The Code reviews details include the summary and every finding.
Up/Down and Home/End move selection; native document scrolling, selection and
copying apply. Rows use stable record IDs, and replaced findings lose selection.
Small panes use fewer columns, with horizontal scrolling below the minimum table
width. Full titles and locations remain available in tooltips and selected details.

Reads coalesce across tabs. Either visible review tab refreshes at most once per
30 seconds on the existing screen poller, including empty or completed-only lists
that can gain reviews started elsewhere, honoring Retry-After. F5 refreshes the
shared read. Hiding/destroying the owner cancels requests; connection, device,
permission, repository and capability changes clear data and reject stale replies.

The current backend's `lib/actions.js` advertises no single `binary` for review,
and `lib/jobs.js` accepts Claude or Codex. The client continues to honor an
advertised binary when present. Its project-default confirmation supports either
of those advertised review binaries, including Codex; it never infers CLI identity
from provider labels. This change adds no review trigger or write endpoint.

## Verification

`tests/fixtures/code_reviews.h` contains synthetic public records only, including
deleted session IDs, null usage and verdicts, running/completed/failed reviews, historical
heads, Markdown, rationale and line ranges. Core and app suites cover parsing,
route encoding, shared reads, errors, unsupported capability, stable selection,
replacement IDs, stale scopes, polling, retry deadlines, cancellation, hit targets
and narrow/DPI fallback. No real reviews or paid requests are used.

Synthetic previews use the existing native screen fixture with a temporary runner,
Wine and Xvfb, with screen polling stopped for static captures. The baseline
preview uses `origin/main`'s pull screen and the retired schema; after previews use
the committed fixture above. Wine substitutes fonts and lacks some Segoe icon
glyphs. These previews cannot prove native Windows font metrics or accessibility;
Windows CI remains authoritative for Windows execution, MSVC and memory checks.
No new Win32 APIs are introduced, so no new Switch shim declarations are needed.
