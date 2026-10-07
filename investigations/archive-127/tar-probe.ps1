param([string]$Fixtures, [string]$Report = 'tar-probe-results.json', [string]$TarPath = '')
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'ToolProbe.cs')
if (!$TarPath) { $TarPath = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::System)) 'tar.exe' }
if (!(Test-Path $TarPath)) {
    @{ present = $false; capability = $false; fallback = 'per-file'; cases = @() } | ConvertTo-Json |
        Set-Content -Encoding UTF8 $Report
    exit 0
}
$results = @()
foreach ($name in @('valid-root', 'valid-github-pax', 'gzip-bad-crc', 'gzip-bad-isize', 'gzip-missing-footer',
                   'gzip-short-footer', 'gzip-truncated-deflate', 'gzip-trailing-junk', 'gzip-forged-trailing-footer',
                   'gzip-concatenated', 'tar-no-end', 'tar-one-end', 'tar-trailing-junk', 'traversal', 'symlink', 'pax-traversal')) {
    $fixture = Join-Path (Resolve-Path $Fixtures).Path ($name + '.tgz')
    if ($fixture.Contains('"')) { throw 'Unsupported quote in prototype path' }
    $result = [ToolProbe]::Run($TarPath, '-tf "' + $fixture + '"')
    $results += @{ name = $name; exit = $result.Exit; stdoutBytes = $result.StdoutBytes; stderr = $result.Stderr; killed = $result.Killed }
}
$fixture = Join-Path (Resolve-Path $Fixtures).Path 'valid-root.tgz'
$rawResult = [ToolProbe]::Run($TarPath, '-xOf "' + $fixture + '" --format raw')
@{ present = $true; tarPath = $TarPath; tarVersion = (& $TarPath --version | Out-String).Trim(); cases = $results;
   rawReaderAttempt = @{ exit = $rawResult.Exit; stdoutBytes = $rawResult.StdoutBytes; stderr = $rawResult.Stderr; killed = $rawResult.Killed } } |
    ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $Report
$results | Format-Table name, exit, stdoutBytes
