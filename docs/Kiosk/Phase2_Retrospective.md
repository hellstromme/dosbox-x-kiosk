# DOSBox-X Kiosk Edition: Phase 2 Retrospective

> **Date:** October 31, 2026  
> **Milestone:** Phase 2 — Kiosk Lifecycle State Machine & Emulation Trapping  
> **Branch:** `feature/kiosk-phase-2`  
> **Plan Reference:** [`docs/Kiosk/Phase2_Lifecycle_State_Machine_Plan.md`](./Phase2_Lifecycle_State_Machine_Plan.md)  
> **Author:** Antigravity / Pair Programming Session  

---

## 1. Executive Summary

Phase 2 transformed the Kiosk subsystem from a passive database/synthesizer library into an active lifecycle supervisor capable of managing pre-boot countdowns, launching synthesized configurations, pausing emulation via an in-game overlay, and intercepting guest OS shutdowns to return cleanly to the Kiosk Shell instead of terminating the host process:

- **6-State Kiosk Lifecycle Engine**: Implemented [`KioskManager`](../../src/kiosk/kiosk_main.h) (`STATE_KIOSK_BOOT`, `STATE_COUNTDOWN_IDLE`, `STATE_SHELL_INTERACTIVE`, `STATE_SYNTHESIZE_LAUNCH`, `STATE_EMULATION_RUNNING`, `STATE_RUNTIME_OVERLAY`) with thread-safe `std::mutex` and `std::atomic` state management, pre-boot watchdog timer, analog/mouse deadzone input filtering, default boot target resolution (`DEFAULT_DOSBOX`, `SPECIFIC_MACHINE`, `SPECIFIC_ITEM`, `LAST_PLAYED`), and launch telemetry (`IncrementPlayCount`).
- **BIOS APM, ISA PnP & Port `0x8900` Shutdown Trapping**: Updated [`src/ints/bios.cpp`](../../src/ints/bios.cpp) under `#if defined(C_KIOSK)` to intercept APM BIOS power-off (`INT 15h AX=5307h BX=0001h CX=0003h`), ISA PnP power-off (`INT 15h AH=41h`), and Bochs Port `0x8900` (`"Shutdown"` ASCII sequence), throwing [`KioskGuestShutdownException`](../../src/kiosk/kiosk_types.h) to unwind `DOSBOX_RunMachine()` cleanly rather than invoking `exit(0)`.
- **SDL2 & Dear ImGui Main Loop Integration**: Updated [`src/gui/sdlmain.cpp`](../../src/gui/sdlmain.cpp) and [`src/kiosk/ui/kiosk_ui.cpp`](../../src/kiosk/ui/kiosk_ui.cpp) to manage OpenGL backend attachment/detachment across resolution switches, composite active Kiosk UI states in `SDL_GL_SwapBuffers()`, route `F12+D` and `SDL_CONTROLLER_BUTTON_GUIDE` overlay hotkeys, and catch `KioskGuestShutdownException`.
- **Unit & Runtime Verification**: Added 6 Phase 2 unit tests in [`tests/kiosk_tests.cpp`](../../tests/kiosk_tests.cpp) (19/19 total Kiosk tests passing with 0 failures), verified both `Debug SDL2|x64` and legacy non-SDL2 `Debug|x64` builds with 0 errors, and obtained **CERTIFIED READY FOR COMMIT** sign-off from the adversarial `code-reviewer` subagent.

---

## 2. What Worked Well

### 2.1 Upfront Ambiguity Resolution (`ask_question`)
- During initial planning, a discrepancy was identified between the Master Roadmap ([`DOSBox-X_Kiosk_Implementation_Plan.md`](./DOSBox-X_Kiosk_Implementation_Plan.md), which defines Phase 2 as *Kiosk Lifecycle State Machine & Emulation Trapping*) and the historical Phase 1 notes in `continue.md` (which referred to Phase 2 as *ImGui Carousel Frontend & Skeuomorphic Caddy UI*).
- Asking a targeted multiple-choice question before writing the plan prevented building the wrong milestone and aligned the implementation with the architectural dependency order (the Carousel UI in Phase 3 depends on the Phase 2 state machine and guest return trap).

