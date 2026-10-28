# Session Mail wake

The conversation's ✉ Mail wake button opens the session's admin-only rules editor beside Webhook. Its marker comes from enabled rules in the session record's `mailWake`.

The editor reads `GET /sessions/{id}/mail-wake`, `GET /settings/mail/accounts`, and `GET /settings/projects` separately. Only the project settings list carries `mailToolsEnabled`. A receiving mailbox must be connected, enabled, and have an available provider; read-only provider access is sufficient. Disabled rules keep unavailable mailbox ids and saved filters.

Save replaces at most 20 rules through `PUT /sessions/{id}/mail-wake` with `{rules, perHour, maxTurns}`. Each rule has `account`, `enabled`, `to`, `from`, `subject`, and its existing optional `id`. Server bookkeeping such as `since` is excluded. Existing ids preserve activation times for unchanged rules. To/Cc and From are single address filters, Subject is a case-insensitive substring; each is limited to 320 UTF-16 units. Wakes/hour is 1–600, Turns/row is 1–1000. Zero is invalid here, unlike Webhook's unlimited turns setting.

Read failures stay visible independently and do not replace edited drafts. A failed save retains the draft. Ambiguous or cancelled writes interlock further saves, including a newly opened editor, until the original request ends and the user refreshes, reviews the saved record, and explicitly discards the draft in favor of that record. Writes never retry automatically. Retry-After deadlines are kept per device and route in connection memory across refresh, hide and reopen. No draft or cooldown is written to disk. Responses cannot retarget a screen to another session, project, device, or connection; cancelled finalizers carry their own metadata and never access destroyed screens.

Back, root replacement and window close consult the existing unsaved-change callbacks. Edits use the existing native controls, clipping, theme and tokens with Tab/Shift+Tab, Ctrl+A, Ctrl+S, Enter and Escape. No new Win32 APIs or dependencies are introduced for the Switch consumer.

Contract references inspected:

- [Swift #131](https://github.com/okanetsolutions/briareus-swift/pull/131), commit `15feb40261e8dcf935a26298b23f863236933b7f`, including MailWakeLogic, model, view and tests.
- [Core #159](https://github.com/nadinyamaui/briareus/pull/159), commit `6fab853074aaf037bb5bc752e16b4627b03d909d`, including `lib/mail-wake.js`, the API catalog and API v1 reference. Both routes return the direct record and require admin access.

All implementation tests use fake transports. They do not enable live rules, wake sessions, send mail, change settings, or start provider consent. Windows CI is the execution authority; local MinGW cross-compilation alone does not run the tests.
