param(
    [Parameter(Mandatory = $true)][string]$Fixtures,
    [string]$Report = 'archive-probe-results.json',
    [string]$TarPath = ''
)
$ErrorActionPreference = 'Stop'
$Fixtures = (Resolve-Path $Fixtures).Path
$nativeWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$hostExe = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
if (!$TarPath -and $nativeWindows) {
    $system = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
    # An x86 process must bypass filesystem redirection to find the OS x64 tool.
    if ([Environment]::Is64BitOperatingSystem -and ![Environment]::Is64BitProcess) {
        $system = Join-Path $env:windir 'Sysnative'
    }
    $TarPath = Join-Path $system 'tar.exe'
}
$environment = @{
    platform = [Environment]::OSVersion.ToString()
    nativeWindows = $nativeWindows
    powershell = $PSVersionTable.PSVersion.ToString()
    clr = [Environment]::Version.ToString()
    executable = $hostExe
    tar = $TarPath
    tarPresent = $TarPath -and (Test-Path $TarPath)
    productionCapability = $false
    capabilityReason = 'Strict gzip completion/consumption is unresolved; per-file fallback is required.'
}
if ($environment.tarPresent) { $environment.tarVersion = (& $TarPath --version 2>&1 | Out-String).Trim() }
$cases = Get-Content -Raw (Join-Path $Fixtures 'cases.json') | ConvertFrom-Json
$results = @()
$failures = 0
foreach ($case in $cases) {
    $jobRoot = Join-Path ([IO.Path]::GetTempPath()) ('archive-127-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($jobRoot) | Out-Null
    $caseFile = Join-Path $jobRoot 'case.json'
    $case | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $caseFile
    $inputFile = Join-Path $Fixtures $case.file
    $worker = Join-Path $PSScriptRoot 'worker.ps1'
    # All arguments are generated local paths; reject quotes instead of shell interpolation.
    foreach ($path in @($worker, $caseFile, $inputFile, $jobRoot)) {
        if ($path.Contains('"')) { throw 'Unsupported quote in prototype path' }
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $hostExe
    $info.Arguments = '-NoProfile -NonInteractive -File "' + $worker + '" -CaseFile "' + $caseFile + '" -InputFile "' + $inputFile + '" -JobRoot "' + $jobRoot + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $cancelSentAt = -1
    $readyAt = -1
    $killed = $false
    $heartbeat = 0
    $maxRawBytes = 0L
    try {
        $process.Start() | Out-Null
        # Only our worker writes these streams: one bounded result (<=2 KiB), no archive names.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        while (!$process.HasExited) {
            $heartbeat++
            if ($readyAt -lt 0 -and (Test-Path (Join-Path $jobRoot 'ready'))) { $readyAt = $timer.ElapsedMilliseconds }
            $raw = Join-Path $jobRoot 'raw.tar'
            $rawInfo = Get-Item $raw -ErrorAction SilentlyContinue
            if ($null -ne $rawInfo) { $maxRawBytes = [Math]::Max($maxRawBytes, $rawInfo.Length) }
            if ($null -ne $case.cancelAfterReadyMs -and $readyAt -ge 0 -and $cancelSentAt -lt 0 -and
                $timer.ElapsedMilliseconds - $readyAt -ge $case.cancelAfterReadyMs) {
                [IO.File]::WriteAllText((Join-Path $jobRoot 'cancel'), 'cancel')
                $cancelSentAt = $timer.ElapsedMilliseconds
            }
            if (($cancelSentAt -ge 0 -and $timer.ElapsedMilliseconds - $cancelSentAt -gt 2000) -or $timer.ElapsedMilliseconds -gt 60000) {
                $process.Kill()
                $killed = $true
                break
            }
            # The launcher stays responsive while the child does all CPU/disk work.
            Start-Sleep -Milliseconds 10
        }
        $process.WaitForExit()
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($killed) { $result = @{ status = 'KILL'; candidateFiles = 0; rawBytes = $maxRawBytes; reason = 'worker killed after cancellation grace period' } }
        else {
            $outputText = $stdout.GetAwaiter().GetResult()
            if ($process.ExitCode -ne 0 -or !$outputText.Trim()) { throw ('Worker failed: ' + $errorText) }
            $result = $outputText | ConvertFrom-Json
        }
    } finally {
        if ($process.Id -and !$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
        Remove-Item -Recurse -Force $jobRoot
    }
    $cleaned = !(Test-Path $jobRoot)
    $pass = $result.status -eq $case.result
    $knownGap = $case.result -eq 'SECURITY_PROBE'
    if ($knownGap) { $pass = $result.status -eq 'ACCEPT' -or $result.status -eq 'REJECT' }
    if (!$cleaned -or ($case.result -eq 'REJECT' -and $result.candidateFiles -ne 0)) { $pass = $false }
    if (!$pass) { $failures++ }
    $row = @{
        name = $case.name; expected = $case.result; actual = $result.status; passed = $pass
        knownUnsafeAcceptance = $knownGap -and $result.status -eq 'ACCEPT'
        reason = $result.reason; rawBytes = $result.rawBytes; candidateFiles = $result.candidateFiles
        compressedBytes = $result.compressedBytes
        candidateBytes = $result.candidateBytes; cleanup = $cleaned; launcherTicks = $heartbeat
        elapsedMs = $timer.ElapsedMilliseconds
    }
    $results += $row
    Write-Host ($case.name + ': ' + $result.status + ' cleanup=' + $cleaned + ' expected=' + $case.result)
}
@{ environment = $environment; cases = $results; failures = $failures; decision = 'NO-GO' } |
    ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $Report
if ($failures) { throw "$failures unexpected prototype results; inspect $Report" }
Write-Host ('Report: ' + $Report + '; production capability remains disabled.')
