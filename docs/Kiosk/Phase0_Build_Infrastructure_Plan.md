# Phase 0 Implementation Plan: Build Infrastructure & Core Dependencies

**Project:** DOSBox-X Kiosk Edition  
**Target Firmware Host:** Buildroot Linux Appliance  
**Development & Verification Platform:** Windows 10/11 (Visual Studio 2022/MSBuild, SDL2 x64) + Linux Autotools (`configure.ac` / `Makefile.am`)  
**Document Status:** Detailed Phase Execution Plan  
**Date:** October 28, 2026  

---

## 1. Objective & Scope of Phase 0

Phase 0 establishes the compile-time and link-time foundation for the DOSBox-X Kiosk fork without modifying any existing non-kiosk behavior or breaking generic upstream builds.

At the conclusion of Phase 0:
1. `C_KIOSK` / `ENABLE_KIOSK` build toggles will be defined for both MSBuild (Visual Studio) and Autotools (Linux / Buildroot).
2. **SQLite 3.4x** (amalgamation) will be vendored into `src/libs/sqlite3/`, configured for WAL mode, JSON1, thread safety, and integrated into the project build trees.
3. **Dear ImGui** (docking/modern branch) with SDL2 and OpenGL3 backends will be vendored into `src/libs/imgui/` and integrated into the project build trees.
4. **Header Utilities** (`stb_image_write.h`) will be integrated into `src/libs/` for image/icon export.
5. The core kiosk directory structure (`src/kiosk/`, `src/kiosk/ui/`, `src/kiosk/machine/`, `src/kiosk/ingest/`) will be created with skeleton classes.
6. A minimal command-line hook in `src/dosbox.cpp` will recognize `--kiosk-mode`, verifying that SQLite3 and ImGui initialize cleanly in Windows development builds.

---

## 2. Component Breakdown & Source Layout

```
dosbox-x-kiosk/
├── src/
│   ├── libs/
│   │   ├── sqlite3/
│   │   │   ├── sqlite3.h              # SQLite 3.4x API header
│   │   │   ├── sqlite3.c              # Amalgamated C implementation
│   │   │   └── Makefile.am            # Autotools library target
│   │   │
│   │   ├── imgui/
│   │   │   ├── imconfig.h             # DOSBox-X ImGui configuration
│   │   │   ├── imgui.h / .cpp         # Dear ImGui core
│   │   │   ├── imgui_draw.cpp
│   │   │   ├── imgui_tables.cpp
│   │   │   ├── imgui_widgets.cpp
│   │   │   ├── imgui_internal.h
│   │   │   ├── imstb_rectpack.h
│   │   │   ├── imstb_textedit.h
│   │   │   ├── imstb_truetype.h
│   │   │   ├── backends/
│   │   │   │   ├── imgui_impl_sdl2.h / .cpp
│   │   │   │   ├── imgui_impl_opengl3.h / .cpp
│   │   │   │   └── imgui_impl_opengl3_loader.h
│   │   │   └── Makefile.am
│   │   │
│   │   └── stb/
│   │       └── stb_image_write.h      # Single-header PNG exporter for icons
│   │
│   └── kiosk/                         # Isolated Kiosk Subsystem
│       ├── kiosk_types.h              # Shared enums (KioskState, LaunchMode, etc.)
│       ├── kiosk_main.h / .cpp        # Skeleton KioskManager singleton
│       ├── kiosk_db.h / .cpp          # Skeleton KioskDatabase (verifies SQLite3)
│       └── ui/
│           ├── kiosk_ui.h / .cpp      # Skeleton KioskUI (verifies Dear ImGui)
│           └── input_router.h / .cpp  # Input abstraction stub
│
├── vs/
│   ├── config.h                       # Defines C_KIOSK=1 for MSVC builds
│   ├── dosbox-x.vcxproj               # Adds kiosk & libs includes and compile items
│   └── dosbox-x.vcxproj.filters       # Visual Studio solution filters
│
├── configure.ac                       # Adds --enable-kiosk option
└── src/Makefile.am                    # Adds conditional compilation for kiosk & libs
```

---

## 3. Step-by-Step Execution Plan

### Step 0.1: Configuration Flags & Preprocessor Definitions
- **Visual Studio (`vs/config.h`):**
  Add preprocessor macro:
  ```c
  /* DOSBox-X Kiosk Edition */
  #ifndef C_KIOSK
  #define C_KIOSK 1
  #endif
  ```
- **Autotools (`configure.ac`):**
  Add configure option:
  ```m4
  AC_ARG_ENABLE(kiosk,
    AS_HELP_STRING([--enable-kiosk], [Enable DOSBox-X Kiosk Edition appliance features]),
    [enable_kiosk=$enableval], [enable_kiosk=no])
  if test "x$enable_kiosk" = "xyes"; then
    AC_DEFINE(C_KIOSK, 1, [Define to 1 to enable DOSBox-X Kiosk Edition])
  fi
  AM_CONDITIONAL(C_KIOSK, test "x$enable_kiosk" = "xyes")
  ```

