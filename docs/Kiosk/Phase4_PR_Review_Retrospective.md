# DOSBox-X Kiosk Edition: Phase 4 PR Review & Remediation Retrospective

> **Date:** November 25, 2026  
> **Scope:** PR #3 (`feat/kiosk-phase-4` -> `master`) — PR description, multi-round CodeRabbit & Codex review triage, remediation, verification, and GitHub thread resolution  
> **Commits Produced:** `08533e9c5`, `9c434f550`, `687d528f6`  
> **Final PR State:** 29/29 review threads resolved; `Debug SDL2 | x64` build clean; 37/37 Kiosk Google Tests + runtime smoke test passing

---

## 1. Executive Summary

This session covered the end-to-end PR lifecycle for **Phase 4 (Skeuomorphic Media Caddy & Runtime Disk-Swap Overlay)**:
1. Generated the PR description for `feat/kiosk-phase-4` -> `master`.
2. Evaluated 16 initial CodeRabbit findings against live `HEAD`, rejecting 2 stale/inapplicable items and implementing 14 valid fixes (`08533e9c5`).
3. Connected via GitHub CLI (`gh`), triaged all 23 review threads (replying to and resolving 10 addressed/disagreed threads), and surfaced 6 new valid issues.
4. Fixed those 6 issues (`9c434f550`) — raw floppy fallback `Drives[drive]` sync, logical vs. host-stream write protection, SDL hotkey chord latching, and PowerShell user guide fixes — and resolved their threads.
5. Reviewed and fixed a final round of 6 CodeRabbit comments (`687d528f6`) — FDC/D88/NFD/VFD pre-write WP guards, boot-mounted `IMGMOUNT` host-RO seeding, CD-ROM `Kiosk_PathsEqual`, and seed SQL schema fixes — bringing PR #3 to **29/29 resolved threads**.

---

## 2. What Worked Well

### 2.1 Evidence-First Triage Before Editing
- Verifying every automated review finding against the actual codebase before touching files prevented wasted churn:
  - Caught a stale CodeRabbit finding (`drives.cpp:748` slot reuse before `fopen`), which was already fixed in `1cff6c852`.
  - Caught a misattributed file reference (`AGENTS.md` vs. `.agents/agents/code-reviewer.md` invocation example).
  - Identified and pushed back on a workflow regression (`git diff --cached master` would have broken `git add -N` intent-to-add files).
  - Immediately surfaced genuine high-impact bugs: `Kiosk_IDE_FindOrCreateATAPI` mutating `D:`'s `drive_index` during read-only `E:` sense polling, and `RuntimeOverlay` leaving Drive `B:` unlocked during machine-only `REAL_HDD_BOOT` sessions.

### 2.2 Fast, Token-Efficient Build & Test Feedback Loop
- `msbuild-runner` (`build.ps1`) compiled incremental `Debug SDL2 | x64` builds in **4.0–14.3 seconds** with zero third-party warning noise.
- `dosbox-config-tester` (`test-kiosk.ps1`) executed all **37 Google Tests** and the **CLI/ImGui/SQLite smoke test** headlessly in **2–13 seconds** without GUI hangs.
- The preflight write-probe in `build.ps1` (upgraded to a unique GUID filename with `FileMode.CreateNew`) failed fast in **4 seconds** when the sandbox was read-only, avoiding 600-second MSBuild node hangs.

### 2.3 Native `edit_file` Worked Cleanly on CRLF Upstream Files
- Even when the Windows AppContainer sandbox blocked `patch-lines.ps1`, native `edit_file` (running in the host process) cleanly edited large CRLF upstream files (`src/dos/drives.cpp`, `src/dos/drives.h`, `src/dos/drive_fat.cpp`, `src/hardware/ide.cpp`, `src/hardware/floppy.cpp`, `src/ints/imagedisk_*.cpp`) without line-ending corruption.

### 2.4 Batch GraphQL Resolution via `gh`
- Once `gh` authentication was working, fetching all review threads, replying with exact commit hashes and technical rationales, and resolving threads via `gh api graphql` was fast, auditable, and repeatable.

---

## 3. What We Struggled With

