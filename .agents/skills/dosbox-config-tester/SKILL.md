---
name: dosbox-config-tester
description: >-
  Executes and validates DOSBox-X Kiosk Edition runtime behavior without GUI hangs or unquoted shell errors.
  Verifies Kiosk mode flag recognition, SQLite database bootstrapping, Dear ImGui lifecycle,
  and clean emulator shutdown. Use this skill whenever verifying runtime changes or debugging startup crashes.
---

# DOSBox Config Tester Skill

This skill automates testing DOSBox-X Kiosk Edition binaries on Windows. It redirects standard output and standard error from GUI subsystem binaries to temporary log buffers, eliminating quoting bugs, window hangs, and missed log outputs.

## When to Use This Skill
- After building the solution with `msbuild-runner` to verify that the generated binary launches and executes kiosk subsystems properly.
- When validating database loading (e.g. testing `-kiosk-db <path>` against custom catalog databases).
- When verifying configuration synthesis, kiosk UI startup, or clean exit cycles.

## Runner Script Reference
The runner script ([`scripts/test-kiosk.ps1`](./scripts/test-kiosk.ps1)) automatically handles:
1. Locating the compiled `dosbox-x.exe` (defaults to `bin/x64/Debug SDL2/dosbox-x.exe` or `bin/x64/Release SDL2/dosbox-x.exe`).
2. Executing with `-fastlaunch`, `-kiosk-mode`, and safe exit controls (`-exit` or `-time-limit <sec>`).
3. Intercepting GUI stdout/stderr streams cleanly.
4. Parsing kiosk initialization strings (SQLite version, ImGui context, subsystem ready signals).
5. Validating that the shutdown hook ran cleanly without crashes or memory corruption.

## Common Invocations

### 1. Default Quick Smoke Test
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1
```

### 2. Run Google Unit Tests
```powershell
# Run all Kiosk unit tests (defaults to -TestFilter "Kiosk*")
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests

# Run specific test filter
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests -TestFilter "KioskDatabaseFixture*"
```

### 3. Custom Catalog Database Test
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -KioskDb "C:\path\to\catalog.db"
```

### 4. Timed Runtime Test (e.g. run for 3 seconds instead of exiting immediately)
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -ExitImmediately:$false -TimeLimit 3
```

### 5. Test Specific Binary (e.g. Release Build)
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -ExecutablePath "bin/x64/Release SDL2/dosbox-x.exe"
```

## Parameter Reference

| Parameter | Type | Default | Description |
|---|---|---|---|
| `-ExecutablePath` | String | Auto-detected | Path to `dosbox-x.exe`. |
| `-Tests` | Switch | `false` | Runs Google Test suite headlessly with `-tests` and `-set waitonerror=false`. |
| `-TestFilter` | String | `"Kiosk*"` | Test filter expression passed to `--gtest_filter`. |
| `-KioskMode` | Switch | `true` (when not `-Tests`) | Passes `-kiosk-mode`. |
| `-KioskDb` | String | `""` | Passes `-kiosk-db <path>`. |
| `-KioskConf` | String | `""` | Passes `-kiosk-conf <path>`. |
| `-TimeLimit` | Int | `5` | Time in seconds before emulator exits (when `-ExitImmediately` is false). |
| `-FastLaunch` | Switch | `true` | Passes `-fastlaunch` to bypass BIOS screen delay. |
| `-ExitImmediately` | Switch | `true` | Passes `-exit` to exit cleanly after AUTOEXEC. |
| `-AdditionalArgs` | Array | `@()` | Extra arguments to pass to DOSBox-X. |

## Expected Output
```text
=================================================================
[dosbox-config-tester] Executable: C:\code\dosbox-x-kiosk\bin\x64\Debug SDL2\dosbox-x.exe
[dosbox-config-tester] Arguments:  -kiosk-mode -fastlaunch -exit
=================================================================
-----------------------------------------------------------------
[dosbox-config-tester] Exit Code:        0
[dosbox-config-tester] Runtime:          0.85s
[dosbox-config-tester] Flag Detected:    PASS
[dosbox-config-tester] SQLite Catalog:   ':memory:' (SQLite v3.46.1)
[dosbox-config-tester] ImGui UI Context: PASS
[dosbox-config-tester] Subsystem Ver:    SQLite 3.46.1, ImGui 1.93.1 WIP
[dosbox-config-tester] Clean Shutdown:   PASS
-----------------------------------------------------------------
[dosbox-config-tester] OVERALL STATUS: PASS
=================================================================
```
