# Pull-list freshness and cooldowns (#121)

Contract inspected in **nadinyamaui/briareus** core PR #123, still open at
`97dbf24e79713928471d7d1f9305b133af480660` (base
`f0ccdf6fda9c06be41db0b5a2ff7eab018306edf`):
`docs/api-v1-reference.md`, `lib/api-v1-catalog.js`, `lib/prboard.js`, and
`server.js`, including the board's issue pagination and error response.

Existing registrations cover `GET /pulls` and `GET /issues/{number}`; no new
routes or permissions are needed. Reads use the deployed OpenAPI catalog and
the token's read permission through `store_supports`.

The board displays the response's `syncedAt`, including when reopened from disk.
The server caches a board for two minutes and may serve `fresh=1` from a cache
under 15 seconds old; completing refresh does not imply a new GitHub query.
Older servers without `syncedAt` use the successful receipt time, saved alongside
the payload as `_receivedAt`; old disk entries without either timestamp show no
invented sync time. A 400 rejection of `fresh` retries without that argument.

Retry-After becomes an absolute deadline saved separately from successful board
data, scoped by server, device and repository, and retained in memory even if startup
verification invalidates the response cache. Manual refresh, activation,
visibility changes and reopening the board or issue screen all consult it.
Absent Retry-After, 429 uses a conservative one-minute retry delay and other
failures back off at 2, 4, 8, 16, 32 and 60 seconds. The shared per-repository
failure count survives deadline expiry and navigation and resets on a successful
list read; optional ISO `retryAt` is not consumed. Last successful
rows and freshness survive errors. The header shows the local retry clock time.
Session/action reads retain their 45-second cadence; their timers never inherit
the pull-list delay. An expired deadline permits the next refresh or timer read.
Board and issue list requests already in flight are retained rather than repeatedly replaced by F5;
pull-detail refresh and write reconciliation invalidate older row requests before
starting a replacement through the same cooldown gate;
screen hiding cancels delivery, and completions from a replaced client or a route
no longer allowed are ignored.

An issue absent from `issuesTruncated: true` or an `issuesError` list remains
accessible. Its own detail read establishes its state when available; on older
servers without that route, the supplied row stays visible without claiming it
has disappeared. Read failures leave successful detail intact.

Regression tests in `tests/app_pulls_tests.c` drive screen refresh, timer,
visibility, activation, reopen and request completion paths with a stub transport.
They are Windows automated tests, not deployed-provider or interactive UI QA.

## Pending release gates

- Recheck and pin the final merged core contract; this open dependency was not altered or merged.
- Verify the deployed OpenAPI catalog and token permissions against the actual target server.
- Perform native interactive Windows QA with live GitHub: cached sync time, real rate limits, recovery, and an issue beyond the first 100.
- Complete the epic #129 provider/deployment checks; simulated replies do not validate them.