### 3.1 Windows AppContainer Read-Only Sandbox ACL
- Throughout the session, `run_command` in standard sandbox mode could not write to the workspace (`Access to the path ... is denied`, `.git/index.lock: Permission denied`), forcing `BypassSandbox: true` retries for `build.ps1`, `git add`, `git commit`, and `git push`.
- Earlier `patch-lines.ps1` attempts failed across 5 parallel calls before pivoting to `edit_file`.

### 3.2 GitHub CLI (`gh`) Authentication Friction
- The first `gh` attempt failed with `401 Bad credentials` because a stale `GITHUB_TOKEN` environment variable overrode `gh`'s credential store and no account was logged in via `gh auth login`. This cost a full user round-trip to resolve.

### 3.3 PowerShell Quoting & Escaping Pitfalls (3 Separate Incidents)
1. **Backticks in double-quoted strings**: First draft of `resolve_pr3_threads.ps1` used `"..."`, which would have stripped markdown backticks; caught before execution and rewritten with `'...'`.
2. **Native CLI argument splitting on Windows**: Passing embedded double quotes inside `gh api graphql -f body=$body` caused `gh.exe` to fail with `accepts 1 arg(s), received 2` on thread `PRRT_kwDOVaVjAc66IFH_`, requiring a manual retry.
3. **PowerShell 5.1 `python -c` quoting in docs**: Fixing the user guide's seed script took two review rounds (`python -c "..."` -> `python -c $seedScript` -> `$seedScript | python -`) because Windows PowerShell 5.1 strips embedded double quotes on native arguments.

### 3.4 First-Pass C++ `switch`/`case` Scope Error
- Declaring `const bool door_closed` inside unbraced `case` blocks in `src/hardware/floppy.cpp` triggered MSVC `C2360`/`C2374` errors on the first compile, costing an extra edit + rebuild cycle.

### 3.5 Multi-Round Review Whack-a-Mole
- Fixing Round 1 findings exposed deeper architectural coupling that triggered Rounds 2 and 3:
  - Separating logical write-protection from host stream access (`9c434f550`) exposed that `IMGMOUNT` boot-mounted `fatDrive` slots never seeded `kiosk_host_readonly` and that `imageDiskD88`/`NFD`/`VFD` overrides bypassed `kiosk_write_protected` (`687d528f6`).
  - Fixing the PowerShell quoting in `Kiosk_Phases_1_to_4_User_Guide.md` drew CodeRabbit's attention to the SQL inside the script, exposing `session_caddy` (wrong table name) and missing `guest_working_dir` (`NOT NULL`).

---

## 4. What Wasted Tokens

| Source of Waste | Impact | Root Cause |
|---|---|---|
| **5 failed `patch-lines.ps1` calls + 5 unused scratch patch files** | ~3 turns | Followed `AGENTS.md` Rule 2.3 before checking if `run_command` had write access or if `edit_file` worked directly. |
| **Repeated sandbox-first failures on known read-only workspace** | ~4 turns | Retried `build.ps1` and `git add` in standard sandbox mode even after confirming the workspace ACL was read-only. |
| **Truncated GraphQL dump requiring secondary `read_file`** | ~2 turns | Printed all 23 threads (including already-resolved ones and verbose CodeRabbit HTML/prompt blocks) instead of filtering to `isResolved == false` and stripping boilerplate. |
| **Three separate review -> fix -> build -> test -> push -> resolve cycles** | High cumulative context re-send | Round 1 fixes didn't audit sibling paths (e.g., `imageDisk` subclasses, `IMGMOUNT` boot path, CD-ROM `==` vs `Kiosk_PathsEqual`, SQL schema verification in docs). |
| **Rewriting `resolve_pr3_threads.ps1` + retrying `PRRT_kwDOVaVjAc66IFH_`** | ~2 turns | Ad-hoc PowerShell script instead of a reusable stdin-JSON helper. |
| **Untracked `imgui.ini` noise** | Minor | Left behind in workspace root every time `dosbox-x.exe` runs; had to carefully avoid `git add -A`. |

---

## 5. What Information You Could Have Provided to Improve Output

