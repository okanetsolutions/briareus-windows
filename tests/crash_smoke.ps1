# Build crash_probe.exe from tests/crash_probe.c and app/crash.c, linking dbghelp, shell32, ole32 and uuid.
param([string]$Probe = '.\build\crash_probe.exe')
$ErrorActionPreference = 'Stop'
$started = [DateTime]::UtcNow
$process = Start-Process -FilePath $Probe -PassThru
if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw 'Crash probe timed out'
}
if ($process.ExitCode -eq 0) { throw 'Crash probe did not crash' }
$directory = Join-Path $env:LOCALAPPDATA 'Okanet\Briareus\Crashes'
$report = Get-ChildItem $directory -Filter "*-$($process.Id).txt" |
    Where-Object { $_.LastWriteTimeUtc -ge $started } | Select-Object -First 1
if (-not $report) { throw 'Crash report missing' }
$text = Get-Content $report.FullName -Raw
if ($text -notmatch 'Exception: 0xE0424242' -or $text -notmatch 'Minidump: saved') {
    throw "Unexpected report: $text"
}
$dump = [IO.Path]::ChangeExtension($report.FullName, '.dmp')
$bytes = [IO.File]::ReadAllBytes($dump)
if ($bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'MDMP') {
    throw 'Invalid minidump'
}
Remove-Item $report.FullName, $dump
Write-Output 'Crash report and minidump verified'
