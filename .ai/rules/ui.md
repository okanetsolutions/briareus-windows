---
paths:
  - 'app/screen_*.c'
  - 'app/project_*.c'
  - 'app/forge_site*.c'
  - 'app/theme.*'
  - 'app/doc.*'
  - 'app/canvas.*'
---

# UI

- Match the shared dashboard behavior and `briareus-contracts` tokens. Use theme, `Doc` and canvas helpers for colors, typography, spacing and interaction.
- Keep layout callbacks concerned with composing the current view. Move reusable parsing and domain decisions into core, and reusable drawing into existing app helpers.
- Preserve capability-dependent actions, loading, empty and error states, keyboard navigation, DPI scaling and unsaved-change checks.
- Static copy, color and layout changes do not need tests that pin wording, coordinates or component order. Changed permissions, actions, data flow and request ownership need meaningful behavior coverage where the harness can exercise them.
- New `screen_*.c`, `project_*.c` and `forge_site*.c` presentation files have a 300-line ceiling. Larger existing files must not grow; extract cohesive behavior rather than compressing lines or renaming a file to bypass the check.
