---
name: msbuild-runner
description: >-
  Builds DOSBox-X Kiosk Edition on Windows using MSBuild with automated toolchain
  discovery (vswhere, v145/v143 toolsets, Windows SDK), token-efficient filtered logging,
  automated project registration (add-source.ps1), and CRLF-safe line-bounded patching (patch-lines.ps1).
  Use this skill whenever compiling the solution, registering source files, or patching large CRLF files.
---

# MSBuild Runner Skill

This skill automates building the DOSBox-X Kiosk Edition Visual Studio solution (`vs/dosbox-x.sln`) with dynamic toolchain discovery, filtered compiler output, XML-safe `.vcxproj` registration, and CRLF-safe bounded file patching.

## When to Use This Skill
- Whenever building or verifying changes in the DOSBox-X codebase on Windows.
- When performing verification across multiple configurations (`Debug SDL2|x64` and legacy `Debug|x64`).
- When registering new `.cpp` or `.h` files in `vs/dosbox-x.vcxproj` (`scripts/add-source.ps1`).
- When making multi-line or line-scoped edits to large CRLF upstream files like `src/gui/sdlmain.cpp` or `src/ints/bios.cpp` (`scripts/patch-lines.ps1`).

## Toolchain Auto-Discovery
The runner script ([`scripts/build.ps1`](./scripts/build.ps1)) automatically handles:
1. Locating the current `MSBuild.exe` via `vswhere.exe` (supporting VS 2022 and VS 18 preview).
2. Detecting the highest installed Windows 10/11 SDK in `C:\Program Files (x86)\Windows Kits\10\Include`.
3. Selecting the matching PlatformToolset (`v145` or `v143`).
4. Applying `/clp:ErrorsOnly` to suppress over 1,100 third-party compiler warnings (e.g., FluidSynth), saving ~50,000 tokens per build invocation.

## Common Invocations

### 1. Standard SDL2 Build (Recommended for Kiosk Development)
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64"
```

### 2. Upstream Legacy Build (Regression Verification)
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug" -Platform "x64"
```

### 3. Clean Rebuild
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64" -Clean
```

### 4. Release SDL2 Build
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Release SDL2" -Platform "x64"
```

### 5. Adding New Source or Header Files
Use `scripts/add-source.ps1` to automatically register new C++ files in `vs/dosbox-x.vcxproj` and `vs/dosbox-x.vcxproj.filters` with proper non-SDL2 exclusion guards and XML ItemGroup structure:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/add-source.ps1 -FilePath "src/kiosk/ui/my_new_widget.cpp"
```

### 6. CRLF-Safe Line-Bounded File Patching
Upstream DOSBox-X files (`src/gui/sdlmain.cpp`, `src/ints/bios.cpp`) use Windows CRLF (`\r\n`) line endings and exceed 10,000 lines. Use `scripts/patch-lines.ps1` to replace a line range `[StartLine..EndLine]` or match a multi-line target inside `[StartLine..EndLine]` while preserving native CRLF/LF line endings:
```powershell
# Replace exact 1-indexed line range [StartLine..EndLine] from a replacement file or string:
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/patch-lines.ps1 -FilePath "src/ints/bios.cpp" -StartLine 10363 -EndLine 10375 -ReplaceEntireRange -ReplacementFile "patch.txt"

# Or match a Target string/file within [StartLine..EndLine]:
powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/patch-lines.ps1 -FilePath "src/gui/sdlmain.cpp" -StartLine 10940 -EndLine 10965 -Target "    SDL_GL_SwapWindow(sdl.window);" -ReplacementFile "swap_patch.txt"
```

## Parameter Reference (`build.ps1`)

| Parameter | Type | Default | Description |
|---|---|---|---|
| `-Configuration` | String | `"Debug SDL2"` | Build configuration (`"Debug SDL2"`, `"Release SDL2"`, `"Debug"`, `"Release"`). |
| `-Platform` | String | `"x64"` | Target architecture (`"x64"` or `"Win32"`). |
| `-PlatformToolset` | String | Auto-detected | Force a specific MSVC toolset (e.g., `"v145"`). |
| `-WindowsTargetPlatformVersion` | String | Auto-detected | Force a specific Windows SDK (e.g., `"10.0.26100.0"`). |
| `-SolutionPath` | String | `"vs/dosbox-x.sln"` | Path to the Visual Studio solution file. |
| `-Clean` | Switch | `false` | Executes target `Clean,Build` instead of incremental `Build`. |
| `-ShowWarnings` | Switch | `false` | Shows compiler warnings (disabled by default to prevent context flooding). |

## Interpreting Output
- **Exit Code 0**: Build succeeded cleanly. Executable is output to `bin/<Platform>/<Configuration>/dosbox-x.exe`.
- **Exit Code 1+**: Build failed. The filtered error list will be printed directly, highlighting the source file, line number, and error code (e.g., `error C2653`).