1. **Upfront `gh` availability & auth state**: Starting directly with `gh` on PR #3 (instead of pasting Round 1 CodeRabbit text manually and introducing `gh` in Round 2) would have caught the 3 Codex threads and exact thread IDs in Round 1 and saved an entire round-trip.
2. **Scope preference on review fixes**: Saying up front *"fix all valid code + doc + outside-diff issues and push"* (or *"code only, skip docs/outside-diff"*) avoids an intermediate confirmation turn.
3. **Regression test expectation**: Indicating whether you want new Google Tests added for review-discovered bugs (e.g., `SetKioskFloppyWriteProtect`, hotkey chord latching, raw floppy fallback, D88/NFD/VFD WP) ensures test coverage grows alongside bug fixes instead of relying only on the existing 37 tests.
4. **Target PowerShell version**: Knowing whether Windows PowerShell 5.1 compatibility is required for user-facing docs (vs. PowerShell 7+) would have led directly to `$seedScript | python -` on the first edit.

---

## 6. New Agents, Skills, and Hooks That Would Have Improved Output

### 6.1 New Skill: `github-pr-review` (`.agents/skills/github-pr-review/`)
A dedicated skill with two small PowerShell scripts would eliminate ~60% of the PR-review friction and token waste:
- **`fetch-pr-threads.ps1 -Pr <N> [-UnresolvedOnly]`**:
  - Checks `gh auth status` first and warns clearly if a stale `GITHUB_TOKEN` env var is shadowing `gh` login.
  - Fetches review threads via GraphQL, strips CodeRabbit's `<details>Prompt for AI Agents</details>` and HTML badge boilerplate, and outputs a compact Markdown table (`threadId`, `path:line`, `author`, `isOutdated`, concise summary).
- **`resolve-pr-threads.ps1 -InputJson <path>`**:
  - Reads an array of `{ threadId, body, resolve }` objects and pipes JSON to `gh api graphql --input -` over stdin.
  - **Completely immune** to PowerShell backtick escaping and Windows native double-quote argument splitting.

### 6.2 New Pre-Commit / Doc-Lint Hook Checks
Extend `.git/hooks/pre-commit` (or add a lightweight `.agents/skills/kiosk-lint/` script) to check:
1. **SQL schema drift in docs**: Verify table names referenced in `docs/Kiosk/*.md` SQL blocks (`INSERT INTO`, `DELETE FROM`, `UPDATE`) exist in `src/kiosk/kiosk_db.cpp` (would have caught `session_caddy` vs. `caddy_bindings` immediately).
2. **PowerShell doc pitfalls**: Flag unquoted `Debug SDL2` paths and `\"` inside PowerShell fenced blocks.
3. **Untracked `imgui.ini`**: Add `imgui.ini` to `.gitignore` so test runs never dirty `git status`.

### 6.3 Sibling-Audit Checklist (Before Pushing Any Fix)
A short mental/skill checklist when fixing a review comment:
- *"Does this same bug exist in a sibling path?"* (e.g., Floppy vs. CD-ROM path comparison; `AppendAndSwapDisk` vs. boot-time `IMGMOUNT`; base `imageDisk` vs. `D88`/`NFD`/`VFD` overrides; Mode A vs. Mode B boot).
- Checking siblings in Round 1 would have collapsed Rounds 2 and 3 into a single commit.

---

## 7. Improvements to Existing Agents, Skills, and Hooks

### 7.1 `.agents/agents/code-reviewer.md`
1. **Fix stale invocation example (lines 48–55)**: Currently says `"current uncommitted changes (git diff) against master"`. Update to `"full current-phase diff against master (git diff master after staging or git add -N on new files)"` so it matches `AGENTS.md` Step 3 (this was the exact discrepancy CodeRabbit spotted, even though it cited `AGENTS.md`).
2. **Add Domain-Specific Scrutiny Areas** (the 4 current areas are build/init/SQLite/headers—they missed every single runtime hardware/media bug that CodeRabbit & Codex caught):
   - **Dual-Layer Media State Consistency**: Verify that mount, swap, fallback, and eject keep *both* integrated-DOS (`Drives[]`, `driveInfos[]`, MSCDEX, `isoDrive`) and native hardware (`imageDiskList[]`, FDC, ATAPI) synchronized.
   - **Write-Protection & Subclass Completeness**: Verify logical write protection is decoupled from host stream open mode, host read-only state is seeded on all creation paths (including boot `IMGMOUNT`), and all `imageDisk` write overrides (`D88`, `NFD`, `VFD`) enforce `kiosk_write_protected` *before* host writes.
   - **Input & UI Viewport Safety**: Verify multi-key SDL chords are latched until key release, path comparisons respect Linux case-sensitivity, and ImGui modals clamp/wrap at 640×480.
   - **32-bit Portability**: Check binary header offset arithmetic (`size_t` overflow on 32-bit ARM Buildroot targets).
