---
paths:
  - 'app/**'
---

# App

- Screens call `store_call`, `store_transcribe` or `store_upload`; transport and protocol implementation remain behind the store. `app/ws.c` owns its WebSocket transport.
- Use the owning domain helper for parsing, filtering and decisions. Keep screen structs focused on the current view, controls, pending requests and selection.
- Cancel pending request slots during destruction before freeing their owner. Completion callbacks run on the UI thread; background work must not mutate controls.
- Reuse `ScreenVTable`, `Pane`, `Doc`, canvas, theme and shared screen helpers. Add a focused domain module when an existing screen needs to grow beyond 300 lines; existing larger presentation files may shrink or stay the same size.
- Keep `if` conditions within three lines and five logical operands. Prefer existing semantic predicates, while retaining normal C enum switches and comparisons.
- Process-lifetime caches allocate through `xmalloc_kept` and release through `xfree_kept`; ordinary allocations still need normal cleanup.
- Check the Switch shim impact before introducing another Windows API, header or Windows-only module.
