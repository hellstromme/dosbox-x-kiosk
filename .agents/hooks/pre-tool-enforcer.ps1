<#
.SYNOPSIS
    Antigravity PreToolUse hook for DOSBox-X Kiosk Edition.
.DESCRIPTION
    1. Intercepts raw MSBuild commands to enforce /v:m /clp:ErrorsOnly (token conservation).
    2. Intercepts git commit commands to run architectural pre-commit checks.
#>

$ErrorActionPreference = "SilentlyContinue"

try {
    $rawInput = [Console]::In.ReadToEnd()
    if (-not $rawInput) {
        Write-Output '{"decision":"allow"}'
        exit 0
    }

    $payload = $rawInput | ConvertFrom-Json
    $cmd = $payload.toolCall.args.CommandLine
    if (-not $cmd) {
        Write-Output '{"decision":"allow"}'
        exit 0
    }

    # 1. MSBuild Token Protection Interceptor
    # If the agent attempts raw MSBuild execution without token-saving flags, append them.
    if ($cmd -match "(?i)(^|[\s;&|])(\S*[\\/])?msbuild(\.exe)?(\s|$)" -and $cmd -notmatch "(?i)build\.ps1" -and $cmd -notmatch "(?i)msbuild-runner") {
        $modifiedCmd = $cmd
        $needsRewrite = $false

        if ($modifiedCmd -notmatch "(?i)/clp:ErrorsOnly" -and $modifiedCmd -notmatch "(?i)/clp:Summary") {
            $modifiedCmd += " /v:minimal /clp:ErrorsOnly"
            $needsRewrite = $true
        }
        if ($modifiedCmd -notmatch "(?i)/m(\s|:|$)" -and $modifiedCmd -notmatch "(?i)/maxcpucount") {
            $modifiedCmd += " /m"
            $needsRewrite = $true
        }

        if ($needsRewrite) {
            $response = @{
                decision = "allow"
                overwrite = @{
                    CommandLine = $modifiedCmd
                }
            }
            Write-Output ($response | ConvertTo-Json -Compress)
            exit 0
        }
    }

    # 2. Git Pre-Commit Architectural Gate
    if ($cmd -match "(^|\s|&&|;)git\s+commit") {
        $preCommitScript = Join-Path $PSScriptRoot "../../.githooks/pre-commit.ps1"
        $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
        if (Test-Path $preCommitScript) {
            $tmpOut = Join-Path $env:TEMP "precommit_out.log"
            $tmpErr = Join-Path $env:TEMP "precommit_err.log"
            $checkProcess = Start-Process powershell -ArgumentList "-ExecutionPolicy", "Bypass", "-File", "`"$preCommitScript`"" -WorkingDirectory $repoRoot -NoNewWindow -Wait -PassThru -RedirectStandardOutput $tmpOut -RedirectStandardError $tmpErr
            if ($checkProcess.ExitCode -ne 0) {
                $errContent = ""
                if (Test-Path $tmpOut) { $errContent = (Get-Content $tmpOut -Raw).Trim() }
                $response = @{
                    decision = "deny"
                    reason = "Commit blocked by DOSBox-X Kiosk architectural pre-commit checks. $errContent"
                }
                Write-Output ($response | ConvertTo-Json -Compress)
                exit 0
            }
        }
    }

    # Default Allow
    Write-Output '{"decision":"allow"}'
} catch {
    # If anything unexpected happens, fail open so as not to break developer workflow
    Write-Output '{"decision":"allow"}'
}
exit 0
