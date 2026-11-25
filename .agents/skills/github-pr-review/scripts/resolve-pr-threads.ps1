<#
.SYNOPSIS
    Replies to and/or resolves GitHub PR review threads using stdin JSON payloads
    so Windows/PowerShell quoting never mangles markdown or double quotes.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$InputJson
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InputJson)) {
    Write-Error "[github-pr-review] Input JSON file not found: $InputJson"
    exit 1
}

$authOut = gh auth status 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "[github-pr-review] gh auth check failed. Clear stale GITHUB_TOKEN or run 'gh auth login'.`n$authOut"
    exit 1
}

$items = @(Get-Content -Raw -Path $InputJson | ConvertFrom-Json)
if ($items.Count -eq 0) {
    Write-Output "[github-pr-review] No items in $InputJson."
    exit 0
}

$replyQuery = 'mutation($threadId: ID!, $body: String!) { addPullRequestReviewThreadReply(input: { pullRequestReviewThreadId: $threadId, body: $body }) { comment { id } } }'
$resolveQuery = 'mutation($threadId: ID!) { resolveReviewThread(input: { threadId: $threadId }) { thread { id isResolved } } }'

$failures = 0
foreach ($item in $items) {
    $id = [string]$item.threadId
    if ([string]::IsNullOrWhiteSpace($id)) {
        Write-Warning "[github-pr-review] Skipping entry without threadId."
        continue
    }

    $hasBody = ($null -ne $item.body) -and (-not [string]::IsNullOrWhiteSpace([string]$item.body))
    $doResolve = if ($null -eq $item.resolve) { $true } else { [bool]$item.resolve }

    if ($hasBody) {
        $payload = @{
            query = $replyQuery
            variables = @{
                threadId = $id
                body     = [string]$item.body
            }
        } | ConvertTo-Json -Depth 6 -Compress

        $res = $payload | gh api graphql --input - 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Output "FAIL reply   $id : $res"
            $failures++
            continue
        }
    }

    if ($doResolve) {
        $payload = @{
            query = $resolveQuery
            variables = @{
                threadId = $id
            }
        } | ConvertTo-Json -Depth 6 -Compress

        $res = $payload | gh api graphql --input - 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Output "FAIL resolve $id : $res"
            $failures++
            continue
        }
    }

    $action = if ($hasBody -and $doResolve) { "replied & resolved" } elseif ($hasBody) { "replied" } else { "resolved" }
    Write-Output "OK $action $id"
}

if ($failures -gt 0) {
    exit 1
}
exit 0
