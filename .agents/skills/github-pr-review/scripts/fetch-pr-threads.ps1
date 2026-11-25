<#
.SYNOPSIS
    Fetches GitHub PR review threads cleanly via gh api graphql with compact output.
#>
param(
    [Parameter(Mandatory = $true)]
    [int]$Pr,

    [string]$Owner = "",
    [string]$Repo = "",
    [switch]$UnresolvedOnly,
    [int]$MaxBodyLines = 12
)

$ErrorActionPreference = "Stop"

# 1. Auth preflight check
$authOut = gh auth status 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "[github-pr-review] gh auth check failed. If GITHUB_TOKEN is set to a stale token, clear it or run 'gh auth login'.`n$authOut"
    exit 1
}

# 2. Resolve Owner/Repo if omitted
if ([string]::IsNullOrWhiteSpace($Owner) -or [string]::IsNullOrWhiteSpace($Repo)) {
    $remoteUrl = (git remote get-url origin 2>$null)
    if ($remoteUrl -match 'github\.com[:/]([^/]+)/([^/.]+?)(?:\.git)?$') {
        if ([string]::IsNullOrWhiteSpace($Owner)) { $Owner = $Matches[1] }
        if ([string]::IsNullOrWhiteSpace($Repo))  { $Repo  = $Matches[2] }
    } else {
        Write-Error "[github-pr-review] Could not infer GitHub owner/repo from origin remote. Pass -Owner and -Repo explicitly."
        exit 1
    }
}

$query = @'
query($owner: String!, $repo: String!, $pr: Int!) {
  repository(owner: $owner, name: $repo) {
    pullRequest(number: $pr) {
      reviewThreads(first: 100) {
        nodes {
          id
          isResolved
          isOutdated
          path
          line
          comments(first: 20) {
            nodes {
              databaseId
              author { login }
              body
              createdAt
            }
          }
        }
      }
    }
  }
}
'@

$payload = @{
    query = $query
    variables = @{
        owner = $Owner
        repo  = $Repo
        pr    = $Pr
    }
} | ConvertTo-Json -Depth 6 -Compress

$raw = $payload | gh api graphql --input - 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "[github-pr-review] GraphQL query failed:`n$raw"
    exit 1
}

$data = $raw | ConvertFrom-Json
$threads = @($data.data.repository.pullRequest.reviewThreads.nodes)
$resolvedCount = @($threads | Where-Object { $_.isResolved }).Count
$unresolvedCount = $threads.Count - $resolvedCount

Write-Output "PR #$Pr ($Owner/$Repo) | Total: $($threads.Count) | Resolved: $resolvedCount | Unresolved: $unresolvedCount"
Write-Output ("=" * 72)

$filtered = if ($UnresolvedOnly) { @($threads | Where-Object { -not $_.isResolved }) } else { $threads }
if ($filtered.Count -eq 0) {
    Write-Output "No matching review threads."
    exit 0
}

function Clean-ReviewBody([string]$text, [int]$maxLines) {
    if ([string]::IsNullOrEmpty($text)) { return "" }
    # Strip HTML comments, <picture>...</picture> badges, and CodeRabbit AI-agent prompt blocks
    $cleaned = [regex]::Replace($text, '(?s)<!--\s*ai-agent-prompt:start\s*-->.*', '')
    $cleaned = [regex]::Replace($cleaned, '(?s)<!--.*?-->', '')
    $cleaned = [regex]::Replace($cleaned, '(?s)<picture>.*?</picture>', '')
    $cleaned = [regex]::Replace($cleaned, '(?s)<details>.*?</details>', '')
    $cleaned = [regex]::Replace($cleaned, '<[^>]+>', '')
    $lines = @($cleaned -split "`r?`n" | ForEach-Object { $_.TrimEnd() } | Where-Object { $_.Trim() -ne "" })
    if ($lines.Count -gt $maxLines) {
        $lines = @($lines | Select-Object -First $maxLines) + @("... (truncated)")
    }
    return ($lines -join "`n")
}

$idx = 0
foreach ($t in $filtered) {
    $idx++
    $loc = if ($t.line) { "$($t.path):$($t.line)" } else { "$($t.path)" }
    Write-Output "[$idx] id=$($t.id) | resolved=$($t.isResolved) | outdated=$($t.isOutdated) | $loc"
    foreach ($c in $t.comments.nodes) {
        $author = if ($c.author) { $c.author.login } else { "ghost" }
        $body = Clean-ReviewBody $c.body $MaxBodyLines
        Write-Output "  @$author ($($c.createdAt)):"
        foreach ($bl in ($body -split "`n")) {
            Write-Output "    $bl"
        }
    }
    Write-Output ("-" * 72)
}
