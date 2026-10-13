---
paths:
  - 'core/api.*'
  - 'core/credentials.*'
  - 'core/cache.*'
  - 'core/update.*'
  - 'app/store.*'
  - 'app/ws.*'
---

# Boundaries

- The server API source is `nadinyamaui/briareus`; compare its `docs/api-v1-reference.md` with `core/api.c` before inventing a route or response shape.
- Add API calls to the route table and test method, encoded path, query/body arguments and failure behavior with `api_client_set_transport`. Preserve capability and device permission checks.
- Never retry a write automatically. A timeout can mean the operation succeeded; preserve the unknown-outcome UI rather than reporting a definite refusal.
- Keep device tokens in Credential Manager, scoped by canonical origin. Forgetting a connection removes local credentials and response caches; it does not revoke server tokens or stop agents.
- Preserve redirect refusal and credential host restrictions. Update downloads and WebSocket upgrades have their own transport rules; they are not reasons to add direct HTTP to a screen.
- A Projects board read immediately after a move may be stale. Preserve locally accepted moves while server indexing catches up, as `app/project_board.c` already does.
