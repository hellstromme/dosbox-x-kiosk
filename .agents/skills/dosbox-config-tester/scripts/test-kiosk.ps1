<#
.SYNOPSIS
    Automated test runner for DOSBox-X Kiosk Edition runtime verification.
.DESCRIPTION
    Executes DOSBox-X with redirected I/O to capture GUI console output cleanly.
    Validates Kiosk mode bootstrapping, database connection, Dear ImGui initialization,
    and graceful shutdown.
#>

[CmdletBinding()]
param(
    [string]$ExecutablePath = "",
    [switch]$Tests = $false,
    [string]$TestFilter = "Kiosk*",
    [switch]$KioskMode = $false,
    [string]$KioskDb = "",
    [string]$KioskConf = "",
    [int]$TimeLimit = 5,
    [switch]$FastLaunch = $true,
    [switch]$ExitImmediately = $true,
    [string[]]$AdditionalArgs = @()
)

$ErrorActionPreference = "Stop"

# If neither -Tests nor -KioskMode was specified, default to -KioskMode for backward compatibility
if (-not $Tests -and -not $PSBoundParameters.ContainsKey('KioskMode')) {
    $KioskMode = $true
}

# 1. Resolve DOSBox-X Executable
if (-not $ExecutablePath) {
    $candidates = @(
        "bin/x64/Debug SDL2/dosbox-x.exe",
        "bin/x64/Release SDL2/dosbox-x.exe",
        "bin/Debug SDL2/dosbox-x.exe",
        "bin/Release SDL2/dosbox-x.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $ExecutablePath = $c
            break
        }
    }
}

if (-not $ExecutablePath -or -not (Test-Path $ExecutablePath)) {
    Write-Error "[dosbox-config-tester] Error: DOSBox-X executable not found. Please compile the solution first or specify -ExecutablePath."
    exit 1
}

$resolvedExe = (Resolve-Path $ExecutablePath).Path

# 2. Build argument list
$arguments = [System.Collections.Generic.List[string]]::new()
$tempDir = [System.IO.Path]::GetTempPath()
$gtestXml = if ($Tests) { Join-Path $tempDir "gtest_result_$([System.Guid]::NewGuid().ToString('N')).xml" } else { "" }

if ($Tests) {
    $arguments.Add("-tests")
    if ($TestFilter) {
        $arguments.Add("--gtest_filter=$TestFilter")
    }
    $arguments.Add("--gtest_output=xml:$gtestXml")
    $arguments.Add("-set")
    $arguments.Add("waitonerror=false")
    if ($FastLaunch) {
        $arguments.Add("-fastlaunch")
    }
    if ($ExitImmediately) {
        $arguments.Add("-exit")
    }
} else {
    if ($KioskMode) {
        $arguments.Add("-kiosk-mode")
    }
    if ($KioskDb) {
        $arguments.Add("-kiosk-db")
        $arguments.Add($KioskDb)
    }
    if ($KioskConf) {
        $arguments.Add("-kiosk-conf")
        $arguments.Add($KioskConf)
    }
    if ($FastLaunch) {
        $arguments.Add("-fastlaunch")
    }
    if ($ExitImmediately) {
        $arguments.Add("-exit")
    } elseif ($TimeLimit -gt 0) {
        $arguments.Add("-time-limit")
        $arguments.Add($TimeLimit.ToString())
    }
}

foreach ($arg in $AdditionalArgs) {
    if ($arg) { $arguments.Add($arg) }
}

$tempDir = [System.IO.Path]::GetTempPath()
$errFile = Join-Path $tempDir "dosbox_kiosk_test_$([System.Guid]::NewGuid().ToString('N')).err"
$outFile = Join-Path $tempDir "dosbox_kiosk_test_$([System.Guid]::NewGuid().ToString('N')).out"

Write-Host "================================================================="
Write-Host "[dosbox-config-tester] Executable: $resolvedExe"
Write-Host "[dosbox-config-tester] Arguments:  $($arguments -join ' ')"
Write-Host "================================================================="

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $resolvedExe `
                      -ArgumentList $arguments `
                      -NoNewWindow `
                      -Wait `
                      -PassThru `
                      -RedirectStandardError $errFile `
                      -RedirectStandardOutput $outFile

$exitCode = $proc.ExitCode
$sw.Stop()

$errContent = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { "" }
$outContent = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { "" }
$allLogs = "$errContent`n$outContent"

# Cleanup temp files
Remove-Item $errFile -Force -ErrorAction SilentlyContinue
Remove-Item $outFile -Force -ErrorAction SilentlyContinue

