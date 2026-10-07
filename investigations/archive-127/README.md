# Archive reader feasibility (#127, epic #120)

Decision: **NO-GO for #128 archive integration** until strict gzip completion and
compressed-input consumption are proven; keep the existing per-file indexer.
This directory is an isolated experiment, never production code or an enabled
archive capability. The branch-only workflow runs it without a PR.

The inspected core PR #122 is open at
`cb38ed74572bd759df5db2c0c3316737e524ccf8` (2026-10-07).
See the final investigation report and evidence files added after verification.

## Reproduce

Generate fixtures with test-only Python stdlib, then use the Windows-provided
PowerShell (5.1) and OS tar, with no runtime library installation:

```powershell
python investigations/archive-127/make_fixtures.py "$env:TEMP/archive-127-fixtures" --large
powershell.exe -NoProfile -NonInteractive -File investigations/archive-127/run.ps1 -Fixtures "$env:TEMP/archive-127-fixtures" -Report managed-windows.json
powershell.exe -NoProfile -NonInteractive -File investigations/archive-127/tar-probe.ps1 -Fixtures "$env:TEMP/archive-127-fixtures" -Report tar-windows.json
```

`make_fixtures.py` supplies malicious/malformed/oversized/cancelled cases, an
independent strict-gzip oracle, and a real Git export-attribute fixture.
Python and its zlib module are test tools, **not** app dependencies.
`run.ps1` launches each experiment in a cancellable worker process and cleans
its private temporary directory, including after forced termination.
`worker.ps1` compiles `ArchiveProbe.cs` through built-in `Add-Type`.
`ArchiveProbe.cs` bounds gzip output, checks CRC/ISIZE, preflights the entire tar,
then writes only generated numeric staging names. It deliberately demonstrates
why trailer checks alone are insufficient; ACCEPT is not a safety guarantee.
`tar-probe.ps1`/`ToolProbe.cs` cap output capture and deadlines and never extract
archive paths to disk. No archive is passed to `tar -xf`.

`SECURITY_PROBE` cases record decoder-dependent acceptance/rejection. A matching
experimental expectation or green workflow does **not** authorize integration.
The production capability flag remains false regardless of test outcome.

## Limits tested

| Resource | Limit |
| --- | --- |
| Compressed input | 300 MiB (314,572,800 bytes) |
| Whole uncompressed tar, including skipped data and metadata | 512 MiB |
| Selected file | 1 MiB |
| Selected index/candidate bytes | 192 MiB |
| Selected files | 5,000 |
| All tar headers, including PAX | 100,000 |
| One PAX payload | 64 KiB |
| One path | 1,024 UTF-8 bytes |
| Worker cancellation grace / absolute deadline | 2 s / 60 s |
| Tool output capture / deadline | 64 KiB per stream / 30 s |

The whole-tar budget is separate from the index budget. A repository containing
large unindexed binaries can therefore require per-file fallback even if its
selected source files fit the index. `--large` includes actual 300 MiB + 1 byte,
513 MiB decompressed, 193 MiB selected, 1 MiB + 1 byte/file and 5,001-file cases.
