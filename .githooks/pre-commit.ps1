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
        $rawIndexContent = git show ":$file" 2>$null
        if ($LASTEXITCODE -eq 0 -and $null -ne $rawIndexContent) {
            $lines = $rawIndexContent -split "`r?`n"
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

# 3. Rule 2: C++14 compliance check in kiosk and tests code
# Reject C++17/C++20 headers (<optional>, <string_view>, <variant>, <filesystem>, <any>)
$forbiddenHeaders = @("optional", "string_view", "variant", "filesystem", "any")
foreach ($file in $stagedFiles) {
    if ($file -like "src/kiosk/*" -or $file -like "src/kiosk/**/*" -or $file -like "tests/kiosk*") {
        $rawIndexContent = git show ":$file" 2>$null
        if ($LASTEXITCODE -eq 0 -and $null -ne $rawIndexContent) {
            $lines = $rawIndexContent -split "`r?`n"
            $lineNum = 0
            foreach ($line in $lines) {
                $lineNum++
                if ($line -match '^\s*#\s*include\s*[<"]([^>"]+)[>"]') {
                    $inc = $Matches[1]
                    if ($forbiddenHeaders -contains $inc) {
                        $violations.Add("$file : Line $lineNum includes forbidden C++17 header '<$inc>'. DOSBox-X Kiosk code must strictly adhere to C++14.")
                    }
                }
            }
        }
    }
}

# 4. Rule 3: Architecture check in vs/dosbox-x.vcxproj
# Kiosk/imgui compilation units must be excluded from non-SDL2 configurations
if ($stagedFiles -contains "vs/dosbox-x.vcxproj") {
    $vcxprojContent = git show ":vs/dosbox-x.vcxproj" 2>$null
    if (-not $vcxprojContent) {
        $vcxprojContent = Get-Content "vs/dosbox-x.vcxproj" -Raw
    }
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
                if ($match.Value -notmatch '<ExcludedFromBuild\s+Condition="!\$\(Configuration\.Contains\(''SDL2''\)\)"\s*>\s*true\s*</ExcludedFromBuild>') {
                    $violations.Add("vs/dosbox-x.vcxproj : '$entry' is missing non-SDL2 exclusion guard (<ExcludedFromBuild Condition=""!`$(Configuration.Contains('SDL2'))"">true</ExcludedFromBuild>).")
                }
            }
        }
    }
}

# 5. Rule 4: Documentation SQL schema & PowerShell quoting lint for docs/Kiosk/*.md
$docFiles = @($stagedFiles | Where-Object { $_ -like "docs/Kiosk/*.md" -and $_ -notlike "*Retrospective.md" })
if ($docFiles.Count -gt 0 -and (Test-Path "src/kiosk/kiosk_db.cpp")) {
    $dbSource = Get-Content "src/kiosk/kiosk_db.cpp" -Raw
    $validTables = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($m in [regex]::Matches($dbSource, 'CREATE\s+TABLE\s+IF\s+NOT\s+EXISTS\s+([A-Za-z_][A-Za-z0-9_]*)')) {
        [void]$validTables.Add($m.Groups[1].Value)
    }

    foreach ($docFile in $docFiles) {
        $docContent = git show ":$docFile" 2>$null
        if ($LASTEXITCODE -ne 0 -or $null -eq $docContent) { continue }
        $docLines = $docContent -split "`r?`n"
        $inPsBlock = $false
        for ($i = 0; $i -lt $docLines.Count; $i++) {
            $ln = $docLines[$i]
            $lineNo = $i + 1
            if ($ln -match '^\s*```powershell\b') {
                $inPsBlock = $true
                continue
            }
            if ($inPsBlock -and $ln -match '^\s*```\s*$') {
                $inPsBlock = $false
                continue
            }
            if ($inPsBlock) {
                if ($ln -match '(?<!["&])\.\\bin\\x64\\Debug SDL2\\dosbox-x\.exe') {
                    $violations.Add("$docFile : Line $lineNo has unquoted 'Debug SDL2' path in PowerShell block; use '& "".\bin\x64\Debug SDL2\dosbox-x.exe""'.")
                }
                if ($ln -match 'python\s+-c\s+"[^"]*\\"') {
                    $violations.Add("$docFile : Line $lineNo uses backslash-escaped double quotes inside PowerShell python -c; use a single-quoted here-string piped to 'python -'.")
                }
            }
            if ($validTables.Count -gt 0) {
                foreach ($sm in [regex]::Matches($ln, '\b(?:INSERT(?:\s+OR\s+REPLACE)?\s+INTO|DELETE\s+FROM)\s+([A-Za-z_][A-Za-z0-9_]*)\b')) {
                    $tbl = $sm.Groups[1].Value
                    if (-not $validTables.Contains($tbl)) {
                        $violations.Add("$docFile : Line $lineNo references unknown SQLite table '$tbl' (not found in src/kiosk/kiosk_db.cpp).")
                    }
                }
            }
        }
    }
}

# 6. Report results
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
