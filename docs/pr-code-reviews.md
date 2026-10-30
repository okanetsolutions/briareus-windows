# Stored pull request reviews

The Code reviews detail tab uses the read-only `code_reviews` operation,
`GET /api/v1/pulls/{number}/code-reviews?repo=owner/name`. It requires project read
access and the advertised route, without provider admin access or a surviving session.
An older server shows an unsupported notice instead of invoking retired triage APIs.

The backend source of this contract is `nadinyamaui/briareus` at
`59ca50917664ab3ef195ef3b98a665005a0214e0`: `lib/api-v1-catalog.js`,
`lib/code-reviews.js` (`publicReview`), `lib/db.js` (`listPullCodeReviews`,
`rowToCodeReview`, `rowToCodeReviewFinding`), and `docs/api-v1-reference.md`.
The public fields are `summary` and `body`; database column names are not wire fields.

The table covers the latest 20 stored reviews, newest first; an opened review lists
its findings in registration order. Totals include historical duplicates and
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

The separate Findings tab is retired: its ID (6) falls back to PR Body like the retired
Commits and Checks IDs, and the default PR Body selection is unchanged. Code reviews keeps
a full-width overview table with visible per-review input/output tokens and cost. Its
footer sums each displayed review once, scopes totals to the latest 20 stored reviews,
and includes historical/dismissed findings, whose severity and verdict breakdown is the
findings total's tooltip. Known zero is zero; all-missing usage is Unknown, while mixed totals show
the known sum with the missing-review count and a partial label. Estimated costs and
unpriced turns remain visible; exact usage is available in cost tooltips and metadata.

Each table row ends in a chevron and opens that review's findings in place of the
table: an "All reviews" back button with the review number and status, its commit,
provider, model and start, tiles for findings, tokens and cost, and a folded review
summary with exact usage and metadata. The review's findings follow as a table of their
own, with semantic severity/verdict badges. A review without findings shows its summary
instead, and keeps it unfolded when its first findings arrive; otherwise each review's
summary starts folded. A persistent accent outline identifies selection in both tables;
hover only changes the fill. On wide panes a balanced findings table and independently
scrollable inspector share the viewport and end with the longer of the two; on narrow
panes a table only as tall as its rows, up to 240px, stacks above the detail. The
selected title, source with intact line range, body and rationale precede registration
times. Full paths and titles remain copyable and available in tips. Only the selected
finding's body and rationale are laid out, so Ctrl+F and copying reach the other findings
through their titles in the table.

The selected review row and the opened review's finding stay stable across refreshes.
A removed finding selects the review's first; a review no longer stored returns to the
table, which selects the newest. Reopening a review keeps the finding last read in it.
Up/Down and Home/End select rows in either table with minimal scrolling, and Enter opens
the selected review. The back button, the pane header's back button, the pane's back keys
(Backspace, and Escape once no text is selected) and the open Code reviews tab return to
the table where it was scrolled; with the table showing, they leave the pull request as
before. Screens opt into this through the pane's `back` hook, which the side panel's
close button skips. Other tabs keep an opened review in place, and a review closed by a
refresh on another tab leaves that tab's scrolling alone. PageUp/PageDown scroll the wide
inspector. Native document text selection, Markdown links, code wrapping and copying apply.
Narrow Code reviews panes scroll the table horizontally to preserve readable usage columns.

The table and opened reviews share one read. The visible Code reviews tab refreshes at
most once per 30 seconds on the existing screen poller, including empty or completed-only
lists that can gain reviews started elsewhere, honoring Retry-After. F5 refreshes the
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
route encoding, shared reads, errors, unsupported capability, opening and leaving a
review by click, key, tab and the pane's back button with the table's scroll restored,
stable selection, replacement IDs, removed reviews, stale scopes, polling, retry
deadlines, cancellation, hit targets and narrow/DPI fallback.
No real reviews or paid requests are used.

`scripts/preview_pull_reviews.sh` builds the synthetic native screen harness and captures
the Code reviews table and an opened review at 1652px, 390px and 192-DPI/780px with Wine
and Xvfb; set DISPLAY, WINEPREFIX and optionally BUILD first. PREVIEW_REVIEW opens a
review by ID and PREVIEW_SUMMARY unfolds its summary. The harness stops polling and
uses only fake transport responses, including nine findings and four reviews. It can
run the focused suite with PREVIEW_TESTS=1. The optional BRIAREUS_WINE_PREVIEW_TIME
flag supplies a harness-only UTC fallback for Wine 9's unimplemented existing time-zone
API; it is never linked into the app or Windows CI tests. PREVIEW_WINE_TIME=0 disables
that workaround. Wine substitutes fonts and lacks some Segoe icon glyphs, so these
renders do not establish native Windows font metrics, accessibility, or Windows test
results. Windows CI remains authoritative for both toolchains and memory checks.
No new production Win32 APIs or Switch shim declarations are needed.
