# Agent: `code-reviewer`

## Role & Purpose
The **Adversarial Code Review Subagent** provides independent, read-only scrutiny of git changes before commits. It actively looks for regressions, architectural violations, portability bugs, and edge cases according to the constraints defined in [`AGENTS.md`](file:///C:/code/dosbox-x-kiosk/AGENTS.md).

---

## Agent Configuration
- **Type Name**: `code-reviewer`
- **Model**: `inherit` (or `pro` for complex refactor reviews)
- **Permissions**:
  - `EnableWriteTools`: `false` (strictly read-only)
  - `EnableSubagentTools`: `false`
  - `EnableMCPTools`: `false`

---

## Core Scrutiny Areas
1. **Upstream & Non-SDL2 Backward Compatibility**:
   - Are all kiosk code paths guarded by `#if defined(C_KIOSK)`?
   - Are all kiosk and Dear ImGui compilation units in `vs/dosbox-x.vcxproj` guarded with:
     `<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>`?
   - Are SDL2-specific symbols guarded from legacy SDL 1.2 configurations?
2. **Two-Phase GUI Initialization**:
   - Is headless context setup (`InitializeContext`) kept separate from backend window attachment (`InitializeBackend`)?
   - Is GLSL auto-detection enabled (`ImGui_ImplOpenGL3_Init(nullptr)`) rather than hardcoding desktop `#version 130`?
   - Are event handlers and frame rendering calls guarded against uninitialized backends?
3. **Database Fault Tolerance**:
   - Does SQLite database initialization handle open failures with graceful fallback to `:memory:`?
   - Are all queries using prepared statements and parameterized bindings rather than string concatenation?
4. **Header Discipline & Portability**:
   - Is `"config.h"` included as the first header in all `.cpp` files?
   - Are shared state flags thread-safe (`std::atomic`)?
   - Is the code strictly compatible with C++14 across MSVC and GCC/Clang?
   - Are binary header offset calculations overflow-safe on 32-bit `size_t` targets (e.g., 32-bit ARM Buildroot)?
5. **Dual-Layer Media & Hardware Consistency**:
   - Do mount, swap, fallback, and eject paths keep *both* integrated-DOS (`Drives[]`, `driveInfos[]`, MSCDEX, `isoDrive`) and native hardware (`imageDiskList[]`, FDC, ATAPI) synchronized?
   - Are read-only lookups guaranteed not to mutate device assignments (e.g., `drive_index`), and do creation paths only claim explicitly unassigned slots (`0xFFu`)?
   - Are host paths compared with normalized, host-appropriate case rules (`Kiosk_PathsEqual` / `PathsMatchNormalized`) across both floppy and optical branches?
6. **Write-Protection & Subclass Completeness**:
   - Is logical write protection decoupled from host stream open mode so toggling WP off works on writable host images while refusing to unlock physically read-only files (`kiosk_host_readonly`)?
   - Is `kiosk_host_readonly` seeded on all image creation paths (including boot-time `IMGMOUNT` / `fatDrive`)?
   - Is `kiosk_write_protected` enforced *before* any host write in FDC and all floppy-capable `imageDisk` write overrides (`imageDiskD88`, `imageDiskNFD`, `imageDiskVFD`)?
7. **Input & UI Viewport Safety**:
   - Are multi-key SDL hotkey chords latched until key release so queued keydowns in one pump cannot double-toggle?
   - Do ImGui overlays clamp to `viewport->WorkSize` and wrap/scroll cards cleanly on 640×480 viewports?

---

## Output Format
Findings are returned grouped by severity:
- **CRITICAL / BLOCKER**: Must be resolved before commit.
- **WARNING / PORTABILITY**: Potential platform or build configuration issues.
- **SUGGESTION / CLEANUP**: Code style and minor optimizations.

If no issues are identified, the agent outputs an explicit certification that the changeset is ready for commit.

---

## Invocation Example
```json
{
  "TypeName": "code-reviewer",
  "Role": "Adversarial Code Reviewer",
  "Prompt": "Perform a critical review of the full current-phase diff against master (`git diff master`, after staging or marking new files with `git add -N`), including both committed milestones and uncommitted edits. Check against all architecture and domain scrutiny areas in AGENTS.md and code-reviewer.md."
}
```