### Step 0.2: Vendor SQLite 3 Amalgamation
- Place `sqlite3.h` and `sqlite3.c` into `src/libs/sqlite3/`.
- Ensure standard compile flags for embedded performance and safety:
  - `-DSQLITE_ENABLE_JSON1=1`
  - `-DSQLITE_THREADSAFE=1`
  - `-DSQLITE_DEFAULT_FOREIGN_KEYS=1`
  - `-DSQLITE_ENABLE_RTREE=1`
  - `-DSQLITE_OMIT_LOAD_EXTENSION=1`
  - `-DSQLITE_ENABLE_FTS5=1`
- Create `src/libs/sqlite3/Makefile.am`.

### Step 0.3: Vendor Dear ImGui
- Place Dear ImGui core source and header files into `src/libs/imgui/`.
- Place SDL2 and OpenGL3 backends into `src/libs/imgui/backends/`.
- Customize `src/libs/imgui/imconfig.h` for DOSBox-X:
  - Enable 32-bit indices if needed (`#define ImDrawIdx unsigned int`).
  - Configure assertions to hook into DOSBox-X `LOG_MSG` / debug output.
- Create `src/libs/imgui/Makefile.am`.

### Step 0.4: Vendor Single-Header Utilities
- Add `stb_image_write.h` to `src/libs/stb/stb_image_write.h` for generating cached PNG icons from Windows `.GRP` and `.LNK` binaries.

### Step 0.5: Create Core Kiosk Skeleton (`src/kiosk/`)
- **`src/kiosk/kiosk_types.h`:**
  - Define `enum class KioskState` (`DISABLED`, `STATE_KIOSK_BOOT`, `STATE_COUNTDOWN_IDLE`, `STATE_SHELL_INTERACTIVE`, `STATE_SYNTHESIZE_LAUNCH`, `STATE_EMULATION_RUNNING`, `STATE_RUNTIME_OVERLAY`).
  - Define `struct LaunchContext`.
- **`src/kiosk/kiosk_main.h` & `src/kiosk/kiosk_main.cpp`:**
  - `KioskManager` singleton class:
    - `bool Initialize(const std::string& db_path, const std::string& base_conf)`
    - `void RunMainLoop()`
    - State getters/setters.
- **`src/kiosk/kiosk_db.h` & `src/kiosk/kiosk_db.cpp`:**
  - `KioskDatabase` singleton class:
    - `bool Open(const std::string& db_path)`
    - `std::string GetSQLiteVersion()`: calls `sqlite3_libversion()` to verify linkage.
- **`src/kiosk/ui/kiosk_ui.h` & `src/kiosk/ui/kiosk_ui.cpp`:**
  - `KioskUI` singleton class:
    - `bool Initialize(SDL_Window* window, void* gl_context)`
    - `void Shutdown()`
    - `void RenderFrame()` (displays initial Hello Kiosk test window).

### Step 0.6: Hook CLI & Init in `src/dosbox.cpp`
- In `src/dosbox.cpp`:
  - Include `"kiosk/kiosk_main.h"`.
  - In `main()` or `DOSBOX_Init()`:
    ```cpp
    #if C_KIOSK
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--kiosk-mode") == 0) {
            kiosk_mode = true;
        }
    }
    if (kiosk_mode) {
        LOG_MSG("KIOSK: DOSBox-X Kiosk Edition initializing...");
        if (KioskManager::GetInstance().Initialize("", "")) {
            LOG_MSG("KIOSK: Subsystems initialized successfully (SQLite %s, ImGui %s).",
                    KioskDatabase::GetInstance().GetSQLiteVersion().c_str(),
                    IMGUI_VERSION);
        }
    }
    #endif
    ```

### Step 0.7: Visual Studio Project Integration
- Update `vs/dosbox-x.vcxproj` and `vs/dosbox-x.vcxproj.filters`:
  - Add include directories:
    - `$(SolutionDir)..\src\libs\sqlite3`
    - `$(SolutionDir)..\src\libs\imgui`
    - `$(SolutionDir)..\src\libs\imgui\backends`
    - `$(SolutionDir)..\src\kiosk`
  - Add compile items:
    - `..\src\libs\sqlite3\sqlite3.c`
    - `..\src\libs\imgui\imgui.cpp`
    - `..\src\libs\imgui\imgui_draw.cpp`
    - `..\src\libs\imgui\imgui_tables.cpp`
    - `..\src\libs\imgui\imgui_widgets.cpp`
    - `..\src\libs\imgui\backends\imgui_impl_sdl2.cpp`
    - `..\src\libs\imgui\backends\imgui_impl_opengl3.cpp`
    - `..\src\kiosk\kiosk_main.cpp`
    - `..\src\kiosk\kiosk_db.cpp`
    - `..\src\kiosk\ui\kiosk_ui.cpp`

---

## 4. Verification Plan

### Automated / Command-Line Build Verification
1. **Compilation Check via MSBuild:**
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
     vs\dosbox-x.sln /p:Configuration="Debug SDL2" /p:Platform="x64" /m
   ```
2. **Regression Check (Non-Kiosk execution):**
   Run without `--kiosk-mode`:
   ```powershell
   .\vs\bin\x64\Debug_SDL2\dosbox-x.exe -version
   ```
   *Expected result:* Standard DOSBox-X runs with no change in behavior.
3. **Kiosk Flag Verification:**
   Run with `--kiosk-mode`:
   ```powershell
   .\vs\bin\x64\Debug_SDL2\dosbox-x.exe --kiosk-mode -exit
   ```
   *Expected result:* Terminal/log displays Kiosk initialization banner confirming SQLite3 and ImGui version detection.
