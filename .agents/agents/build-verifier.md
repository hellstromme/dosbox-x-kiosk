# Agent: `build-verifier`

## Role & Purpose
The **Build and Verification Subagent** isolates build executions, toolchain diagnostics, and runtime tests in a separate agent context. This ensures that compiler warning dumps (such as FluidSynth's >1,100 warnings) and runtime logs never pollute the primary agent's context window.

---

## Agent Configuration
- **Type Name**: `build-verifier`
- **Model**: `inherit` (or `flash` for fast status checks)
- **Permissions**:
  - `EnableWriteTools`: `true` (executes PowerShell skill scripts)
  - `EnableSubagentTools`: `false`
  - `EnableMCPTools`: `false`

---

## Standard Capabilities & Workflow
1. **Compilation Verification**:
   Runs compilation via `msbuild-runner` skill:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug SDL2" -Platform "x64"
   ```
2. **Regression Verification**:
   Runs non-SDL2 legacy build to ensure upstream compatibility:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .agents/skills/msbuild-runner/scripts/build.ps1 -Configuration "Debug" -Platform "x64"
   ```
3. **Runtime Verification**:
   Runs runtime lifecycle checks via `dosbox-config-tester` skill:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1
   ```
4. **Filtered Response**:
   Extracts and returns only the concise summary and any fatal error lines back to the parent agent.

---

## Invocation Example
```json
{
  "TypeName": "build-verifier",
  "Role": "Build and Test Verifier",
  "Prompt": "Verify current codebase with a full build of Debug SDL2|x64 and run the dosbox-config-tester smoke test. Return only the summary and any failure lines."
}
```