### 2.2 Zero-Warning Builds & Automated Google Test Execution
- The improvements made at the end of Phase 1 paid off immediately:
  - [`msbuild-runner`](../../.agents/skills/msbuild-runner/SKILL.md) built `Debug SDL2|x64` in **9.89s** and legacy `Debug|x64` in **7.63s** with **0 errors on the very first compile attempt**.
  - [`dosbox-config-tester`](../../.agents/skills/dosbox-config-tester/SKILL.md) with `-Tests -TestFilter "Kiosk*"` executed all 19 Google Tests headlessly via XML output in **12.37s** with zero console hangs.
  - Strictly adhering to the C++14 constraint (`AGENTS.md` Rule 3.4) avoided any `std::optional` / `std::string_view` compilation failures.

### 2.3 Adversarial Pre-Commit Code Review (`code-reviewer`)
- The [`code-reviewer`](../../.agents/agents/code-reviewer.md) subagent audited all 8 touched files against `AGENTS.md` constraints and caught a meaningful embedded performance issue:
  - **Per-Frame SQLite Queries in UI Rendering**: `KioskUI::RenderInteractiveShellPlaceholder()` initially called `GetAllCatalogItems()` and `GetAllMachines()` every frame (60 Hz). On an embedded Linux appliance booting from SD/eMMC, per-frame SQLite queries during idle menu rendering are undesirable. We immediately added catalog caching (`m_cached_items`, `m_cached_machines`, `InvalidateCatalogCache()`) to [`KioskUI`](../../src/kiosk/ui/kiosk_ui.h).
  - **Test & Scope Cleanups**: Caught duplicate `#include` lines and a duplicate `UpdateConfig` assertion in [`tests/kiosk_tests.cpp`](../../tests/kiosk_tests.cpp).

---

## 3. What We Struggled With

### 3.1 `edit_file` Multi-Line Target Failures on CRLF Files
- **Problem**: Upstream DOSBox-X files (`src/ints/bios.cpp`, `src/gui/sdlmain.cpp`, `src/kiosk/kiosk_types.h`, `docs/Kiosk/continue.md`) use Windows CRLF (`\r\n`) line endings. Because tool call arguments normalize newlines to LF (`\n`), every multi-line `Target` passed to `edit_file` on a CRLF file failed with:
  ```text
  Encountered error in tool execution: target content not found in the file; the Target must exactly match existing file text, including whitespace
  ```
- **Secondary Risk of Single-Line Workarounds**:
  - To bypass the CRLF newline mismatch, we used single-line `Target` strings in `edit_file` (since a single line contains no `\r\n` internally).
  - However, `edit_file` replaces **every** occurrence of `Target` in the file:
    1. In [`src/ints/bios.cpp`](../../src/ints/bios.cpp), anchoring on a single line inside an `if (...) { ... } else { ... }` block required careful inspection to avoid breaking the `else` branch.
    2. In [`src/gui/sdlmain.cpp`](../../src/gui/sdlmain.cpp), a single-line target `    SDL_GL_SwapWindow(sdl.window);` matched **6 occurrences** across the file rather than just the one inside `SDL_GL_SwapBuffers()`.

### 3.2 Navigating Massive Monolithic Upstream Files
- [`src/ints/bios.cpp`](../../src/ints/bios.cpp) is **13,527 lines** and [`src/gui/sdlmain.cpp`](../../src/gui/sdlmain.cpp) is **10,943 lines**.
- Locating all relevant hook points (APM `INT 15h`, ISA PnP `INT 15h`, `BOCHS_PORT_E9` init/cleanup, `GFX_SetSize` GL context creation/deletion, `GFX_Events` poll loop, `fresh_boot:` / `DOSBOX_RunMachine()` exception handling, and `SDL_GL_SwapBuffers`) required numerous `read_file` slices.