# 3. Analyze Results
if ($Tests) {
    $totalTests = 0
    $totalFailures = 0
    $totalErrors = 0
    if ($gtestXml -and (Test-Path $gtestXml)) {
        try {
            $xml = [xml](Get-Content $gtestXml -Raw)
            $totalTests = [int]$xml.testsuites.tests
            $totalFailures = [int]$xml.testsuites.failures
            $totalErrors = [int]$xml.testsuites.errors
        } catch {
            Write-Warning "[dosbox-config-tester] Could not parse Google Test XML: $_"
        }
        Remove-Item $gtestXml -Force -ErrorAction SilentlyContinue
    }

    $testsPassed = ($exitCode -eq 0) -and ($totalTests -gt 0) -and ($totalFailures -eq 0) -and ($totalErrors -eq 0)
    Write-Host "-----------------------------------------------------------------"
    Write-Host "[dosbox-config-tester] Mode:             Unit Tests (Google Test)"
    Write-Host "[dosbox-config-tester] Filter:           $TestFilter"
    Write-Host "[dosbox-config-tester] Tests Executed:   $totalTests"
    Write-Host "[dosbox-config-tester] Failures:         $totalFailures"
    Write-Host "[dosbox-config-tester] Errors:           $totalErrors"
    Write-Host "[dosbox-config-tester] Exit Code:        $exitCode"
    Write-Host "[dosbox-config-tester] Runtime:          $($sw.Elapsed.TotalSeconds.ToString('F2'))s"
    Write-Host "-----------------------------------------------------------------"
    if ($totalTests -eq 0) {
        Write-Host "[dosbox-config-tester] OVERALL STATUS: FAIL (0 tests executed matching filter: '$TestFilter')" -ForegroundColor Red
        Write-Host "================================================================="
        exit 1
    } elseif ($testsPassed) {
        Write-Host "[dosbox-config-tester] OVERALL STATUS: PASS (All $totalTests tests passed cleanly)" -ForegroundColor Green
        Write-Host "================================================================="
        exit 0
    } else {
        Write-Host "[dosbox-config-tester] OVERALL STATUS: FAIL ($totalFailures failure(s), $totalErrors error(s), exit code $exitCode)" -ForegroundColor Red
        Write-Host "================================================================="
        exit 1
    }
}

$kioskFlagDetected = $allLogs -match "KIOSK: DOSBox-X Kiosk Edition flag detected\."
$dbOpened = $allLogs -match "KIOSK: Opened SQLite database (.*)"
$dbMatch = if ($dbOpened) { $Matches[1] } else { "None" }

$uiInit = $allLogs -match "KIOSK: Dear ImGui context initialized successfully"
$subsystemsReady = $allLogs -match "KIOSK: Subsystems initialized successfully \(([^)]+)\)"
$versionInfo = if ($subsystemsReady) { $Matches[1] } else { "Unknown" }

$kioskShutdown = $allLogs -match "KIOSK: Shutting down Kiosk Subsystems\.\.\."
$uiShutdown = $allLogs -match "KIOSK: Dear ImGui shutdown completed\."

$allPassed = ($exitCode -eq 0) -and $kioskFlagDetected -and $subsystemsReady -and $kioskShutdown

Write-Host "-----------------------------------------------------------------"
Write-Host "[dosbox-config-tester] Exit Code:        $exitCode"
Write-Host "[dosbox-config-tester] Runtime:          $($sw.Elapsed.TotalSeconds.ToString('F2'))s"
Write-Host "[dosbox-config-tester] Flag Detected:    $(if ($kioskFlagDetected) { 'PASS' } else { 'FAIL' })"
Write-Host "[dosbox-config-tester] SQLite Catalog:   $dbMatch"
Write-Host "[dosbox-config-tester] ImGui UI Context: $(if ($uiInit) { 'PASS' } else { 'FAIL' })"
Write-Host "[dosbox-config-tester] Subsystem Ver:    $versionInfo"
Write-Host "[dosbox-config-tester] Clean Shutdown:   $(if ($kioskShutdown -and $uiShutdown) { 'PASS' } else { 'FAIL' })"
Write-Host "-----------------------------------------------------------------"

if ($allPassed) {
    Write-Host "[dosbox-config-tester] OVERALL STATUS: PASS" -ForegroundColor Green
    Write-Host "================================================================="
    exit 0
} else {
    Write-Host "[dosbox-config-tester] OVERALL STATUS: FAIL" -ForegroundColor Red
    Write-Host "--- Relevant Log Snippet ---"
    $relevantLines = $allLogs -split "`r?`n" | Where-Object { $_ -like "*KIOSK*" -or $_ -like "*ERROR*" -or $_ -like "*fatal*" }
    $relevantLines | ForEach-Object { Write-Host "  $_" }
    Write-Host "================================================================="
    exit 1
}
