param([string]$CaseFile, [string]$InputFile, [string]$JobRoot)
$ErrorActionPreference = 'Stop'
$result = @{ status = 'REJECT'; reason = ''; compressedBytes = 0; rawBytes = 0; candidateFiles = 0; candidateBytes = 0; cleaned = $false }
try {
    Add-Type -Path (Join-Path $PSScriptRoot 'ArchiveProbe.cs')
    $case = Get-Content -Raw $CaseFile | ConvertFrom-Json
    [IO.File]::WriteAllText((Join-Path $JobRoot 'ready'), 'ready')
    if ($case.hang) { while ($true) { Start-Sleep -Milliseconds 100 } }
    $compressedLimit = [ArchiveProbe]::CompressedLimit
    $rawLimit = [ArchiveProbe]::RawLimit
    $fileLimit = [ArchiveProbe]::FileLimit
    $indexLimit = [ArchiveProbe]::IndexLimit
    $fileCountLimit = [ArchiveProbe]::FileCountLimit
    if ($null -ne $case.compressedLimit) { $compressedLimit = [long]$case.compressedLimit }
    if ($null -ne $case.rawLimit) { $rawLimit = [long]$case.rawLimit }
    if ($null -ne $case.fileLimit) { $fileLimit = [long]$case.fileLimit }
    if ($null -ne $case.indexLimit) { $indexLimit = [long]$case.indexLimit }
    if ($null -ne $case.fileCountLimit) { $fileCountLimit = [int]$case.fileCountLimit }
    $raw = Join-Path $JobRoot 'raw.tar'
    $cancel = Join-Path $JobRoot 'cancel'
    $stage = Join-Path $JobRoot 'candidate'
    $compressed = Join-Path $JobRoot 'compressed.gz'
    $source = [IO.File]::OpenRead($InputFile)
    try {
        $declaredLength = $source.Length
        if ($case.unknownLength) { $declaredLength = -1L }
        if ($null -ne $case.declaredLength) { $declaredLength = [long]$case.declaredLength }
        $result.compressedBytes = [ArchiveProbe]::CopyCompressed($source, $compressed, $declaredLength, $compressedLimit, $cancel)
    } finally { $source.Dispose() }
    $result.rawBytes = [ArchiveProbe]::Inflate($compressed, $raw, $compressedLimit, $rawLimit, $cancel, [int]$case.delayMs)
    $members = [ArchiveProbe]::ValidateTar($raw, [string[]]$case.selected, $fileLimit, $indexLimit, $fileCountLimit, $cancel)
    [ArchiveProbe]::Stage($raw, $stage, $members, $cancel)
    $result.status = 'ACCEPT'
} catch {
    $errorObject = $_.Exception
    while ($null -ne $errorObject.InnerException) { $errorObject = $errorObject.InnerException }
    $result.reason = $errorObject.Message.Substring(0, [Math]::Min(256, $errorObject.Message.Length))
    if ($errorObject -is [OperationCanceledException]) { $result.status = 'CANCEL' }
} finally {
    $raw = Join-Path $JobRoot 'raw.tar'
    $stage = Join-Path $JobRoot 'candidate'
    $compressed = Join-Path $JobRoot 'compressed.gz'
    if (Test-Path $compressed) { $result.compressedBytes = (Get-Item $compressed).Length; Remove-Item -Force $compressed }
    if (Test-Path $raw) { $result.rawBytes = (Get-Item $raw).Length }
    if (Test-Path $stage) {
        $files = @(Get-ChildItem -File $stage)
        $result.candidateFiles = $files.Count
        foreach ($file in $files) { $result.candidateBytes += $file.Length }
        Remove-Item -Recurse -Force $stage
    }
    if (Test-Path $raw) { Remove-Item -Force $raw }
    $result.cleaned = !(Test-Path $raw) -and !(Test-Path $stage) -and !(Test-Path $compressed)
}
$result | ConvertTo-Json -Compress
