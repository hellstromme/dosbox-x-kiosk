# AGENTS.md — DOSBox-X Kiosk Edition Guidelines

This document outlines workspace-specific rules, toolchain requirements, and standard operating procedures for agents working on the DOSBox-X Kiosk Edition codebase.

---

## 1. Project Overview & Environments
- **Project**: Turn-key retro computing appliance fork of DOSBox-X.
- **Target Host**: Embedded Linux appliance via Buildroot (`BR2_PACKAGE_DOSBOX_X_KIOSK=y`).
- **Development & Verification Host**: Windows (Visual Studio 2022 Community v18, PlatformToolset `v145`, Windows SDK `10.0.26100.0`).
- **Core Toggles**: `C_KIOSK` (C++ preprocessor) and `--enable-kiosk` (Autotools). Requires SDL2.
- **Canonical Roadmap**: [`docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md`](docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md) is the authoritative phase schedule.

---

## 2. Standard Build, Editing & Verification Procedures (Token Conservation)

> [!IMPORTANT]
> Never run unfiltered MSBuild from the command line. Third-party dependencies (such as FluidSynth) emit >1,100 warnings that flood the context window (~50,000 wasted tokens per run). Always use the custom skills below.

### 2.1 Compiling with `msbuild-runner`
Use the custom skill script:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64"
```
- **Clean rebuild**: Add `-Clean` switch.
- **Legacy non-SDL2 regression verification**: Run with `-Configuration "Debug" -Platform "x64"`.
- **Registering new `.cpp`/`.h` files**: Run `.agents/skills/msbuild-runner/scripts/add-source.ps1 -FilePath "src/kiosk/..."`.

### 2.2 Verifying with `dosbox-config-tester`
Use the custom test skill script to run DOSBox-X with redirected I/O:
```powershell
# Runtime CLI & subsystem smoke test
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1

# Google Test suite execution (headless XML output)
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests -TestFilter "Kiosk*"
```
- Validates Kiosk CLI flags, SQLite catalog connection, Dear ImGui context, unit test suite, and clean shutdown without window hangs or shell quoting errors.

### 2.3 Editing CRLF Upstream Files with `patch-lines.ps1`
- Upstream DOSBox-X files (`src/gui/sdlmain.cpp`, `src/ints/bios.cpp`) use Windows CRLF (`\r\n`) line endings and exceed 10,000 lines.
- Passing multi-line `Target` strings to `edit_file` on CRLF files will fail due to LF (`\n`) normalization, and passing single-line `Target` strings to `edit_file` replaces **every** match in the file.
- For multi-line or scoped edits in large CRLF files, use `.agents/skills/msbuild-runner/scripts/patch-lines.ps1` bounded by `-StartLine` and `-EndLine`:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/patch-lines.ps1 -FilePath "src/gui/sdlmain.cpp" -StartLine 10940 -EndLine 10965 -ReplaceEntireRange -ReplacementFile "patch.txt"
```

### 2.4 Specialized Custom Subagents
Two dedicated subagents are configured to isolate heavy context operations:
- **`build-verifier`** ([spec](.agents/agents/build-verifier.md)): Runs MSBuild and runtime testing in an isolated subagent context, shielding the primary conversation from token-heavy compiler logs.
- **`code-reviewer`** ([spec](.agents/agents/code-reviewer.md)): Conducts pre-commit adversarial reviews against the architecture constraints below.
  - **IMPORTANT (Windows Sandbox ACL Rule)**: Because `code-reviewer` and `research` run with `EnableWriteTools: false` (read-only sandbox), invoking them with `Workspace: inherit` downgrades the shared Windows AppContainer SID ACL on the workspace directory to read-only for subsequent `run_command` calls in the same session. Always run `build.ps1` and `test-kiosk.ps1` **before** invoking `code-reviewer`, or use native `edit_file` / `write_file` tools (which run in the host process) after a read-only subagent completes.

---

## 3. Architecture & Code Quality Constraints

1. **Upstream & Non-SDL2 Backward Compatibility**:
   - Never break non-kiosk or non-SDL2 configurations (`Debug|Win32`, `Release|x64`).
   - Guard all kiosk code with `#if defined(C_KIOSK)`.
   - In `vs/dosbox-x.vcxproj`, ensure any kiosk/imgui compilation units include:
     `<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>`.
