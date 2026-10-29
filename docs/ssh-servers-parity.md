# Global SSH servers

The icon-only **SSH servers** footer button beside Settings, or Ctrl+Shift+H, opens the global page when `settings_ssh_servers` is available to the current token. The existing API catalog and permission gate still apply. The contract is `GET /api/v1/settings/ssh/servers`, returning `{ servers, defaults }`, requiring admin; no new endpoint was added.

Folders follow the current sidebar project order, then unlisted repositories alphabetically, with **No project** last. Servers retain registry order within each folder. The global page has no search bar.

The page and project tabs share the existing terminal registry and registered-server identities. Existing sessions are selected rather than launched again, including closed sessions; Reconnect explicitly starts them again. Cross-project duplicate tab labels include their project/repository. Ctrl+Tab and Ctrl+Shift+Tab cycle all sessions on the global page and only the current project's sessions on project tabs.

Settings notifications and refresh cancel pending reads before starting replacements. Connection, device permission or project-scope changes cancel old reads, clear lists/folds, and retire local SSH sessions. Screen destruction cancels owned requests; cancelled completions cannot restore stale rows. Unmounted navigation candidates do not hide existing terminals. Password and approval management are unchanged; the list never reads database credentials, and terminal launches still use this PC's existing OpenSSH authentication.

Reference: Swift PR #141, merge `1cb62321489460b4c6d82cfabb70b45d1ced8133`, including its final model, folder, session-label and terminal-cycling fixes; current `Mac/App/Remote/SSHServersModel.swift`, `SSHServerFolders.swift`, `SSHServersScreen.swift`, `ProjectSSHTab.swift`, `Mac/Core/SSHServerLogic.swift` and `MacTests/SSHServerLogicTests.swift` were compared. Later Swift #155 preserves independent monitoring settings; no settings form writes are added here. Windows starts from main `62cdf08b` with #178, #179 and #181 already present.

Regression suites use replaceable HTTP transport and a fake terminal launcher. No test starts SSH, changes credentials, decides approvals, edits `.env`, or performs a live operation. Required Windows CI runs both toolchains, sanitizers, leak checks, coverage and the existing approval/password suites. The cross-build is compilation evidence only. No separate QA session or live terminal validation is part of this change. The native footer adds owner-drawn button and tooltip messages/flags; the Switch shim needs a compatibility check before its next submodule update.

SSH, Settings and Sign out use native footer buttons with accessible names,
tooltips and keyboard focus; Sign out keeps its confirmation and token cleanup.
At narrow widths the version moves above the action row. The shared SSH view adds
a DPI-scaled content inset above folders and session tabs while retaining the
viewport bottom for the terminal and independent folder scrolling.
