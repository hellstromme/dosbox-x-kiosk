# Technical Review: DOSBox-X Kiosk Edition Plans & Implementation

**Date:** November 1, 2026  
**Scope:**
1. **Part 1:** Review of [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md) and [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md)
2. **Part 2:** Review of Completed Phase 1 & Phase 2 Plans ([`Phase2_Lifecycle_State_Machine_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase2_Lifecycle_State_Machine_Plan.md), Retrospectives) and Actual Implementation (`src/kiosk/`, `src/gui/sdlmain.cpp`, `src/ints/bios.cpp`, `tests/kiosk_tests.cpp`)
3. **Part 3:** Review of [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md)

---

## Part 1: Review of Design Document & High-Level Implementation Plan

The core architectural vision in [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md) and [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md)—embedding the pre-boot shell, hardware archetype synthesizer, runtime media caddy overlay, and guest shutdown traps directly inside the DOSBox-X process—is well-reasoned and avoids the context-switching and raw-disk-boot limitations of external frontends.

However, several technical flaws and architectural inconsistencies exist in the baseline design and high-level roadmap:

### 1.1 `[HIGH]` Primary IDE Slave (`-ide 1s` / `D:`) Dispatch Drive Shifts CD-ROM Drive Letters
- **Location:** [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L805-L842), [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L180-L182)
- **Finding:** Both documents specify mounting the ephemeral 1.44MB FAT12 dispatch disk (`kiosk_dispatch.img`) as a hard disk on Primary IDE Slave (`-ide 1s`) during Carousel launches (Mode A), while omitting it during Machine-Only boots (Mode B).
- **Impact:** Because MS-DOS and Windows 9x assign drive letters to all primary partitions on fixed IDE disks before assigning letters to optical drives (`MSCDEX` / `CDFS.VXD`), mounting a dispatch HDD on `-ide 1s` claims `D:` and shifts the CD-ROM drive on Secondary IDE Master (`-ide 2m`) from `D:` to `E:`. Any game installed during a Machine-Only boot (when the CD-ROM was `D:`) that records `D:\` in `.CFG`/`.INI` files or the Windows Registry will fail with "Insert CD-ROM" errors when launched from the Carousel.
- **Note:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L20-L35) already identified and corrected this by moving the dispatch volume to **Floppy Drive `B:` (`IMGMOUNT 1`)**. The High-Level Implementation Plan should be updated to match.

### 1.2 `[HIGH]` Off-by-One Relative Jump & Array Bounds Mismatch in `KIOSKEXT.COM`
- **Location:** [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L947-L1011), [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L68)
- **Finding:**
  1. In the `kioskext.asm` listing and byte arrays (`KIOSKEXT_COM_BYTES` / `KIOSKEXT_COM_ALIGNED`), `jc bochs_fallback` at offset `0x010B` is encoded as `72 14` (`+20` bytes) and `mov si, shutdown_str` as `BE 28 01`. However, the APM 1.1 negotiation (`10` bytes) + APM power-off (`11` bytes) sequence between `0x010D` and `bochs_fallback` is **21 bytes (`0x15`)**, not 20 (`0x14`). On an 8088 XT where `INT 15h AX=5301h` sets Carry, `72 14` jumps into the second byte (`0x15`) of `CD 15` at `0x0121` instead of `mov dx, 0x8900` at `0x0122`, executing corrupted instructions.
  2. `KIOSKEXT_COM_ALIGNED[48]` is declared with size `48` but initialized with `56` elements (which fails C++ compilation) and omits the safety `ret` (`0xC3`).
- **Note:** This is fixed in [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L38-L41) (`72 15`, `BE 31 01`, `57` bytes).

### 1.3 `[HIGH]` Multi-Session Emulation Re-Entry vs. DOSBox-X Subsystem Lifecycle (Resolved: Hybrid Option 3 Adopted)
- **Location:** [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L1006-L1034), [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L152-L169), [`Phase2_Lifecycle_State_Machine_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase2_Lifecycle_State_Machine_Plan.md#L19-L80)
- **Finding:** Originally, the design assumed `KioskManager` could repeatedly call `control->ParseConfigFiles()` and `DOSBOX_RunMachine()` in a loop inside `main()` (`goto fresh_boot;`) to switch between radically different hardware archetypes (e.g., running a Pentium MMX + Voodoo + 64MB RAM Win98 session, returning to the Carousel, and launching an 8088 XT + CGA + 1MB RAM session).
- **Impact:** In DOSBox-X, over 45 hardware and OS subsystems (`Init_RAM`, `VGA_Init`, `CPU_Init`, `SBLASTER_Init`, `GUS_Init`, `VOODOO_Init`, `IDE_Init`, `FDC_Primary_Init`, `DOS_Init`, `AUTOEXEC_Init`, `VM_PowerOn`) are initialized *once* in [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L9855-L10170) before the `fresh_boot:` label. Simply catching an exception from `DOSBOX_RunMachine()` and jumping to `fresh_boot:` does **not** re-initialize hardware subsystems or re-execute `[autoexec]` once `dos_kernel_disabled` becomes `true` during `BOOT -l c`.
- **Resolution (Option 3 — Hybrid Architecture Adopted in Docs):**
  1. **Pre-Boot Shell Before Hardware Init (Zero-Flicker In-Process Launch):** Run `KioskManager::RunMainLoop()` right after `GUI_StartUp()` creates the SDL2/OpenGL window (**before** line 9855 hardware initialization). When the user or watchdog selects a target in `STATE_SYNTHESIZE_LAUNCH`, write `kiosk_session.conf` and `kiosk_dispatch.img`, call `control->ParseConfigFile("kiosk_session.conf")`, and let `main()` run its normal one-time hardware initialization (`Init_RAM`, `VOODOO_Init`, `DOS_Init`, `AUTOEXEC_Init`, `VM_PowerOn`, `DOSBOX_RunMachine()`) in-process with zero display flicker.
  2. **Clean Shutdown & Fast Re-Exec on Guest Exit:** When `KioskGuestShutdownException` (or shell exit) is caught, flush virtual drives (`DOS_ShutdownDrives()`, `FreeBIOSDiskList()`), delete `kiosk_dispatch.img`, tear down SDL cleanly, and re-exec the binary with `--kiosk-mode --kiosk-skip-countdown` so the user lands directly in `STATE_SHELL_INTERACTIVE` (`Carousel`) with a 100% fresh hardware address space.

### 1.4 `[MEDIUM]` Buildroot Target Host vs. `systemd` / `udisks2` / `polkit` / `pulseaudio` Assumptions (Resolved: Dual-Target Desktop Big-Picture + Lightweight Buildroot Adopted)
- **Location:** [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L45-L60), [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L1740-L1830), [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L280-L305)
- **Finding:**
  1. Originally, Phase 8 of the design and high-level plan assumed `systemd` (`sd_notify`, `sd-bus`), `udisks2`, and JavaScript `polkit` rules, which add ~60–80MB of rootfs/RAM bloat on a Buildroot appliance for old laptops and do not apply to Windows/Desktop "Big Picture" mode.
  2. In `ARCH_386DX` ([`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L597)), `fluid.driver = pulseaudio` was specified in the design doc (though already omitted in [`src/kiosk/machine/archetypes.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/machine/archetypes.cpp#L56-L61)).
  3. [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L19) referred to `ENABLE_KIOSK` and `src/dosbox.cpp` instead of `C_KIOSK` and [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp).
- **Resolution (Updated in Docs):**
  1. **Dual-Target Host Architecture:** Updated [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md) and [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md) to explicitly support both **Desktop "Big Picture" Mode** (Windows 10/11 & Desktop Linux) and **Lightweight Buildroot Appliance Mode** for old laptops (BusyBox `init` `/etc/inittab` + SDL2 `KMSDRM` + direct `ALSA` with optional `#ifdef HAVE_LIBSYSTEMD`).
  2. **Cross-Platform `KioskUsbBridge`:** Replaced the heavy `udisks2` + `polkit` + `sd-bus` dependency with a lightweight `eudev`/`mdev` mount rule + `inotify` watcher on Linux (`/run/media/kiosk` & `/run/media/$USER`) and Win32 `GetLogicalDrives()` (`DRIVE_REMOVABLE` / `DRIVE_CDROM`) on Windows.
  3. **Audio & Guard Alignment:** Removed `fluid.driver = pulseaudio` from `ARCH_386DX` in the design doc and aligned the high-level plan on `C_KIOSK` and `src/gui/sdlmain.cpp`.

---

## Part 2: Review of Completed Phases 1 & 2 (Plans & Actual Code)

Phases 1 and 2 established clean C++14 data structures, thread-safe SQLite CRUD operations with `:memory:` fallback, the 5 hardware archetypes, two-phase ImGui initialization, and the BIOS `INT 15h` / Port `0x8900` exception traps. All 19 unit tests in [`tests/kiosk_tests.cpp`](file:///C:/code/dosbox-x-kiosk/tests/kiosk_tests.cpp) pass in isolation.

However, inspecting the **actual runtime integration** in [`src/kiosk/kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp) and [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp) reveals several critical issues introduced during Phase 2 implementation that prevent end-to-end Kiosk operation outside of unit tests:

> [!CAUTION]
> While the unit tests in [`tests/kiosk_tests.cpp`](file:///C:/code/dosbox-x-kiosk/tests/kiosk_tests.cpp) test `KioskManager` state transitions by calling methods directly (`UpdateWatchdog`, `SynthesizeAndPrepareLaunch`, `EnterRuntimeOverlay`), the actual SDL main loop in [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp) does not pump the pre-boot state machine, never calls `UpdateWatchdog()`, never loads the synthesized configuration into DOSBox-X, and exits the process upon catching `KioskGuestShutdownException`.

### 2.1 `[CRITICAL]` `KioskManager::RunMainLoop()` Does Not Block in Pre-Boot States and Immediately Boots the Guest
- **Location:** [`src/kiosk/kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L91-L97), [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10382-L10388)
- **Finding:** In [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10382-L10387):
  ```cpp
  #if defined(C_KIOSK)
  if (KioskManager::GetInstance().IsKioskModeActive()) {
      KioskManager::GetInstance().RunMainLoop();
  }
  #endif
  DOSBOX_RunMachine();
  ```
  And in [`KioskManager::RunMainLoop()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L91-L97):
  ```cpp
  void KioskManager::RunMainLoop() {
      LOG_MSG("KIOSK: Entering Kiosk Main Loop (State: %d)", static_cast<int>(m_state.load()));
      if (m_state.load() == KioskState::STATE_SYNTHESIZE_LAUNCH) {
          std::string conf;
          SynthesizeAndPrepareLaunch(conf);
      }
  }
  ```
- **Issues Introduced:**
  1. **No Pre-Boot Event/Render Loop:** When `boot_timeout_sec > 0` (the default `30` seconds), `Initialize()` sets `m_state = STATE_COUNTDOWN_IDLE`. `RunMainLoop()` checks `if (m_state == STATE_SYNTHESIZE_LAUNCH)` (which is `false`), returns immediately, and execution falls straight into `DOSBOX_RunMachine()`! As a result, DOSBox-X boots the integrated DOS shell immediately in the background while `m_state` is still `STATE_COUNTDOWN_IDLE`.
  2. **`UpdateWatchdog()` Is Dead Code at Runtime:** `KioskManager::UpdateWatchdog(float delta_sec)` ([`src/kiosk/kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L131-L153)) is never called anywhere in `src/gui/sdlmain.cpp` or `src/kiosk/`. The 30-second countdown timer never ticks down at runtime.
  3. **Synthesized Configuration Is Never Applied to DOSBox-X:** Even when `SynthesizeAndPrepareLaunch(conf)` is called, the synthesized INI + `[autoexec]` string in `conf` is discarded into a local variable (and saved only to `m_last_synthesized_conf`). It is never written to a session `.conf` file or parsed into `control` (`control->ParseConfigFile(...)`), and when a user clicks a launch button in [`KioskUI::RenderInteractiveShellPlaceholder()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.cpp#L166-L199), `m_state` transitions to `STATE_SYNTHESIZE_LAUNCH`, which nothing ever processes because `RunMainLoop()` has already returned!
- **Recommended Remediation (Planned for Phase 3):**
  1. **Force OpenGL Output When Kiosk Mode Is Active:** In [`GUI_StartUp()`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L4110), if `KioskManager::GetInstance().IsKioskModeActive()` and `output` is `"default"` or `"surface"`, force `output = "openglnb"` (`SetVal("sdl", "output", "openglnb")`) so `sdl_opengl.context` and `KioskUI::InitializeBackend()` are always initialized during `GUI_StartUp()`.
  2. **Move `RunMainLoop()` Before Line 9855 Hardware Init:** Relocate `KioskManager::GetInstance().RunMainLoop()` from `fresh_boot:` (line 10321) to right after `GUI_StartUp()` (around line 9852 of [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp), prior to `MAPPER_StartUp()` and `Init_RAM()`).
  3. **Implement 60 Hz Pre-Boot Frame Pump (with `-exit` Headless Bypass):** Inside `RunMainLoop()`, loop until `m_state == STATE_EMULATION_RUNNING` or `IsExitRequested()`, calling `GFX_Events()`, `UpdateWatchdog(delta_sec)`, `glClear()`, `SDL_GL_SwapBuffers()`, and `SDL_Delay(16)`. If `control->opt_exit` is set or `!KioskUI::GetInstance().IsBackendInitialized()`, immediately call `TriggerDefaultBootAction()` so automated CLI smoke tests (`test-kiosk.ps1`) do not block waiting on GUI input.
  4. **Write & Parse `kiosk_session.conf`:** Have `SynthesizeAndPrepareLaunch()` write `kiosk_session.conf` to disk (when launching a configured machine/item) and call `control->ParseConfigFile(GetSessionConfPath().c_str())` immediately after `RunMainLoop()` returns, before DOSBox-X initializes hardware subsystems.

### 2.2 `[CRITICAL]` Catching `KioskGuestShutdownException` Falls Through to `SDL_Quit()` Instead of Returning to the Kiosk Shell
- **Location:** [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10388-L10400), [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10760-L10885)
- **Finding:** Step 2.4 of [`Phase2_Lifecycle_State_Machine_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase2_Lifecycle_State_Machine_Plan.md#L252-L256) specified that catching `KioskGuestShutdownException` would clean up guest drives (`DOS_ShutdownDrives()`, `FreeBIOSDiskList()`, `Kiosk_ResetPort8900State()`), transition to `STATE_SHELL_INTERACTIVE`, and pump the pre-boot shell until the next launch (`goto fresh_boot;`) or appliance exit.
  However, in the actual implementation ([`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10394-L10399)):
  ```cpp
  } catch (const KioskGuestShutdownException& e) {
      LOG_MSG("KIOSK: Caught guest shutdown exception (%s)", KioskShutdownReasonToString(e.reason));
      KioskManager::GetInstance().OnGuestShutdown(e.reason);
      run_machine = false;
      dos_kernel_shutdown = !dos_kernel_disabled;
  }
  ```
- **Issues Introduced:**
  1. Because `run_machine`, `run_bios`, `reboot_machine`, and `reboot_dos` are all `false` after the `catch` block, execution runs `if (dos_kernel_shutdown) { ... }` and then **falls straight through to line 10761 (`GFX_ShutDown()`, `SDL_Quit()`, process termination)**! The user is never returned to `STATE_SHELL_INTERACTIVE`.
  2. Additionally, during a `REAL_HDD_BOOT` (`BOOT -l c`), the `BOOT` command throws `int(2)` (caught at line 10401), which sets `run_machine = true; dos_kernel_shutdown = true;`, shuts down the integrated DOSBox-X kernel (`dos_kernel_disabled = true`), and jumps to `fresh_boot:` (line 10630) to run the guest OS. At `fresh_boot:` (line 10384), `KioskManager::GetInstance().RunMainLoop()` is invoked a **second time** right before the guest MBR boots!
- **Recommended Remediation (Planned for Phase 3):**
  1. **Add `--kiosk-skip-countdown` CLI Flag:** Parse `-kiosk-skip-countdown` in [`DOSBOX_parse_argv()`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L7583-L7593) (`control->opt_kiosk_skip_countdown`) and transition `KioskManager` directly from `STATE_KIOSK_BOOT` to `STATE_SHELL_INTERACTIVE` when returning from a completed session.
  2. **Remove `RunMainLoop()` from `fresh_boot:`:** Relocating `RunMainLoop()` above line 9855 (per Finding 2.1) ensures `goto fresh_boot;` during `BOOT -l c` never re-invokes `RunMainLoop()` mid-boot.
  3. **Set `kiosk_reexec_to_shell` on Guest Shutdown:** In [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10388-L10399), when `KioskGuestShutdownException` is caught or `DOSBOX_RunMachine()` returns (`SHELL_EXIT`), set `kiosk_reexec_to_shell = (!control->opt_exit && !control->opt_test && !KioskManager::GetInstance().IsExitRequested())` and let execution proceed through lines 10761–10884 (`DOS_ShutdownDrives()`, `FreeBIOSDiskList()`, `KioskManager::Shutdown()`, `SDL_Quit()`, and `myconf` destructor) so all disk images, SQLite WAL files, and `kiosk_dispatch.img` are flushed and closed cleanly.
  4. **Re-Exec at End of `main()` (`KioskManager::RelaunchIntoInteractiveShell`):** At line 10885 of `src/gui/sdlmain.cpp`, if `kiosk_reexec_to_shell` is true, invoke `KioskManager::RelaunchIntoInteractiveShell(argv[0], saved_kiosk_db, saved_kiosk_conf)` using POSIX `execv()` on Linux (replacing the process image under the same PID) or `CreateProcessW()` on Windows to launch `--kiosk-mode --kiosk-skip-countdown`.

### 2.3 `[HIGH]` Runtime Overlay (`F12+D`) Does Not Pause CPU Emulation, Mute Mixer, or Unlock Mouse, and "Quit to Kiosk Menu" Leaves Guest Running
- **Location:** [`src/kiosk/kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L416-L439), [`src/kiosk/ui/kiosk_ui.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.cpp#L228-L235), [`src/gui/sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L5988-L6009)
- **Finding:**
  1. [`KioskManager::EnterRuntimeOverlay()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L416-L422) and [`ResumeEmulation()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L424-L430) only change `m_state` and log a message. They do **not** pause guest CPU execution, mute audio DMA, or release SDL relative mouse capture (`sdl.mouse.locked`).
  2. When the user clicks `"Quit to Kiosk Menu"` in [`KioskUI::RenderRuntimeOverlayPlaceholder()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.cpp#L232-L234), it calls [`KioskManager::AbortSessionToShell()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L432-L439), which sets `m_state = STATE_SHELL_INTERACTIVE` **without throwing `KioskGuestShutdownException` or stopping `DOSBOX_RunMachine()`**. As a result, the guest game continues running underneath the `STATE_SHELL_INTERACTIVE` window!
- **Recommended Remediation (Planned for Phase 3 / Phase 4):**
  1. **Run a 60 Hz Modal Overlay Loop in `src/gui/sdlmain.cpp` (`Kiosk_RunRuntimeOverlayLoop`):** Following the upstream [`PauseDOSBoxLoop()`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L1632-L1661) and SDL menu modal loop ([lines 5377–5465](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L5377-L5465)) pattern, when `F12+D` or `SDL_CONTROLLER_BUTTON_GUIDE` transitions into `STATE_RUNTIME_OVERLAY` inside `GFX_Events()`, enter `Kiosk_RunRuntimeOverlayLoop()` on the main thread (automatically halting `DOSBOX_RunMachine()` and `PIC_RunQueue()`).
  2. **Mute Audio, Release Keys & Unlock Mouse on Entry:** Call `MAPPER_ReleaseAllKeys()`, `SDL_PauseAudioDevice(SDL2_AudioDevice, 1)`, save `was_mouse_locked = sdl.mouse.locked`, and call `SDL_SetRelativeMouseMode(SDL_FALSE)` + `SDL_ShowCursor(SDL_ENABLE)`.
  3. **60 Hz Frozen-Frame Redraw + ImGui Composite:** Inside the modal loop, poll `SDL_PollEvent(&event)` (routing `F12+D` / `Guide` / `Escape` to `ResumeEmulation()` and passing events to `KioskUI::ProcessEvent(&event)`), then call `glClear()`, `GFX_OpenGLRedrawScreen()` (redrawing the frozen guest texture `sdl_opengl.texture`), `SDL_GL_SwapBuffers()`, and `SDL_Delay(16)`.
  4. **Restore on Resume or Throw `KioskGuestShutdownException` on Abort:** Upon exiting the loop, restore `SDL_PauseAudioDevice(SDL2_AudioDevice, 0)`, `CPU_Reset_AutoAdjust()`, and mouse lock (if resuming). If the user clicked `"Quit to Kiosk Menu"` (`AbortSessionToShell()`, setting `m_last_shutdown_reason = USER_OVERLAY_ABORT`), throw `KioskGuestShutdownException(KioskShutdownReason::USER_OVERLAY_ABORT)` so `DOSBOX_RunMachine()` unwinds into the clean shutdown + re-exec handler from Finding 2.2.

### 2.4 `[MEDIUM]` Missing SQL `CHECK` Constraints & `KioskUI` Cache Invalidation
- **Location:** [`src/kiosk/kiosk_db.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_db.cpp#L137-L206), [`src/kiosk/ui/kiosk_ui.h`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.h#L32)
- **Finding:**
  1. In [`KioskDatabase::InitializeSchema()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_db.cpp#L137-L206), several `CHECK` constraints specified in the Design Doc DDL were omitted (`media_pool.media_type`, `media_pool.physical_style`, `media_pool.label_font_style`, `os_distributions.install_strategy`, `machines.archetype_id`, and `catalog_items.launch_mechanism`).
  2. [`KioskUI::InvalidateCatalogCache()`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.h#L32) was added during Phase 2 review, but is never called when `KioskDatabase` mutates machines/items or when `KioskManager::OnGuestShutdown()` returns to the shell.
- **Recommended Remediation (Planned for Phase 3):**
  1. **Restore 6 SQL `CHECK` Constraints in `KioskDatabase::InitializeSchema()` ([`src/kiosk/kiosk_db.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_db.cpp#L137-L206)):** Add the `CHECK` constraints on `media_pool.media_type`, `media_pool.physical_style`, `media_pool.label_font_style`, `os_distributions.install_strategy`, `machines.archetype_id`, and `catalog_items.launch_mechanism` to match [`DOSBox-X Kiosk Launcher Design.md:320-380`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L320-L380).
  2. **Wire `KioskUI::GetInstance().InvalidateCatalogCache()` to Lifecycle & DB Mutations:** Call `InvalidateCatalogCache()` inside `KioskManager::Initialize()`, `KioskManager::OnGuestShutdown()`, and at the end of successful machine/catalog mutation methods in `KioskDatabase` (`InsertMachine`, `UpdateMachine`, `DeleteMachine`, `InsertCatalogItem`, `UpdateCatalogItem`, `DeleteCatalogItem`, `IncrementPlayCount`).

---

## Part 3: Technical Review of Phase 3 Plan (`Phase3_Ephemeral_Boot_Injector_Plan.md`)

[`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md) makes two major engineering improvements over the initial design doc:
- **Moving the dispatch volume to Floppy Drive `B:` (`IMGMOUNT 1`)**, which completely eliminates the `D:` CD-ROM drive-letter shift bug.
- **Fixing the 57-byte `KIOSKEXT.COM` machine code offsets** (`72 15` relative jump to `0x0122` and `BE 31 01` pointer to `"Shutdown"` at `0x0131`), which we verified byte-for-byte.

However, the Phase 3 plan has **6 technical issues and architectural gaps** that should be resolved before implementation:

### 3.1 `[CRITICAL]` `KIOSKWIN.EXE` Cannot Be a "128-Byte Stub" (`KIOSKWIN_EXE_BYTES[128]`)
- **Location:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L167-L170), [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L68-L70)
- **Finding:** Component 1 defines:
  ```cpp
  // Minimal MZ/Win32-compatible wrapper stub for B:\KIOSKWIN.EXE
  static constexpr size_t KIOSKWIN_EXE_SIZE = 128;
  static constexpr uint8_t KIOSKWIN_EXE_BYTES[KIOSKWIN_EXE_SIZE] = { ... };
  ```
- **Why This Fails:**
  1. A 128-byte binary is barely larger than a 64-byte DOS `IMAGE_DOS_HEADER`. It cannot contain a valid Windows 16-bit NE or 32-bit PE header + import table + code section capable of restoring `C:\WINDOWS\SYSTEM.INI`, parsing `B:\KIOSKWIN.INI`, calling `CreateProcessA` / `WinExec`, waiting for process termination, and invoking `ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCE, 0)`.
  2. **Win 3.1x vs. Win 95/98 Architecture Mismatch:** Windows 3.1x (`WIN3X_PROGMAN_HOOK`) cannot execute a 32-bit Win32 PE binary as its `SYSTEM.INI` `[boot] shell=`. Conversely, in Windows 3.1x, `WIN.COM` natively accepts a target executable on the command line (`C:\WINDOWS\WIN.COM C:\APP\APP.EXE`) or via a 16-bit NE helper, and returns to DOS when the shell exits (where `B:\KIOSKEXT.COM` can power off the VM).
  3. If `SYSTEM.INI` is patched with `shell=B:\KIOSKWIN.EXE` pointing to a 128-byte dummy MZ stub, Windows 3.1 and Windows 95/98 will fail to load the shell and hang or exit immediately without launching the target app.
- **Recommendation (Resolved — `WIN.COM <target_exe>` Direct Invocation with Zero `SYSTEM.INI` Mutation):**
  - Eliminate `KIOSKWIN.EXE`, `KIOSKWIN.INI`, `SYSTEM.KIO`, and `PatchSystemIniShell()` completely.
  - For both Windows 3.1x (`WIN3X_PROGMAN_HOOK`) and Windows 95/98 (`WIN9X_RUNONCE_HOOK`) targets, `SynthesizeKioskGoBat()` invokes `C:\WINDOWS\WIN.COM <guest_executable> [guest_arguments]` directly from `B:\KIOSKGO.BAT` (followed by `B:\KIOSKEXT.COM` when `return_on_exit == true`), leaving `C:\WINDOWS\SYSTEM.INI` untouched.

### 3.2 `[HIGH]` `SYSTEM.INI` Overwrite Hazard (`original_system_ini` Defaulting to `""`) & Crash Self-Healing
- **Location:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L223-L250), [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L300-L305), [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L348-L351)
- **Finding:**
  1. In `KioskManager::SynthesizeAndPrepareLaunch()` (line 350), `BuildDispatchVolume(dispatch_path, machine, *item_ptr)` is called with `original_system_ini` defaulting to `""`.
  2. In `PatchSystemIniShell()` (line 305), if `[boot]` is absent (because `original_system_ini` is `""`), it creates a 2-line string `[boot]\r\nshell=B:\KIOSKWIN.EXE\r\n` and writes it to `B:\SYSTEM.KIO`.
  3. Then `B:\KIOSKGO.BAT` (line 303) runs `COPY /Y B:\SYSTEM.KIO C:\WINDOWS\SYSTEM.INI`, **replacing the guest's entire `C:\WINDOWS\SYSTEM.INI` with a 2-line file**! Without `[386Enh]`, `[drivers]`, keyboard, mouse, and display driver sections in `SYSTEM.INI`, Windows 3.1 / 95 / 98 cannot boot!
  4. Furthermore, if the guest crashes before `SYSTEM.INI` is restored, a subsequent Mode B ("Machine-Only") boot does not run `B:\KIOSKGO.BAT`, leaving `C:\WINDOWS\SYSTEM.INI` permanently pointing to `B:\KIOSKWIN.EXE`.
- **Recommendation (Resolved — Eliminated via Zero-Mutation `WIN.COM` Launch):**
  - By launching Windows 3.1x and Windows 95/98 applications via `C:\WINDOWS\WIN.COM <guest_executable> [guest_arguments]` inside `B:\KIOSKGO.BAT` (see Finding 3.1), `C:\WINDOWS\SYSTEM.INI` is never modified or overwritten on `hdd_c.img`, completely eliminating any risk of `SYSTEM.INI` corruption on power loss or crash.

### 3.3 `[HIGH]` Underspecified `EnsureOneTimeAutoexecHook` for Partitioned `hdd_c.img` Files
- **Location:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L209-L210), [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L271-L306)
- **Finding:** `BootInjector::EnsureOneTimeAutoexecHook(const std::string& hdd_or_autoexec_path)` claims to hook `C:\AUTOEXEC.BAT` inside a raw `FAT12/FAT16/FAT32` hard disk image or host file, yet:
  1. Section 4 only details `InjectAutoexecHookToString()` and `BuildFat12ImageInMemory()` (for unpartitioned 1.44MB FAT12 floppies). It does not describe how `EnsureOneTimeAutoexecHook` parses an MBR partition table (offset `0x1BE`), locates the active FAT12/FAT16/FAT32 partition LBA, updates `AUTOEXEC.BAT`'s 32-byte root directory entry (`DIR_FileSize`), or allocates a new cluster in FAT16/FAT32 if appending the 90-byte hook crosses a cluster boundary.
  2. `KioskManager::SynthesizeAndPrepareLaunch()` (lines 347–355) never calls `EnsureOneTimeAutoexecHook(machine.primary_hdd_path)`.
- **Recommendation (Resolved — Dual-Mode `EnsureOneTimeAutoexecHook` + Pre-Boot Invocation):**
  - Specified dual-mode `EnsureOneTimeAutoexecHook(hdd_or_autoexec_path)` in [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L256-L266): plain text mode (`< 64 KB` or no `0x55 0xAA` signature) and raw disk image mode (superfloppy LBA `0` or MBR `0x1BE..0x1FD` partition LBA resolution, FAT12/16/32 root directory `"AUTOEXECBAT"` lookup/creation, cluster chain walking/allocation across all FAT copies, and `DIR_FileSize` update).
  - Updated `KioskManager::SynthesizeAndPrepareLaunch()` to invoke `EnsureOneTimeAutoexecHook(machine.primary_hdd_path)` on `REAL_HDD_BOOT` machines, and expanded `KioskBootInjectorTest.OneTimeAutoexecHookIdempotency` to test raw disk image hooking.

### 3.4 `[MEDIUM]` Internal Contradiction in Mode B ("Machine-Only Boot") Idle Floppy Mounting
- **Location:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L26-L35) vs. [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L352-L354) & [`L382-L384`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L382-L384)
- **Finding:**
  - Sections 1, 2, and 3.1 correctly explain that during a Mode B ("Machine-Only") boot on a `REAL_HDD_BOOT` machine, `BootInjector::BuildIdleDispatchVolume()` (containing `KIOSKEXT.COM` and **no** `KIOSKGO.BAT`) **must** be mounted on `IMGMOUNT 1` (if the user hasn't bound a `B:` floppy). Otherwise, BIOS `INT 11h` reports only 1 floppy drive, and MS-DOS maps `B:` as a phantom alias for `A:`, halting boot at `IF EXIST B:\KIOSKGO.BAT` with:
    `Insert diskette for drive B: and press any key when ready`
  - However, Component 3 (lines 352–354) and Test 6 (lines 382–384) contradict this by stating `SynthesizeAndPrepareLaunch()` passes `""` as `dispatch_image_path` in Mode B!
- **Recommendation (Resolved — Mode B Always Mounts Idle Dispatch Floppy When `B:` Is Unbound):**
  - Updated Component 3 (`KioskManager::SynthesizeAndPrepareLaunch()`) and Test 6 (`RealHddMachineOnlyBootMountsIdleDispatchVolumeWithoutKioskGoBat`) in [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L360-L415) so Mode B always calls `BuildIdleDispatchVolume(dispatch_path)` and mounts it on `IMGMOUNT 1` when no user `B:` caddy binding exists.

### 3.5 `[MEDIUM]` `B:\KIOSKGO.BAT` Drive Selection, Path Quoting, and Disk-Swap Edge Case
- **Location:** [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L290-L299), [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L334-L340)
- **Finding:**
  1. `SynthesizeKioskGoBat()` hardcodes `C:` before `CD <guest_working_dir>`. If `item.guest_working_dir` already includes a drive prefix (e.g., `"C:\GAMES\DOOM2"` as shown in [`DOSBox-X Kiosk Launcher Design.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X%20Kiosk%20Launcher%20Design.md#L379) or `"D:\"` for a CD-ROM executable), `SynthesizeKioskGoBat()` should parse any leading `<Drive>:` from `guest_working_dir` and quote executables/paths containing spaces.
  2. If the user swaps Floppy `B:` during gameplay via `F12+D`, when the game exits, MS-DOS `COMMAND.COM` attempts to read the next line of `B:\KIOSKGO.BAT` (`B:\KIOSKEXT.COM`) from Drive `B:`. If `kiosk_dispatch.img` is no longer the active disk in `B:`, DOS will fail with `Insert disk with batch file`.
- **Recommendation (Resolved — Smart Drive Prefix Parsing & Dedicated `IMGMOUNT 1` Dispatch Slot):**
  - Updated [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L281-L337) so `SynthesizeKioskGoBat()` parses any leading `<Drive>:` prefix from `item.guest_working_dir` (defaulting to `C:`), and `ConfigSynthesizer::SynthesizeAutoexec()` dedicates `IMGMOUNT 1` (`B:`) to `kiosk_dispatch.img` while user multi-floppy sets remain on `IMGMOUNT 0` (`A:`).

---

## Summary Priority Matrix

| ID | Part | Severity | Component | Summary |
| :--- | :--- | :--- | :--- | :--- |
| **P2-1** | Part 2 | **CRITICAL** | [`kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L91-L97), [`sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10382-L10388) | `RunMainLoop()` does not block in pre-boot states, never calls `UpdateWatchdog()`, and never applies synthesized configs to `control`. |
| **P2-2** | Part 2 | **CRITICAL** | [`sdlmain.cpp`](file:///C:/code/dosbox-x-kiosk/src/gui/sdlmain.cpp#L10394-L10762) | Catching `KioskGuestShutdownException` falls through to `GFX_ShutDown()` / `SDL_Quit()` instead of returning to `STATE_SHELL_INTERACTIVE`. |
| **P3-1** | Part 3 | **CRITICAL** | [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L167-L170) | `KIOSKWIN.EXE` is specified as a 128-byte placeholder stub, which cannot function as a Win16 (NE) or Win32 (PE) shell wrapper. |
| **P2-3** | Part 2 | **HIGH** | [`kiosk_main.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/kiosk_main.cpp#L416-L439), [`kiosk_ui.cpp`](file:///C:/code/dosbox-x-kiosk/src/kiosk/ui/kiosk_ui.cpp#L232-L234) | `STATE_RUNTIME_OVERLAY` does not pause CPU/audio/mouse, and `AbortSessionToShell()` leaves guest emulation running underneath the shell UI. |
| **P3-2** | Part 3 | **HIGH** | [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L300-L351) | `PatchSystemIniShell("")` overwrites `C:\WINDOWS\SYSTEM.INI` with a 2-line file if `original_system_ini` is not extracted from `hdd_c.img`. |
| **P3-3** | Part 3 | **HIGH** | [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L209-L210) | `EnsureOneTimeAutoexecHook()` lacks specification for MBR + FAT12/16/32 directory/cluster mutation and is not called in `SynthesizeAndPrepareLaunch()`. |
| **P1-1** | Part 1 | **HIGH** | [`DOSBox-X_Kiosk_Implementation_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/DOSBox-X_Kiosk_Implementation_Plan.md#L180-L182) | High-level plan still references `-ide 1s` (`D:`) for dispatch drive instead of Floppy `B:` (`IMGMOUNT 1`). |
| **P3-4** | Part 3 | **MEDIUM** | [`Phase3_Ephemeral_Boot_Injector_Plan.md`](file:///C:/code/dosbox-x-kiosk/docs/Kiosk/Phase3_Ephemeral_Boot_Injector_Plan.md#L352-L354) | Mode B ("Machine-Only") section 4/test 6 passes `""` instead of mounting `BuildIdleDispatchVolume()` on `B:`, risking DOS phantom-drive prompts. |
