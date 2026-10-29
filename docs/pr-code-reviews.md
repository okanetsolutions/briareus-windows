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

Code reviews immediately precedes Findings in the tab strip; tab IDs and the default
PR Body selection are unchanged. Code reviews keeps a full-width overview table with
visible per-review input/output tokens and cost. Its footer sums each displayed review
once, scopes totals to the latest 20 stored reviews, and includes historical/dismissed
findings. Known zero is zero; all-missing usage is Unknown, while mixed totals show
the known sum with the missing-review count and a partial label. Estimated costs and
unpriced turns remain visible; exact usage is available in cost tooltips and metadata.

Findings groups table rows by reviewed commit, with semantic severity/verdict badges.
A persistent accent outline identifies selection; hover only changes the fill. The old
orange hover outline could appear on a different row from the selected detail and was
not a data-selection mismatch. On wide panes a balanced table and independently
scrollable inspector share the viewport; on narrow panes a bounded table stacks above
the detail. The selected title, source with intact line range, body and rationale precede
secondary review metadata. Full paths and titles remain copyable and available in tips.

Each tab retains its own stable selection across refreshes. Removed records select a
remaining record; Up/Down and Home/End reveal the selected row with minimal scrolling.
The Code reviews report remains below its table and includes every stored finding.
PageUp/PageDown scroll the wide inspector; arrow keys continue to select rows.
Native document text selection, Markdown links, code wrapping and copying apply.
Narrow Code reviews panes scroll horizontally to preserve readable usage columns.

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

`scripts/preview_pull_reviews.sh` builds the synthetic native screen harness and captures
Findings and Code reviews at 1652px, 390px and 192-DPI/780px with Wine and Xvfb;
set DISPLAY, WINEPREFIX and optionally BUILD first. The harness stops polling and
uses only fake transport responses, including nine findings and four reviews. It can
run the focused suite with PREVIEW_TESTS=1. The optional BRIAREUS_WINE_PREVIEW_TIME
flag supplies a harness-only UTC fallback for Wine 9's unimplemented existing time-zone
API; it is never linked into the app or Windows CI tests. PREVIEW_WINE_TIME=0 disables
that workaround. Wine substitutes fonts and lacks some Segoe icon glyphs, so these
renders do not establish native Windows font metrics, accessibility, or Windows test
results. Windows CI remains authoritative for both toolchains and memory checks.
No new production Win32 APIs or Switch shim declarations are needed.