### 3.3 Read-Only Subagent Windows Sandbox ACL Side Effect
- When `code-reviewer` (`EnableWriteTools: false`) was invoked with default `Workspace: inherit`, the Windows AppContainer setup for the read-only subagent replaced the workspace directory's AppContainer SID ACE (`S-1-15-3-1024-...`) with traverse-only (`0x100020`), revoking write permissions for subsequent sandboxed `run_command` calls in the parent agent.
- Native host tools (`edit_file`, `write_file`) remain unaffected because they run outside the AppContainer. We documented this rule in [`AGENTS.md`](../../AGENTS.md) so `build.ps1` and `test-kiosk.ps1` are always executed prior to read-only subagent invocation.

### 3.4 Mid-Session Context Compaction
- Executing both the `/plan` research phase and the full Phase 2 code implementation within a single conversation caused the context window to reach capacity and compact midway through Step 2.3 (`src/ints/bios.cpp`), requiring a few extra file reads to verify the post-compaction state of `bios.cpp` and `sdlmain.cpp`.

---

## 4. Token Waste & Efficiency Analysis

```text
+-------------------------------------------------------------------------+
| Estimated Token Waste Breakdown (Phase 2)                               |
+-------------------------------------------------------------------------+
| 1. Multi- Slice Reads of sdlmain.cpp (10.9k) & bios.cpp (13.5k): ~18,000|
| 2. Failed Multi-Line edit_file Calls on CRLF Files & Retries:    ~10,000|
| 3. Post-Compaction Context Re-orientation Reads:                  ~5,000|
| 4. Background Build Task Polling / Status Checks:                 ~2,000|
+-------------------------------------------------------------------------+
```

- **Compare with Phase 0 / Phase 1**:
  - Build log token waste was **0 tokens** (down from ~50,000 tokens in Phase 0).
  - C++14 refactoring waste was **0 tokens** (down from ~8,000 tokens in Phase 1).
  - Hook debugging waste was **0 tokens** (down from ~25,000 tokens in Phase 1).
  - The primary remaining token waste in Phase 2 came from **CRLF `edit_file` friction** and **searching 24,000+ lines of monolithic upstream files**.

---

## 5. Information That Would Have Improved the Output

1. **Canonical Roadmap Precedence**:
   - A brief note in the prompt or `continue.md` confirming that [`DOSBox-X_Kiosk_Implementation_Plan.md`](./DOSBox-X_Kiosk_Implementation_Plan.md) is the authoritative phase schedule.
2. **CRLF Line-Ending Warning for `edit_file`**:
   - Documenting in `AGENTS.md` that upstream files use CRLF (`\r\n`) and that multi-line `edit_file` targets will fail unless a CRLF-aware script or full-file `write_file` (for small kiosk files) is used.
3. **Splitting `/plan` and Execution Across `/compact` or Separate Sessions**:
   - Running `/compact` immediately after approving `implementation_plan.md` (before starting multi-file edits) would give the implementation phase a fresh context window and prevent mid-edit compaction.

---

## 6. Agents, Skills, and Hooks Evaluation & Recommended Improvements

### 6.1 Evaluation of Existing Customizations
| Customization | Type | Rating | Notes |
|---|---|---|---|
| `msbuild-runner` | Skill | **10/10** | Fast, zero warning noise, verified both SDL2 and legacy non-SDL2 builds cleanly. |
| `dosbox-config-tester` | Skill | **10/10** | The `-Tests` flag added after Phase 1 worked flawlessly for all 19 Google Tests. |
| `code-reviewer` | Subagent | **9.5/10** | Caught the per-frame SQLite query in `KioskUI` and minor test cleanups. Needs `define_subagent` called once per session before `invoke_subagent`. |
| `build-verifier` | Subagent | **8/10** | Defined in `.agents/agents/build-verifier.md`, but running builds via `build-verifier` subagent instead of direct `run_command` in the main agent would save main-thread turns. |
| `pre-commit.ps1` | Git Hook | **9/10** | Enforces non-SDL2 exclusions and C++14 headers cleanly. |

### 6.2 Recommended New & Improved Customizations

