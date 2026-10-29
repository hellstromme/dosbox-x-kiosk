<#
.SYNOPSIS
    Automated MSBuild runner for DOSBox-X Kiosk Edition.
.DESCRIPTION
    Discovers MSBuild, toolset, and Windows SDK dynamically.
    Enforces token-efficient filtered logging (/clp:ErrorsOnly) to avoid context flooding.
#>

[CmdletBinding()]
param(
    [string]$Configuration = "Debug SDL2",
    [string]$Platform = "x64",
    [string]$PlatformToolset = "",
    [string]$WindowsTargetPlatformVersion = "",
    [string]$SolutionPath = "vs/dosbox-x.sln",
    [switch]$Clean,
    [switch]$ShowWarnings
)

$ErrorActionPreference = "Stop"

# 1. Locate MSBuild.exe
$msBuildExe = $null
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
    if ($found -and (Test-Path $found)) {
        $msBuildExe = $found
    }
}

if (-not $msBuildExe) {
    $fallbackPaths = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $fallbackPaths) {
        if (Test-Path $p) {
            $msBuildExe = $p
            break
        }
    }
}

if (-not $msBuildExe) {
    Write-Error "[msbuild-runner] Error: MSBuild.exe could not be located via vswhere or fallback paths."
    exit 1
}

# 2. Auto-detect Windows SDK version if not provided
if (-not $WindowsTargetPlatformVersion) {
    $sdkDir = "C:\Program Files (x86)\Windows Kits\10\Include"
    if (Test-Path $sdkDir) {
        $installedSdks = @(Get-ChildItem -Directory $sdkDir | Select-Object -ExpandProperty Name | Sort-Object -Descending)
        if ($installedSdks.Count -gt 0) {
            $WindowsTargetPlatformVersion = $installedSdks[0]
        }
    }
    if (-not $WindowsTargetPlatformVersion) {
        $WindowsTargetPlatformVersion = "10.0.26100.0"
    }
}

# 3. Auto-detect PlatformToolset if not provided
if (-not $PlatformToolset) {
    if ($msBuildExe -like "*Visual Studio\18\*") {
        $PlatformToolset = "v145"
    } elseif ($msBuildExe -like "*Visual Studio\2022\*") {
        $PlatformToolset = "v143"
    } else {
        $PlatformToolset = "v145"
    }
}

# 4. Resolve Solution Path
$resolvedSolution = Resolve-Path $SolutionPath -ErrorAction SilentlyContinue
if (-not $resolvedSolution) {
    Write-Error "[msbuild-runner] Error: Solution file '$SolutionPath' does not exist."
    exit 1
}

# 5. Build arguments
$buildArgs = @(
    $resolvedSolution.Path,
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:PlatformToolset=$PlatformToolset",
    "/p:WindowsTargetPlatformVersion=$WindowsTargetPlatformVersion",
    "/m",
    "/v:minimal"
)

if ($Clean) {
    $buildArgs += "/t:Clean,Build"
} else {
    $buildArgs += "/t:Build"
}

if ($ShowWarnings) {
    $buildArgs += "/clp:Summary"
} else {
    $buildArgs += "/clp:ErrorsOnly"
}

Write-Host "================================================================="
Write-Host "[msbuild-runner] Target:    $($resolvedSolution.Path)"
Write-Host "[msbuild-runner] Config:    $Configuration | $Platform"
Write-Host "[msbuild-runner] Toolset:   $PlatformToolset (SDK: $WindowsTargetPlatformVersion)"
Write-Host "[msbuild-runner] MSBuild:   $msBuildExe"
Write-Host "[msbuild-runner] Filtering: $(if ($ShowWarnings) { 'Show Warnings' } else { 'Errors Only (Token Saver)' })"
Write-Host "================================================================="

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $msBuildExe $buildArgs
$exitCode = $LASTEXITCODE
$sw.Stop()

Write-Host "-----------------------------------------------------------------"
if ($exitCode -eq 0) {
    Write-Host "[msbuild-runner] SUCCESS: Build completed cleanly in $($sw.Elapsed.TotalSeconds.ToString('F2'))s (Exit code: 0)" -ForegroundColor Green
} else {
    Write-Host "[msbuild-runner] FAILED: Build exited with error code $exitCode after $($sw.Elapsed.TotalSeconds.ToString('F2'))s" -ForegroundColor Red
}
Write-Host "================================================================="

exit $exitCode