3. **Sandbox ACL Mitigation**: Investigate running `code-reviewer` with `Workspace: share` or `branch` (or documenting that `code-reviewer` should run in a dedicated session) so it stops downgrading the parent workspace's Windows AppContainer ACL to read-only.

### 7.2 `.agents/skills/msbuild-runner/SKILL.md` & `AGENTS.md`
1. **Update `AGENTS.md` Rule 2.3 (`patch-lines.ps1`)**: Clarify that native `edit_file` works cleanly on CRLF upstream files and runs in the host process (unaffected by read-only sandbox ACLs), so `edit_file` should be the default for scoped edits, reserving `patch-lines.ps1` for large multi-hundred-line range replacements.
2. **Keep `build.ps1` unique GUID probe**: The fix made in `08533e9c5` (`FileMode.CreateNew` + `finally` cleanup) worked reliably and saved time.

### 7.3 `tests/kiosk_tests.cpp` & `dosbox-config-tester`
- Add targeted unit tests for the Phase 4 review fixes so they cannot regress in Phase 5+:
  - `KioskManager::IsOverlayHotkeyEvent` chord latching (two queued keydowns in one pump fire once; release resets latch).
  - `DriveManager::SetKioskFloppyWriteProtect` refusing to unlock a `kiosk_host_readonly` floppy, and succeeding on a writable host floppy initially mounted write-protected.
  - `Kiosk_IDE_FindOrCreateATAPI` read-only lookup (`Kiosk_IDE_GetATAPISense`) not mutating `D:` when querying unmounted `E:`.
  - Non-SDL2 regression build (`Debug | x64`) verification before merging PR #3 to `master`.

---

## 8. Recommended Action Plan for Future Sessions

| Priority | Action | Target | Expected Benefit |
|---|---|---|---|
| **P0** | Add `imgui.ini` to `.gitignore` | `.gitignore` | Prevents accidental staging / dirty working tree after test runs. |
| **P0** | Run non-SDL2 regression build (`Debug \| x64`) before merging PR #3 | `build.ps1` | Satisfies `AGENTS.md` Rule 3.1 before merging Phase 4 to `master`. |
| **P1** | Create `github-pr-review` skill (`fetch-pr-threads.ps1` + stdin-JSON `resolve-pr-threads.ps1`) | `.agents/skills/github-pr-review/` | Eliminates GraphQL truncation, PowerShell quoting bugs, and ~30% of PR-review token usage. |
| **P1** | Update `.agents/agents/code-reviewer.md` (fix invocation prompt + add dual-layer media, WP subclass, chord latch, and 32-bit overflow checks) | `.agents/agents/code-reviewer.md` | Catches hardware/media bugs locally before opening PRs, reducing multi-round CodeRabbit cycles. |
| **P1** | Add regression unit tests for chord latch, `SetKioskFloppyWriteProtect`, and ATAPI `E:` sense lookup | `tests/kiosk_tests.cpp` | Locks in Phase 4 review fixes permanently. |
| **P2** | Update `AGENTS.md` Rule 2.3 to prefer native `edit_file` over `patch-lines.ps1` for scoped CRLF edits | `AGENTS.md` | Avoids wasted sandbox failures and scratch patch files. |
| **P2** | Add SQL table-name & PowerShell-quoting lint checks for `docs/Kiosk/*.md` | `.git/hooks/pre-commit` | Catches broken manual-testing commands before commit. |