#### Recommendation 1: Add a CRLF-Safe Scoped Patch Script (`scripts/patch-lines.ps1`)
To eliminate `edit_file` CRLF failures and prevent single-line `Target` collisions in 10,000+ line upstream files (`sdlmain.cpp`, `bios.cpp`):
- Add a helper script `.agents/skills/msbuild-runner/scripts/patch-lines.ps1` that accepts `-FilePath`, `-StartLine`, `-EndLine`, and `-ReplacementFile` (or preserves the file's native CRLF/LF line endings while matching normalized multi-line blocks within a bounded `[StartLine..EndLine]` range).
- This guarantees 100% deterministic edits on CRLF files and makes accidental multi-location replacements impossible.

#### Recommendation 2: Add an "Upstream Integration Landmarks" Section to `AGENTS.md`
Document the exact functions and line neighborhoods in `src/gui/sdlmain.cpp` and `src/ints/bios.cpp` in `AGENTS.md`:
- `src/gui/sdlmain.cpp`:
  - `GFX_SetSize()` (~lines 1840–1970): OpenGL context creation/deletion & `KioskUI::InitializeBackend` / `ShutdownBackend`.
  - `GFX_Events()` (~lines 5960–6020): SDL2 event loop, `F12+D` / `SDL_CONTROLLER_BUTTON_GUIDE` overlay hotkey, and `KioskUI::ProcessEvent`.
  - `main()` (~lines 8535–8555, 10270–10410, 10780–10790): Kiosk init, `fresh_boot:`, `DOSBOX_RunMachine()`, `KioskGuestShutdownException` catch block, and Kiosk shutdown.
  - `SDL_GL_SwapBuffers()` (~lines 10945–10960): `KioskUI::CompositeActiveState()`.
- `src/ints/bios.cpp`:
  - `bochs_port_8900_write()` (~lines 1600–1635): Port `0x8900` shutdown trap.
  - `INT15_Handler()` (~lines 2745, 7580): ISA PnP (`AH=41h`) and APM (`AX=5307h CX=0003h`) power-off traps.
  - `BIOS::BIOS()` / `BIOS::~BIOS()` (~lines 10365–10415, 12880–12895): `BOCHS_PORT_8900` I/O handler lifecycle.

#### Recommendation 3: Audit `sdlmain.cpp` for Accidental Multi-Match Edits Before Commit
- During Step 2.4, replacing single-line `    SDL_GL_SwapWindow(sdl.window);` matched 6 occurrences in `src/gui/sdlmain.cpp` instead of only inside `SDL_GL_SwapBuffers(void)`.
- Before committing `feature/kiosk-phase-2`, inspect `git diff src/gui/sdlmain.cpp` and `git diff src/ints/bios.cpp` to ensure `KioskUI::GetInstance().CompositeActiveState()` is only called in the intended swap hook (or guarded appropriately) and remove any redundant `if (false) {}` scaffolding.

#### Recommendation 4: Use `/compact` Between Planning and Execution
- When a phase requires both architectural planning (`/plan`) and multi-file C++ implementation, run `/compact` immediately after approving the plan so the implementation starts with maximum context headroom.

---

## 7. Next Steps

1. Clean up the 5 extra `SDL_GL_SwapWindow(sdl.window)` replacements in [`src/gui/sdlmain.cpp`](../../src/gui/sdlmain.cpp) and the `if (false) {}` scaffolding in [`src/ints/bios.cpp`](../../src/ints/bios.cpp) and [`src/gui/sdlmain.cpp`](../../src/gui/sdlmain.cpp).
2. Commit Phase 2 on `feature/kiosk-phase-2` using Conventional Commits (`feat(kiosk): implement phase 2 lifecycle state machine and guest shutdown traps`).
3. Proceed to **Phase 3: Non-Destructive Ephemeral Boot Injector & Guest Return Mechanism** ([`Phase3_Ephemeral_Boot_Injector_Plan.md`](./Phase3_Ephemeral_Boot_Injector_Plan.md)).
