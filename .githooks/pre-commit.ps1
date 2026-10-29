<#
.SYNOPSIS
    Pre-commit sanitizer hook for DOSBox-X Kiosk Edition.
.DESCRIPTION
    Enforces header discipline, architecture constraints, and SDL2 exclusion guards
    before any commit is accepted into git.
#>

$ErrorActionPreference = "Stop"

$repoRoot = (git rev-parse --show-toplevel 2>$null)
if ($repoRoot -and (Test-Path $repoRoot)) {
    Set-Location $repoRoot
}

Write-Host "[pre-commit] Checking staged changes against Kiosk architectural constraints..." -ForegroundColor Cyan

# 1. Get staged files (Added, Copied, Modified)
$stagedFiles = @(git diff --cached --name-only --diff-filter=ACM)
if ($stagedFiles.Count -eq 0) {
    # If commit was run with -a, check unstaged modified files as well
    $stagedFiles = @(git diff --name-only --diff-filter=ACM)
}

if ($stagedFiles.Count -eq 0) {
    Write-Host "[pre-commit] No modified files detected. Skipping checks." -ForegroundColor Green
    exit 0
}

$violations = [System.Collections.Generic.List[string]]::new()

# 2. Rule 1: Header discipline in kiosk .cpp files
# Every .cpp in src/kiosk/ must have #include "config.h" before any other include
foreach ($file in $stagedFiles) {
    if ($file -like "src/kiosk/*.cpp" -or $file -like "src/kiosk/**/*.cpp") {
        if (Test-Path $file) {
            $lines = Get-Content $file
            $firstInclude = $null
            foreach ($line in $lines) {
                $trimmed = $line.Trim()
                if ($trimmed -match '^#\s*include\s*[<"]([^>"]+)[>"]') {
                    $firstInclude = $Matches[1]
                    break
                }
            }
            if (-not $firstInclude) {
                $violations.Add("$file : No header includes found. Expected #include ""config.h"".")
            } elseif ($firstInclude -ne "config.h") {
                $violations.Add("$file : First header included is '$firstInclude', expected 'config.h'. Always include 'config.h' at the top.")
            }
        }
    }
}

# 3. Rule 2: Architecture check in vs/dosbox-x.vcxproj
# Kiosk/imgui compilation units must be excluded from non-SDL2 configurations
if ($stagedFiles -contains "vs/dosbox-x.vcxproj") {
    $vcxprojContent = Get-Content "vs/dosbox-x.vcxproj" -Raw
    $kioskEntries = @(
        "..\src\libs\sqlite3\sqlite3.c",
        "..\src\libs\imgui\imgui.cpp",
        "..\src\libs\imgui\backends\imgui_impl_sdl2.cpp",
        "..\src\kiosk\kiosk_main.cpp",
        "..\src\kiosk\kiosk_db.cpp",
        "..\src\kiosk\ui\kiosk_ui.cpp",
        "..\src\kiosk\machine\archetypes.cpp",
        "..\src\kiosk\machine\config_synthesizer.cpp"
    )
    foreach ($entry in $kioskEntries) {
        if ($vcxprojContent.Contains($entry)) {
            # Find the block for this entry
            $pattern = [regex]::Escape($entry) + "[\s\S]*?</ClCompile>"
            $match = [regex]::Match($vcxprojContent, $pattern)
            if ($match.Success) {
                if (-not $match.Value.Contains('ExcludedFromBuild Condition="!$(Configuration.Contains(''SDL2''))"')) {
                    $violations.Add("vs/dosbox-x.vcxproj : '$entry' is missing non-SDL2 exclusion guard (<ExcludedFromBuild Condition=""!`$(Configuration.Contains('SDL2'))"">true</ExcludedFromBuild>).")
                }
            }
        }
    }
}

# 4. Report results
if ($violations.Count -gt 0) {
    Write-Host "[pre-commit] FAILED: Architectural constraint violations found:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  - $v" -ForegroundColor Red
    }
    Write-Host "[pre-commit] Commit aborted. Please remediate findings before committing." -ForegroundColor Red
    exit 1
}

Write-Host "[pre-commit] PASSED: All architectural constraints verified successfully." -ForegroundColor Green
exit 0
