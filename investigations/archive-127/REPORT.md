# Issue #127 feasibility decision

**NO-GO for #128 archive integration. Preserve the existing per-file indexer.**
The tested Windows-provided paths fail strict gzip completion/consumption, even
when combined with CRC/ISIZE and a strict tar preflight. No library, production
code, API route, UI integration, PR, comment, message or merge was introduced.
The only workflow added is restricted to this investigation branch.

## Contract and instructions

Read [#127](https://github.com/okanetsolutions/briareus-windows/issues/127),
[epic #120](https://github.com/okanetsolutions/briareus-windows/issues/120),
[dependent #128](https://github.com/okanetsolutions/briareus-windows/issues/128),
`CONTRIBUTING.md`, README platform requirements, `.editorconfig`, existing CI and
release workflows, and the index/transport code. No applicable `AGENTS.md` or
`CLAUDE.md` was present in this worktree or its ancestor directories.
`CONTRIBUTING.md` forbids new dependencies; the app minimum is Windows 10 1809.

Core [PR #122](https://github.com/nadinyamaui/briareus/pull/122) was **open and
unmerged**, with its current head still matching the observed head on the final
recheck: **`cb38ed74572bd759df5db2c0c3316737e524ccf8`**. Inspected source/docs:

- [lib/repofiles.js](https://github.com/nadinyamaui/briareus/blob/cb38ed74572bd759df5db2c0c3316737e524ccf8/lib/repofiles.js): exact ref validation, GitHub tarball fetch, 300 MiB limit, pipeline teardown and upstream AbortSignal.
- [server.js](https://github.com/nadinyamaui/briareus/blob/cb38ed74572bd759df5db2c0c3316737e524ccf8/server.js#L946): response-close cancellation before upstream headers, gzip content type, optional content length, connection destruction on stream error.
- [catalog](https://github.com/nadinyamaui/briareus/blob/cb38ed74572bd759df5db2c0c3316737e524ccf8/lib/api-v1-catalog.js#L1405) and [reference](https://github.com/nadinyamaui/briareus/blob/cb38ed74572bd759df5db2c0c3316737e524ccf8/docs/api-v1-reference.md#L464): `GET /api/v1/repo/archive?repo=&ref=`, project-scoped read permission, required ref, binary response, one top-level directory, 314,572,800-byte cap.

Send the canonical tree's full commit SHA, never the moving branch or the Git
tree-object SHA. A declared oversized body gets 413; an unknown-length oversized
body can be cut off **after HTTP 200**. EOF or HTTP success alone is insufficient.
Recheck the final merged contract before implementation/release.

## Tested implementation choice

The only plausible provided-tool design tested was a worker using built-in
Windows PowerShell/.NET `GZipStream`, a separately bounded tar file, and a strict
tar preflight, followed by generated numeric staging filenames. It is **rejected
as a production choice** because its gzip envelope validator is incomplete.
The executable remains unchanged and its archive capability must remain false.

OS `tar.exe` alone is also rejected: `-tf` returns success for invalid integrity
and incomplete tar end markers; `-tvf` is human-readable metadata, not a bounded
raw tar stream; `-xOf` emits file payloads, omitting headers/padding/skipped data.
`tar -xOf archive.tgz --format raw` is explicitly unsupported in extraction mode.
No direct `tar -xf` extraction or list-then-extract approach is approved.
[Microsoft documents](https://learn.microsoft.com/en-us/windows/tar/) that OS tar
is bsdtar-based; results below are from the actual OS executable, not MSYS2 tar.

The documented [Windows Compression API](https://learn.microsoft.com/en-us/windows/win32/cmpapi/using-the-compression-api)
provides MSZIP/XPRESS/LZMS, not a gzip reader; raw MSZIP has a 32 KiB block
contract. Treating it as a drop-in reader for GitHub gzip is unproven.
Core supplies `Content-Type: application/gzip`, not `Content-Encoding: gzip`,
so WinHTTP's HTTP content-decoding option is not an archive decoding solution.

## Reproduction and actual environments

Commands and code entry points are in [README.md](README.md). The test generator
uses Python/zlib only as fixture tools and an independent strict-gzip oracle;
neither is an app dependency. No package is installed in the Windows prototype.
`run.ps1` uses the running PowerShell executable and resolves OS tar through the
system directory, with a Sysnative correction for a 32-bit caller.

Registered SSH discovery returned `servers: []`; no SSH command was submitted.
The stock CI triggers only on PR/main, and release triggers on main/tags. Native
verification therefore used the added branch-only push workflow, without a PR.

Native Windows evidence: GitHub Actions **Windows Server 2025 Datacenter x64**,
OS build **10.0.26100**, image **windows-2025-vs2026 / 20260925.250.1**;
PowerShell **5.1.26100.33438**, CLR **4.0.30319.42000**;
`C:\Windows\system32\tar.exe`, **bsdtar/libarchive 3.8.4**.
The final run and exact counts are recorded in [evidence/SUMMARY.json](evidence/SUMMARY.json).
The [final native run](https://github.com/okanetsolutions/briareus-windows/actions/runs/37689523528)
ran **68 managed cases**: 59 rejects, 5 expected accepts, **2 unsafe accepts**,
one cooperative cancellation and one forced kill; every job directory was
removed and all 59 rejects wrote zero candidate files. Both actual compressed
storage checks held at 300 MiB, and decompressed storage held at 512 MiB.
The earlier corrected [native run](https://github.com/okanetsolutions/briareus-windows/actions/runs/37689243022)
also reproduced both managed gzip gaps. An initial run failed because inherited
Git for Windows newline settings changed the export fixture's ordinary file;
the generator now writes LF bytes and pins `core.autocrlf=false`, `core.eol=lf`.

Local evidence is separate: Ubuntu 24.04/aarch64, Linux 7.0.0-1019-nvidia,
temporary official PowerShell 7.6.6/.NET 10.0.12 and Ubuntu bsdtar 3.7.2.
The downloaded PowerShell tarball's SHA-256 was verified against its release
asset digest (`924829e54c983648f6f1419a2dc7f9433c861b2fb5bd57736ff096c24f133729`).
No Windows emulation or cross-compile is represented as a Windows execution.

## Results and limits

| Constraint | Actual prototype result |
| --- | --- |
| Compressed storage | `CopyCompressed` checks declared length before writing, counts each chunk before writing, and rejects incomplete/mismatched known length; actual 300 MiB + 1 byte known- and unknown-length bodies reject. |
| Whole decompressed storage | Counts every output byte, including unindexed data, headers and padding, before writing; 513 MiB fixture rejects without exceeding 512 MiB on disk. |
| Selected file/index/count | Actual 1 MiB + 1 byte file, 193 MiB selected and 5,001-file fixtures reject before candidate writes; scaled exact-boundary case accepts. |
| Pre-write safety | Traversal, POSIX absolute, drive/UNC, backslashes, ADS, devices, dot/empty components, trailing dot/space, controls, duplicates/case collisions, file-parent conflicts, links and special entries all reject before the first candidate file. |
| Tar/PAX completion | Header checksum, octal overflow, UTF-8 decoding, payload extent/padding, two zero end blocks, zero trailing padding and no orphan PAX are checked; missing/truncated completion rejects. |
| PAX/GNU behavior | GitHub global comment and local path are supported; raw and overridden paths are validated; unknown keys, link/size/sparse overrides and GNU extensions conservatively fall back. |
| Cancellation/cleanup | Cooperative cancel during inflation and forced kill of a hung worker pass; controller continues polling, worker exits, all per-job temporary files/directories are removed. |
| Strict gzip completion | **FAIL**: Windows PowerShell 5.1 ignores a second member and accepts a valid member followed by junk plus a copied footer; the oracle rejects unused input. |

`ArchiveProbe.cs` uses 64 KiB copy/inflate buffers, a 300 MiB compressed budget,
512 MiB whole-tar budget, existing 1 MiB/file and 192 MiB/5,000-file index limits,
100,000 total headers, 64 KiB/PAX payload and 1,024 UTF-8 bytes/path. At most
**1,004 MiB** of compressed + raw + candidate payload is retained per worker,
plus bounded metadata and filesystem overhead. A new index serialization must
get its own bound; it is not produced by this prototype. Handle disk-full as a
failed candidate, regardless of the size budget.

The bounds are independent: large unindexed files still count toward 512 MiB.
Tar names are never filesystem destinations. Whole-tar validation precedes
`Stage`, which writes only numbered `.blob` files in a generated directory.
All unsupported/malformed cases create zero candidate files. The known gzip
gaps intentionally record unsafe ACCEPT results; the green workflow means the
experiment reproduced expectations, **not that archives are safe**.

Cancellation uses a per-worker marker checked between bounded reads; after 2 s
the launcher kills the worker, waits for termination and deletes staging. There
is a 60 s absolute worker deadline. Tool stdout/stderr retention is capped at
64 KiB/stream with a 30 s deadline. This demonstrates a responsive controller,
not native Win32 UI responsiveness, ancestor-process crash recovery or a tested
Windows Job Object process-tree guarantee. Those remain implementation work.

The completion counterexample is directly reproducible from generated fixtures:

```text
gzip-forged-trailing-footer.tgz = valid_gzip + "NOT-A-GZIP-MEMBER" + valid_gzip[-8:]
gzip-concatenated.tgz = valid_gzip + valid_gzip
```

Neither a copied footer check nor `source.Position == source.Length` establishes
which bytes the decoder consumed: read-ahead can consume trailing input from
the file without decoding it. The missing component is a strict gzip/DEFLATE
reader that proves stream termination, validates CRC/ISIZE and optional header
fields, and accounts for **every** input byte/member. This investigation does
not assert that such a dependency-free implementation is impossible.

## GitHub root and canonical content

Downloaded the real pinned core commit directly from GitHub codeload (not a
deployed Briareus server): **940,081 compressed bytes**, root
`nadinyamaui-briareus-cb38ed7`. Managed preflight on Linux accepted the real
3,788,800-byte tar and staged two selected files, then cleaned it. Their bytes
matched pinned GitHub contents for `lib/repofiles.js` and
`docs/api-v1-reference.md`; hashes are in the evidence. This is a two-file
comparison, **not full-repository or deployed-core parity**. The real archive
contains `.gitattributes`, so conservative production rules would bypass it.

[GitHub generates these snapshots using git archive](https://docs.github.com/en/repositories/working-with-files/using-files/downloading-source-code-archives).
The native Windows Git fixture proved [export-ignore/export-subst](https://git-scm.com/docs/gitattributes)
at one commit: tree includes `src/hidden.cs`, the archive omits it; tree/blob text
contains `$Format:%H$`, the archive replaces it with the commit; even
`.gitattributes` itself is export-ignored. `src/plain.cs` matches canonical bytes
after fixing the fixture's EOL environment. Looking for attributes **in the
archive** or comparing only file sizes cannot guarantee canonical content.

## Capability, fallback and remaining work

1. **Current capability is false everywhere.** Preserve `repo_index_reconcile`
   and the existing four concurrent per-file fetch slots. Do not ship the helper
   or register an archive route as an enabled bulk-index path from this evidence.
2. If gzip is later proven, gate on deployed OpenAPI/read permission, full
   canonical tree/commit SHA, absolute OS tool/helper availability, runtime
   behavior tests and policy permitting execution. Missing tar/PowerShell,
   unsupported runtime/header/PAX, or blocked execution means per-file fallback;
   do not download a tool or locate an arbitrary executable through PATH.
3. Conservatively bypass the whole archive when **any canonical tree entry** is
   `.gitattributes`, including nested/skipped folders, or when the tree is
   truncated/unknown and cannot exclude one. This covers omitted attribute files
   and transformed content without implementing Git's attribute semantics.
4. Choose the same selected queue and limits as the current indexer before
   download. Strip exactly one consistent validated root; require each selected
   archive path to match its canonical tree path. Missing/mismatched/extra
   selected content is not a completed tree; use canonical per-file reads at the
   same full SHA. Do not publish transformed text as the viewer's cache.
5. A future C/store implementation must download off the UI thread with the
   existing HTTPS/token/redirect restrictions, validate transfer framing plus
   archive completion, close/abort network work on cancel, and retain the active
   index until candidate validation and cache persistence succeed. Cancel stale
   repository/ref/account generations; discard late callbacks.
6. Before any GO: resolve strict gzip; port/audit the tar parser to repository
   conventions; add Windows Job Object ownership (create suspended, assign,
   then resume), cancellation during startup/blocked I/O/staging and crash
   cleanup; apply canonical UTF-8/NUL/binary rules and disk/cache write bounds;
   exercise actual WinHTTP interrupted 200 responses, state switches and HTTP
   failures. Body truncation here is simulated by source streams with and
   without declared length, not a deployed-server download.
7. After that: run required GCC/MSVC/lint/coverage/ASan/leaks/UBSan/editorconfig,
   native Windows 10 1809/Windows 11 desktop UI QA, and same-SHA full-index parity
   and request/time/byte measurements against a deployed core. Only Server 2025
   was natively tested here; desktop support and performance gains are unproven.

Validation/size failures preserve the active index. Deterministic absence or
attribute incompatibility can select the old path; transient download failures
should report the failure and offer the existing throttled fallback. Respect
auth failures and Retry-After rather than starting another request burst.
