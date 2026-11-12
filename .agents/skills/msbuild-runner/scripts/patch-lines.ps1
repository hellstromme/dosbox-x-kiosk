<#
.SYNOPSIS
    CRLF-safe, line-range-bounded file patcher for large DOSBox-X source files.
.DESCRIPTION
    Solves two critical issues when editing large upstream C++ files (e.g., sdlmain.cpp, bios.cpp):
    1. Normalizes CRLF (\r\n) and LF (\n) during matching while preserving the file's native line endings on save.
    2. Scopes edits to a specific 1-indexed [StartLine..EndLine] window so single-line or multi-line targets never accidentally match multiple occurrences elsewhere in a 13,000-line file.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,

    [int]$StartLine = 1,
    [int]$EndLine = 0,

    [string]$Target = "",
    [string]$TargetFile = "",

    [string]$Replacement = "",
    [string]$ReplacementFile = "",
    [string]$OutFile = "",

    [switch]$ReplaceEntireRange
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $FilePath -PathType Leaf)) {
    Write-Error "[patch-lines] File not found: $FilePath"
    exit 1
}

$resolvedPath = (Resolve-Path $FilePath).Path
$rawBytes = [System.IO.File]::ReadAllBytes($resolvedPath)
$hasUtf8Bom = ($rawBytes.Length -ge 3 -and $rawBytes[0] -eq 0xEF -and $rawBytes[1] -eq 0xBB -and $rawBytes[2] -eq 0xBF)
$encoding = [System.Text.UTF8Encoding]::new($hasUtf8Bom, $true)
$rawText = $encoding.GetString($rawBytes)
if ($hasUtf8Bom -and $rawText.Length -gt 0 -and [int]$rawText[0] -eq 0xFEFF) {
    $rawText = $rawText.Substring(1)
}

# Detect dominant line ending
$crlfCount = ([regex]::Matches($rawText, "\r\n")).Count
$lfOnlyCount = ([regex]::Matches($rawText, "(?<!\r)\n")).Count
$newline = if ($crlfCount -ge $lfOnlyCount) { "`r`n" } else { "`n" }

# Split into lines normalized without \r\n
$normalizedFull = $rawText.Replace("`r`n", "`n")
$lines = $normalizedFull.Split([char]"`n")
$totalLines = $lines.Count

if ($StartLine -lt 1 -or $StartLine -gt $totalLines) {
    Write-Error "[patch-lines] StartLine ($StartLine) out of range (1..$totalLines) in $FilePath"
    exit 1
}

if ($EndLine -le 0 -or $EndLine -gt $totalLines) {
    $EndLine = $totalLines
}

if ($EndLine -lt $StartLine) {
    Write-Error "[patch-lines] EndLine ($EndLine) cannot be less than StartLine ($StartLine)"
    exit 1
}

if ($TargetFile) {
    if (-not (Test-Path $TargetFile -PathType Leaf)) {
        Write-Error "[patch-lines] TargetFile not found: $TargetFile"
        exit 1
    }
    $Target = [System.IO.File]::ReadAllText((Resolve-Path $TargetFile).Path)
}

if ($ReplacementFile) {
    if (-not (Test-Path $ReplacementFile -PathType Leaf)) {
        Write-Error "[patch-lines] ReplacementFile not found: $ReplacementFile"
        exit 1
    }
    $Replacement = [System.IO.File]::ReadAllText((Resolve-Path $ReplacementFile).Path)
}

$normTarget = $Target.Replace("`r`n", "`n")
$normReplacement = $Replacement.Replace("`r`n", "`n")

# Extract prefix, range, and suffix (0-indexed slices)
$prefixLines = if ($StartLine -gt 1) { $lines[0..($StartLine - 2)] } else { @() }
$rangeLines  = $lines[($StartLine - 1)..($EndLine - 1)]
$suffixLines = if ($EndLine -lt $totalLines) { $lines[$EndLine..($totalLines - 1)] } else { @() }

$rangeText = $rangeLines -join "`n"

if ($ReplaceEntireRange) {
    $updatedRangeText = $normReplacement
    if ($updatedRangeText.EndsWith("`n")) {
        $updatedRangeText = $updatedRangeText.Substring(0, $updatedRangeText.Length - 1)
    }
} else {
    if ([string]::IsNullOrEmpty($normTarget)) {
        Write-Error "[patch-lines] Either -Target/-TargetFile or -ReplaceEntireRange must be specified."
        exit 1
    }
    $idx = $rangeText.IndexOf($normTarget, [System.StringComparison]::Ordinal)
    if ($idx -lt 0) {
        Write-Error "[patch-lines] Target block not found within lines $StartLine..$EndLine of $FilePath"
        exit 1
    }
    $UpdatedRangeBuilder = [System.Text.StringBuilder]::new()
    [void]$UpdatedRangeBuilder.Append($rangeText.Substring(0, $idx))
    [void]$UpdatedRangeBuilder.Append($normReplacement)
    [void]$UpdatedRangeBuilder.Append($rangeText.Substring($idx + $normTarget.Length))
    $updatedRangeText = $UpdatedRangeBuilder.ToString()
}

$newFullLines = @()
if ($prefixLines.Count -gt 0) { $newFullLines += $prefixLines }
$newFullLines += $updatedRangeText.Split([char]"`n")
if ($suffixLines.Count -gt 0) { $newFullLines += $suffixLines }

$finalText = $newFullLines -join $newline
$outBytes = $encoding.GetBytes($finalText)
if ($hasUtf8Bom) {
    $preamble = $encoding.GetPreamble()
    $combined = [byte[]]::new($preamble.Length + $outBytes.Length)
    [System.Array]::Copy($preamble, 0, $combined, 0, $preamble.Length)
    [System.Array]::Copy($outBytes, 0, $combined, $preamble.Length, $outBytes.Length)
    $outBytes = $combined
}
$destPath = if ($OutFile) { $OutFile } else { $resolvedPath }
if ($OutFile -and -not (Test-Path $OutFile)) {
    [System.IO.File]::WriteAllText($OutFile, $finalText, $encoding)
} else {
    $fs = [System.IO.File]::Open($destPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
    try {
        $fs.SetLength(0)
        $fs.Write($outBytes, 0, $outBytes.Length)
        $fs.Flush()
    } finally {
        $fs.Dispose()
    }
}

$lineEndingName = if ($newline -eq "`r`n") { "CRLF" } else { "LF" }
Write-Host "[patch-lines] Patched $FilePath (lines $StartLine..$EndLine, preserved $lineEndingName, $($lines.Count) -> $($newFullLines.Count) lines)" -ForegroundColor Green
