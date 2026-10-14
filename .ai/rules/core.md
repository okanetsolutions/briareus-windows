---
paths:
  - 'core/**'
---

# Core

- Keep UI headers, drawing, windows and screen state out of core modules. Existing platform adapters may use Windows APIs; do not move them into screens.
- Put domain behavior alongside its existing parser or model, with declarations in the owning header. Do not duplicate JSON traversal, permission decisions or filtering in screens.
- Reuse `Str`, `Json`, model parsers, and the existing allocation helpers. Document whether a return value is borrowed or owned, and pair allocations with the module's cleanup function.
- Use existing semantic predicates such as `api_error_unauthorized` and `api_error_is_refusal` when they express the decision. Do not add a predicate for every C enum case.
- Keep `if` conditions within three lines and five logical operands. Function-call arguments count as part of one operand. Extract a domain-named predicate or use early returns when the decision grows.
- Update behavior tests for changed core tokens, including public headers. Comments and whitespace are exempt. Generated app tokens and version resources are outside this gate.
