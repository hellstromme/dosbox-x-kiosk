# DOSBox-X Kiosk Edition: Phase 1 Retrospective

> **Date:** October 29, 2026  
> **Milestone:** Phase 1 — Catalog Database & Hardware Archetype Engine  
> **Commits:** [`4ea5f1f13`](file:///C:/code/dosbox-x-kiosk) (`feat(kiosk)`), [`fdf4ef8ee`](file:///C:/code/dosbox-x-kiosk) (`docs(kiosk)`)  
> **Author:** Antigravity / Pair Programming Session  

---

## 1. Executive Summary

Phase 1 delivered the data and configuration core of the DOSBox-X Kiosk Edition appliance:
- **Relational Catalog Database**: Implemented [`KioskDatabase`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_db.h) managing 7 normalized SQLite tables (`kiosk_config`, `virtual_machines`, `catalog_items`, `media_pool`, `session_caddy`, `os_distributions`, and `software_signatures`) with thread-safe mutex guards, prepared statements, and automatic `:memory:` fallback on inaccessible paths or open errors.
- **Hardware Archetype Engine**: Implemented [`ArchetypeManager`](file:///C:/code/dosbox-x-kiosk/src/kiosk/machine/archetypes.h) defining 5 historical PC profiles (`ARCH_XT_8088` through `ARCH_PENTIUM_3DFX`), rule-based OS compatibility validation, and hardware section generation.
- **Hierarchical Configuration Synthesizer**: Implemented [`ConfigSynthesizer`](file:///C:/code/dosbox-x-kiosk/src/kiosk/machine/config_synthesizer.h) supporting 4-tier layer merging and dynamic `[autoexec]` generation for both `REAL_HDD_BOOT` (raw disk images, IDE/geometry configuration) and `INTEGRATED_DOS` (folder mount, auto-launch, caddy floppies/CDs, exit semantics).
- **Automated Verification**: Registered 13 Google Test cases in [`tests/kiosk_tests.cpp`](file:///C:/code/dosbox-x-kiosk/tests/kiosk_tests.cpp), passing 100% on `Debug SDL2|x64` with 0 failures, while preserving legacy non-SDL2 compatibility on `Debug|x64` (0 build errors).
- **Pre-Commit Adversarial Review**: Spawned subagent `code-reviewer` (`ff48bb49-af7c-43e7-90b3-76d2198da82b`), remediated 7 distinct architectural and syntax issues, and obtained official **CERTIFIED CLEAN (PASSED)** sign-off prior to commit.

This retrospective reviews what worked well, key friction points, token inefficiencies, input gaps, and architectural improvements across agents, skills, and hooks.

---

## 2. What Worked Well

### 2.1 The Two-Tier Custom Tooling Architecture (`msbuild-runner` & `dosbox-config-tester`)
- In Phase 0, unfiltered MSBuild dumps consumed ~50,000 tokens per run due to 1,100+ legacy third-party compiler warnings.
- In Phase 1, using the custom skill script:
  ```powershell
  powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64"
  ```
  eliminated warning floods entirely, compiling the solution in **8.80s** with concise, token-efficient filtered status lines.
- Non-SDL2 backward compatibility was validated in **1.38s** without noisy log dumps.

### 2.2 Autonomous Adversarial Code Review Subagent
- Deploying the dedicated `code-reviewer` subagent ([spec](file:///C:/code/dosbox-x-kiosk/.agents/agents/code-reviewer.md)) in an isolated conversation context was highly effective.
- **Critical issues caught before commit:**
  1. **Fault Tolerance Gap**: `sqlite3_open` and `InitializeSchema` failed silently without falling back to `:memory:` if an invalid or read-only path was supplied.
  2. **XML Schema Malformation**: In [`vs/dosbox-x.vcxproj.filters`](file:///C:/code/dosbox-x-kiosk/vs/dosbox-x.vcxproj.filters), `<ClCompile>` and `<ClInclude>` elements had been placed outside `<ItemGroup>` tags, which would corrupt Visual Studio's Solution Explorer.
  3. **Autoexec Space Quoting**: Guest executables with spaces (`MY GAME.EXE`) were unquoted in synthesized batch files, causing DOS command parser failures.
  4. **Caddy Incompleteness**: Session caddy floppies (`IMGMOUNT 0`/`1`) and secondary optical discs (`IMGMOUNT E`) were omitted from `INTEGRATED_DOS` autoexec synthesis.
  5. **Test Completeness**: An explicit test for database fallback under an unwriteable directory (`InaccessiblePathFallsBackToMemory`) was missing.
- After all findings were remediated, a re-review independently verified the diff against `master` and awarded **CERTIFIED CLEAN (PASSED)**.

### 2.3 Integrated Unit Testing Harness
- Leveraging DOSBox-X's internal Google Test framework by registering [`tests/kiosk_tests.cpp`](file:///C:/code/dosbox-x-kiosk/tests/kiosk_tests.cpp) conditionally under `#if defined(C_KIOSK)` allowed instant regression testing without external harnesses or mock libraries.
- The 13 tests ran in under 1 second, providing immediate confidence in CRUD queries, archetype generation, and synthesizer rules.

---

## 3. What We Struggled With

### 3.1 C++ Standard Baseline: C++14 vs C++17
- The workspace targets MSVC with default `/std:c++14` compatibility.
- Initial data model signatures employed `std::optional` for nullable database columns (e.g. `std::optional<std::string>`).
- MSVC immediately rejected compilation with:
  ```text
  error C2039: 'optional': is not a member of 'std'
  ```
- **Resolution:** Replaced `std::optional` with boolean out-parameters (`bool Get...(..., T& out_record)`) and nullable pointers (`const CatalogItemRecord*`), strictly compliant with C++14.

### 3.2 DOSBox-X Windows Subsystem & Interactive Console Hangs
- DOSBox-X on Windows is compiled as a Win32 GUI subsystem binary (`/SUBSYSTEM:WINDOWS`).
- When invoked from the command line with `-tests`, it allocates a temporary console. However, by default, `sdl.wait_on_error` causes it to call `DOSBox_ConsolePauseWait()` on exit or error, blocking indefinitely for a keystroke.
- Initial test runs hung waiting for standard input.
- **Resolution:** Discovered and standardized the headless execution parameters:
  ```powershell
  & "bin\x64\Debug SDL2\dosbox-x.exe" -tests --gtest_filter=Kiosk* -set waitonerror=false -fastlaunch -exit < $null
  ```

### 3.3 AGY Pre-Tool Hook Protocol Violation & PowerShell Subexpression Collision
- Two interrelated issues blocked `git commit` at the end of the phase:
  1. **PowerShell Subexpression in Pre-Commit Hook**: Inside [`.githooks/pre-commit.ps1`](file:///C:/code/dosbox-x-kiosk/.githooks/pre-commit.ps1), double-quoted string `"ExcludedFromBuild Condition=""!$(Configuration.Contains('SDL2'))"""` caused PowerShell to interpret `$(Configuration.Contains('SDL2'))` as an executable subexpression cmdlet, halting execution with:
     ```text
     The term 'Configuration.Contains' is not recognized as the name of a cmdlet.
     ```
  2. **Pre-Tool Hook Standard Output Contamination**: The AGY PreToolUse hook [`.agents/hooks/pre-tool-enforcer.ps1`](file:///C:/code/dosbox-x-kiosk/.agents/hooks/pre-tool-enforcer.ps1) invoked `Start-Process powershell ... -NoNewWindow`. Because child stdout was not redirected, pre-commit console banner text (`[pre-commit] Checking staged changes...`) leaked into the hook's standard output alongside the JSON response (`{"decision":"allow"}`). This broke the protojson unmarshaler in the Antigravity CLI runtime:
     ```text
     failed to unmarshal result from hook via protojson: [pre-commit] Checking staged changes... {"decision":"allow"}: proto: syntax error: unexpected token [
     ```
  3. **Working Directory Drift**: When invoked via the hook, the child process executed with working directory `C:\code\dosbox-x-kiosk\.agents`, causing relative path queries (`Get-Content "vs/dosbox-x.vcxproj"`) to fail.
- **Resolution:**
  - Fixed string escaping in [`.githooks/pre-commit.ps1`](file:///C:/code/dosbox-x-kiosk/.githooks/pre-commit.ps1) using single quotes `'...'` and dynamic repo root resolution (`git rev-parse --show-toplevel`).
  - Updated [`.agents/hooks/pre-tool-enforcer.ps1`](file:///C:/code/dosbox-x-kiosk/.agents/hooks/pre-tool-enforcer.ps1) to redirect child process stdout/stderr to `$env:TEMP` and pass explicit `-WorkingDirectory`.

---

## 4. Token Waste & Efficiency Analysis

```
+-----------------------------------------------------------------------+
| Estimated Token Waste Breakdown                                       |
+-----------------------------------------------------------------------+
| 1. Hook Protocol & Escaping Debug Cycles:        ~20,000 - 30,000 tokens|
| 2. Large Visual Studio Project File Inspections: ~15,000 - 20,000 tokens|
| 3. C++14 Refactoring Cycles (std::optional):      ~6,000 -  8,000 tokens|
| 4. Subagent Status Checking Post-Compaction:      ~4,000 -  6,000 tokens|
+-----------------------------------------------------------------------+
```

### 4.1 Hook Debugging Cycles
- Multiple failed `git commit` attempts and inspection of raw hook scripts consumed several command/tool call rounds.
- **Mitigation:** Pre-tool hooks must always redirect stdout/stderr to files and output strictly valid, single-line JSON.

### 4.2 Large `.vcxproj` File Inspections
- [`vs/dosbox-x.vcxproj`](file:///C:/code/dosbox-x-kiosk/vs/dosbox-x.vcxproj) is 2,114 lines (154KB) and [`vs/dosbox-x.vcxproj.filters`](file:///C:/code/dosbox-x-kiosk/vs/dosbox-x.vcxproj.filters) is 2,901 lines (118KB).
- Grepping or reading wide line ranges to ensure XML nodes were correctly structured consumed substantial context tokens.
- **Mitigation:** Automate project file updates via targeted PowerShell XML manipulation or a specialized skill.

---

## 5. Information That Would Have Improved the Output

1. **Explicit C++ Language Standard Constraint**:
   - Stating upfront: *"The repository is strictly C++14 compliant across MSVC and GCC. Do not use C++17 library types (`std::optional`, `std::string_view`, `std::variant`, `std::filesystem`)."*
2. **Hook Execution Details**:
   - Informing the agent that AGY PreToolUse hooks (`.agents/hooks.json`) actively intercept `run_command` invocations for `msbuild` and `git commit`, requiring zero stdout pollution from scripts.
3. **Canonical Headless Unit Test Invocation**:
   - Providing the exact arguments needed to prevent DOSBox-X from creating an interactive console pause during automated test runs.

---

## 6. Agents, Skills, and Hooks Evaluation & Recommended Improvements

### 6.1 Evaluation of Existing Tooling
- **`msbuild-runner` Skill (Rating: 10/10)**:
  - Outstanding performance. Filtered warning spam completely and executed in <10 seconds.
- **`dosbox-config-tester` Skill (Rating: 8/10)**:
  - Successfully verified headless startup, catalog connection, and clean shutdown.
  - *Gap:* Currently only tests kiosk CLI flags, not the unit test suite (`-tests`).
- **`code-reviewer` Subagent (Rating: 10/10)**:
  - Highly effective. Caught 5 subtle logic and architecture flaws that human reviewers might have missed.
- **`kiosk-governance` Hook (Rating: 7/10)**:
  - High utility in preventing non-compliant commits and enforcing MSBuild token limits.
  - *Gap:* Vulnerable to stdout contamination if child processes print unredirected text.

---

## 7. Actionable Recommendations for Future Phases (Phase 2+)

### Recommendation 1: Expand `dosbox-config-tester` to Run Google Tests
Enhance [`.agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1`](file:///C:/code/dosbox-x-kiosk/.agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1) to accept a `-Tests` switch:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests -Filter "Kiosk*"
```
This encapsulates the `-set waitonerror=false -fastlaunch -exit < $null` flags and parses Google Test output into a compact 1-line summary: `[TESTS PASSED: 13/13]`.

### Recommendation 2: Create a `vcxproj-updater` Skill / Script
Create a helper script in `.agents/skills/msbuild-runner/scripts/add-source.ps1` that uses PowerShell's `[xml]` parser to add `<ClCompile>` and `<ClInclude>` elements to both `.vcxproj` and `.vcxproj.filters` automatically, guaranteeing:
- Elements are always placed inside `<ItemGroup>`.
- The `<ExcludedFromBuild Condition="!$(Configuration.Contains('SDL2'))">true</ExcludedFromBuild>` guard is applied without manual string editing.

### Recommendation 3: Pre-Tool Hook Standard Output Sanitization Policy
Ensure all scripts registered under `.agents/hooks.json` follow strict stream isolation:
- Child processes must use `-RedirectStandardOutput` and `-RedirectStandardError`.
- Only the final `{"decision":"allow"}` or `{"decision":"deny","reason":"..."}` JSON string may be emitted to stdout.

### Recommendation 4: Add C++14 Linter to Pre-Commit Hook
Update [`.githooks/pre-commit.ps1`](file:///C:/code/dosbox-x-kiosk/.githooks/pre-commit.ps1) to check staged files for forbidden C++17 headers (`<optional>`, `<string_view>`, `<variant>`, `<filesystem>`) and reject them immediately before compilation.

---

## 8. Conclusion

Phase 1 established a robust data and machine synthesis foundation for DOSBox-X Kiosk Edition. The custom build tooling and adversarial code review subagent proved indispensable for maintaining code quality, preserving legacy compatibility, and conserving token bandwidth.

By implementing the test runner expansion, XML project helper, and C++14 pre-commit guard, Phase 2 (ImGui Carousel Frontend & Skeuomorphic Caddy UI) can proceed with even greater speed and token efficiency.
