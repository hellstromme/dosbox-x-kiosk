---
name: github-pr-review
description: Fetches, filters, replies to, and resolves GitHub PR review threads using the GitHub CLI (gh) with stdin JSON payloads immune to Windows/PowerShell quoting issues. Use whenever triaging or resolving PR comments on GitHub.
---

# Skill: `github-pr-review`

## Purpose
Streamlines PR review triage and thread resolution via `gh api graphql` on Windows while avoiding:
- Truncated raw GraphQL dumps (filters unresolved threads and strips bot HTML/prompt boilerplate).
- Stale `GITHUB_TOKEN` environment variable overrides (runs an upfront `gh auth status` diagnostic).
- Windows/PowerShell backtick and double-quote argument splitting (always passes GraphQL payloads over stdin via `gh api graphql --input -`).

## Scripts

### 1. Fetch PR Review Threads
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/github-pr-review/scripts/fetch-pr-threads.ps1 -Pr 3 -UnresolvedOnly
```
- Defaults to the current git repository's `owner/name` (auto-detected via `gh repo view` or `git remote get-url origin`), or specify `-Owner` and `-Repo`.
- Use `-UnresolvedOnly` to show only open threads and save context tokens.
- Strips CodeRabbit HTML `<picture>` badges and `<details>Prompt for AI Agents</details>` blocks for concise output.

### 2. Batch Reply & Resolve Threads (Stdin JSON)
Create a JSON file (e.g., in your scratch directory) containing an array of objects:
```json
[
  {
    "threadId": "PRRT_...",
    "body": "Addressed in commit abc1234: ...",
    "resolve": true
  }
]
```
Then run:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/github-pr-review/scripts/resolve-pr-threads.ps1 -InputJson "path/to/actions.json"
```
- Uses `gh api graphql --input -` with UTF-8 JSON piped via stdin, so markdown backticks, double quotes, and Windows paths are preserved verbatim.
- Set `"resolve": false` to reply without resolving.
- Omit `"body"` (or set it empty) to resolve without posting a reply.
