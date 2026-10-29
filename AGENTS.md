# AGENTS.md — DOSBox-X Kiosk Edition Guidelines

This document outlines workspace-specific rules, toolchain requirements, and standard operating procedures for agents working on the DOSBox-X Kiosk Edition codebase.

---

## 1. Project Overview & Environments
- **Project**: Turn-key retro computing appliance fork of DOSBox-X.
- **Target Host**: Embedded Linux appliance via Buildroot (`BR2_PACKAGE_DOSBOX_X_KIOSK=y`).
- **Development & Verification Host**: Windows (Visual Studio 2022 Community v18, PlatformToolset `v145`, Windows SDK `10.0.26100.0`).
- **Core Toggles**: `C_KIOSK` (C++ preprocessor) and `--enable-kiosk` (Autotools). Requires SDL2.

---

## 2. Standard Build & Verification Procedures (Token Conservation)

> [!IMPORTANT]
> Never run unfiltered MSBuild from the command line. Third-party dependencies (such as FluidSynth) emit >1,100 warnings that flood the context window (~50,000 wasted tokens per run). Always use the custom skills below.

### 2.1 Compiling with `msbuild-runner`
Use the custom skill script:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64"
```
- **Clean rebuild**: Add `-Clean` switch.
- **Legacy non-SDL2 regression verification**: Run with `-Configuration "Debug" -Platform "x64"`.

### 2.2 Verifying with `dosbox-config-tester`
Use the custom test skill script to run DOSBox-X with redirected I/O:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1
```
- Validates Kiosk CLI flags, SQLite catalog connection, Dear ImGui context, and clean shutdown without window hangs or shell quoting errors.

---

## 3. Architecture & Code Quality Constraints

1. **Upstream & Non-SDL2 Backward Compatibility**:
   - Never break non-kiosk or non-SDL2 configurations (`Debug|Win32`, `Release|x64`).
   - Guard all kiosk code with `#if defined(C_KIOSK)`.
   - In `vs/dosbox-x.vcxproj`, ensure any kiosk/imgui compilation units include:
     `<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>`.
2. **Two-Phase GUI Initialization**:
   - `KioskUI` must maintain clean separation between headless context setup (`InitializeContext()`) and backend window/GL attachment (`InitializeBackend()`).
   - Pass `nullptr` to `ImGui_ImplOpenGL3_Init(nullptr)` to enable GLSL auto-detection across desktop OpenGL and GLES.
3. **Database Fault Tolerance**:
   - All SQLite database file openings must handle file lock / permission errors and fall back gracefully to in-memory databases (`:memory:`).
   - Use prepared statements and parameterized bindings for all dynamic queries.
4. **Header Discipline**:
   - Always include `"config.h"` at the very top of `.cpp` files before any DOSBox-X or kiosk headers.
   - Guard against C++17/C++20 feature creep; maintain clean compatibility with MSVC and GCC/Clang C++14 standards.

---

## 4. Phase Verification & Commit Lifecycle
Before committing changes at each phase milestone:
1. **Build Verification**: Run `build.ps1` for both `Debug SDL2|x64` and `Debug|x64`.
2. **Runtime Verification**: Run `test-kiosk.ps1` to verify clean startup and shutdown.
3. **Pre-Commit Code Review Subagent**:
   - Spawn a dedicated `research` subagent to perform an adversarial review of `git diff master`.
   - Remediate all critical, portability, and concurrency findings.
4. **Commit**: Use Conventional Commits (`feat(kiosk): ...`, `fix(kiosk): ...`).