2. **Two-Phase GUI Initialization**:
   - `KioskUI` must maintain clean separation between headless context setup (`InitializeContext()`) and backend window/GL attachment (`InitializeBackend()` / `ShutdownBackend()`).
   - Pass `nullptr` to `ImGui_ImplOpenGL3_Init(nullptr)` to enable GLSL auto-detection across desktop OpenGL and GLES.
3. **Database Fault Tolerance & Caching**:
   - All SQLite database file openings must handle file lock / permission errors and fall back gracefully to in-memory databases (`:memory:`).
   - Use prepared statements and parameterized bindings for all dynamic queries.
   - Never execute SQLite queries per-frame inside 60 Hz ImGui render loops; cache query results and invalidate on state/catalog mutation.
4. **Header Discipline**:
   - Always include `"config.h"` at the very top of `.cpp` files before any DOSBox-X or kiosk headers.
   - Guard against C++17/C++20 feature creep; maintain clean compatibility with MSVC and GCC/Clang C++14 standards.

---

## 4. Upstream Integration Landmarks (Token-Saving Index)

To avoid scanning 24,000+ lines of monolithic upstream source files during future phases, jump directly to these line neighborhoods:

### 4.1 `src/gui/sdlmain.cpp` (~10,980 lines)
- **Lines 175–185**: `#if defined(C_KIOSK)` header includes (`kiosk_main.h`, `kiosk_db.h`, `ui/kiosk_ui.h`).
- **Lines 1840–1980 (`GFX_SetSize`)**: OpenGL context lifecycle (`SDL_GL_DeleteContext`, `SDL_GL_CreateContext`, `SDL_GL_MakeCurrent`) and `KioskUI::InitializeBackend` / `ShutdownBackend`.
- **Lines 5960–6020 (`GFX_Events`)**: `while (SDL_PollEvent(&event))` loop, `F12+D` / `SDL_CONTROLLER_BUTTON_GUIDE` runtime overlay hotkey routing, `KioskManager::HandlePreBootInputEvent`, and `KioskUI::ProcessEvent`.
- **Lines 8450–8560 (`main` CLI & Early Init)**: `-kiosk-mode`, `-kiosk-db`, `-kiosk-conf` flag parsing and `KioskManager::Initialize()`.
- **Lines 10270–10420 (`fresh_boot:` & `DOSBOX_RunMachine`)**: `KioskManager::RunMainLoop()`, `DOSBOX_RunMachine()`, and `catch (const KioskGuestShutdownException& e)`.
- **Lines 10810–10830 (`main` Shutdown)**: `KioskManager::Shutdown()`.
- **Lines 10945–10975 (`SDL_GL_SwapBuffers`)**: `KioskUI::CompositeActiveState()` before `SDL_GL_SwapWindow(sdl.window)`.

### 4.2 `src/ints/bios.cpp` (~13,530 lines)
- **Lines 55–62**: `#if defined(C_KIOSK)` include for `kiosk/kiosk_main.h`.
- **Lines 1595–1635 (`bochs_port_8900_write`)**: Bochs Port `0x8900` `"Shutdown"` sequence state machine and `KioskGuestShutdownException(KioskShutdownReason::PORT_8900_SHUTDOWN)` trap.
- **Lines 2740–2755 (`INT15_Handler` `case 0x41`)**: ISA PnP OS power-off trap (`KioskShutdownReason::PNP_POWER_OFF`).
- **Lines 7575–7595 (`INT15_Handler` `case 0x53` / `0x07` / `0x03`)**: APM BIOS power-off trap (`KioskShutdownReason::APM_POWER_OFF`).
- **Lines 10360–10415 (`BIOS::BIOS`)**: `BOCHS_PORT_8900` cleanup and `Install(0x8900, bochs_port_8900_write, IO_MB)`.
- **Lines 12880–12900 (`BIOS::~BIOS`)**: `BOCHS_PORT_8900` destructor cleanup.

---

## 5. Phase Verification & Commit Lifecycle
Before committing changes at each phase milestone:
1. **Build & Runtime Verification**: Invoke `build-verifier` (or run `build.ps1` and `test-kiosk.ps1`).
2. **Pre-Commit Code Review**: Invoke `code-reviewer` to perform an adversarial review of `git diff master`.
3. **Remediation**: Remediate all critical, portability, and concurrency findings.
4. **Commit**: Use Conventional Commits (`feat(kiosk): ...`, `fix(kiosk): ...`).
