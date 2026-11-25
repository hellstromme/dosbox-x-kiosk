# DOSBox-X Kiosk Edition — User & Manual Testing Guide (Phases 1–4)

This guide explains how to launch, configure, and manually test all Kiosk Edition features implemented across **Phases 1 through 4**:

- **Phase 1:** Kiosk Core Subsystems, SQLite Catalog (`catalog.db`), Hardware Archetypes, and Dynamic `.conf` Synthesis
- **Phase 2:** Kiosk Lifecycle State Machine, Auto-Boot Watchdog Countdown, Pre-Boot Interactive Shell, and Guest Shutdown Return Traps
- **Phase 3:** Ephemeral Boot Injector for Real HDD Guest OSes (`MS-DOS`, `FreeDOS`, `Windows 3.x`, `Windows 95/98`)
- **Phase 4:** Procedural Skeuomorphic Media Caddy & Runtime Disk-Swap Overlay (`F12+D` / `F11+O` / Gamepad Guide)

> [!NOTE]
> The full Big-Picture 10-foot CRT/Amber Carousel UI and Media Pool Manager GUI are scheduled for **Phase 5**. Through Phase 4, the Pre-Boot Launcher uses a functional Dear ImGui placeholder window, while the **Runtime Disk-Swap Overlay** (`STATE_RUNTIME_OVERLAY`) renders the full procedural skeuomorphic media shelf and compact list view.

---

## 1. Launching DOSBox-X Kiosk Edition

### Compiled Binary Location (Windows x64)
After building the `Debug SDL2 | x64` (or `Release SDL2 | x64`) configuration:

```powershell
& ".\bin\x64\Debug SDL2\dosbox-x.exe" -kiosk-mode
```

### Kiosk Command-Line Flags

| Flag | Description |
|---|---|
| `-kiosk-mode` | Enables DOSBox-X Kiosk Edition (Dear ImGui overlay, SQLite catalog, watchdog, runtime media caddy, and guest shutdown traps). |
| `-kiosk-db <path>` | Path to a persistent SQLite catalog database (e.g. `C:\kiosk\catalog.db`). If omitted or unwritable, Kiosk mode gracefully falls back to an in-memory database (`:memory:`). |
| `-kiosk-conf <path>` | Optional base `dosbox-x.conf` file merged beneath synthesized machine profiles. |
| `-kiosk-skip-countdown` | Skips the 30-second auto-boot countdown banner and opens directly into the Pre-Boot Interactive Launcher (`STATE_SHELL_INTERACTIVE`). |
| `-fastlaunch` | Optional upstream flag to skip the DOSBox-X splash screen for faster manual testing. |

### Quick Launch Examples

```powershell
# 1. Launch with an ephemeral in-memory catalog and 30-second countdown banner
& ".\bin\x64\Debug SDL2\dosbox-x.exe" -kiosk-mode -fastlaunch

# 2. Launch directly into the Interactive Launcher with a persistent SQLite catalog
& ".\bin\x64\Debug SDL2\dosbox-x.exe" -kiosk-mode -kiosk-skip-countdown -kiosk-db "C:\temp\kiosk_test.db" -fastlaunch
```

---

## 2. Setting Up a Test Catalog Database (Optional)

Because the graphical Machine/Media Setup Wizard is built in Phases 5–6, you can seed a persistent `catalog.db` with `sqlite3` (or Python's built-in `sqlite3` module) to test multi-disk games, hardware archetypes, and real HDD boots right away.

1. **Create the target directory and initialize the schema** by launching once with `-kiosk-db` and `-exit`:
   ```powershell
   New-Item -ItemType Directory -Force -Path "C:\temp" | Out-Null
   & ".\bin\x64\Debug SDL2\dosbox-x.exe" -kiosk-mode -kiosk-db "C:\temp\kiosk_test.db" -fastlaunch -exit
   ```
2. **Seed sample machines, media, and caddy bindings** (replace the file paths with real `.img`, `.ima`, `.iso`, or `.cue` files on your machine):
   ```powershell
   $seedScript = @'
   import sqlite3
   con = sqlite3.connect(r'C:\temp\kiosk_test.db')
   cur = con.cursor()

   # Optional: shorten boot countdown to 10s and customize overlay hotkey
   cur.execute("UPDATE kiosk_config SET boot_timeout_sec = 10, overlay_hotkey = 'F12+D', default_caddy_view = 'SKEUOMORPHIC' WHERE id = 1")

   # Add an Integrated DOS 486DX2 machine
   cur.execute('''INSERT OR REPLACE INTO machines
       (id, display_name, archetype_id, archetype_tier, boot_mode)
       VALUES ('mach-486', '486DX2-66 Sound Blaster 16', 'ARCH_486DX2', 4, 'INTEGRATED_DOS')''')

   # Add a Catalog Item (Game)
   cur.execute('''INSERT OR REPLACE INTO catalog_items
       (id, machine_id, item_type, title, launch_mechanism, host_container_dir, guest_executable, return_on_exit)
       VALUES ('item-demo', 'mach-486', 'GAME', 'Multi-Disk Demo', 'INTEGRATED_AUTOEXEC', 'C:\\temp\\dosgames', 'COMMAND.COM', 1)''')

   # Register floppy & CD-ROM images in the Global Media Pool
   cur.execute('''INSERT OR REPLACE INTO media_pool
       (id, file_path, media_type, physical_style, label_title, label_subtitle, is_write_protected)
       VALUES
       ('med-fd1', 'C:\\temp\\disk1.img', 'FLOPPY_35_1440K', 'BEIGE_35', 'Install Disk 1', '3.5" 1.44MB HD', 0),
       ('med-fd2', 'C:\\temp\\disk2.img', 'FLOPPY_35_1440K', 'BLUE_35',  'Install Disk 2', '3.5" 1.44MB HD', 1),
       ('med-cd1', 'C:\\temp\\game.iso',  'CDROM_ISO',       'JEWEL_CD', 'Game Data CD',   'CD-ROM ISO9660', 1)''')

   # Bind media to the Catalog Item's Session Caddy
   cur.execute("DELETE FROM session_caddy WHERE catalog_item_id = 'item-demo'")
   cur.execute('''INSERT INTO session_caddy (catalog_item_id, media_id, target_drive, slot_order) VALUES
       ('item-demo', 'med-fd1', 'A', 1),
       ('item-demo', 'med-fd2', 'A', 2),
       ('item-demo', 'med-cd1', 'D', 1)''')

   con.commit()
   con.close()
   print(r'Seeded C:\temp\kiosk_test.db successfully.')
   '@
   python -c $seedScript
   ```

---

## 3. Controls Quick Reference

### Pre-Boot States (`STATE_COUNTDOWN_IDLE` & `STATE_SHELL_INTERACTIVE`)

| Input | Action |
|---|---|
| **Any Key / Mouse Click / Mouse Movement ($\ge 5\text{ px}$) / Gamepad Button / Analog Stick** | Cancels the auto-boot countdown banner and opens the Pre-Boot Interactive Launcher. |
| **Mouse / Arrow Keys / Tab / Enter / Gamepad D-Pad & A** | Select **"Boot Default DOSBox-X Shell"**, a **Catalog Title**, or a **Machine Profile**. |

### Live Emulation (`STATE_EMULATION_RUNNING`)

| Input | Action |
|---|---|
| **`F12 + D`** (default) | Opens the **Runtime Media Caddy & Disk-Swap Overlay** (pauses CPU & audio, releases mouse capture). |
| **`F11 + O`** (built-in alias) | Opens the **Runtime Media Caddy & Disk-Swap Overlay**. |
| **Gamepad `Guide` / `Home` Button** | Opens the **Runtime Media Caddy & Disk-Swap Overlay**. |
| **Custom `kiosk_config.overlay_hotkey`** | Opens the overlay if customized in SQLite (e.g. `F12+O`, `F11+D`, `F10`). |

### Inside Runtime Overlay (`STATE_RUNTIME_OVERLAY`)

| Input | Action |
|---|---|
| **`V`** / **`F2`** / **Gamepad `Y`** | Toggles between **Skeuomorphic Shelf View** and **Compact List View**. |
| **`Left`** / **`Right`** / **Gamepad D-Pad Left & Right** | Moves selection highlight across disks in the Session Caddy. |
| **`Enter`** / **Click Card** / **Gamepad `A`** | Hot-swaps the highlighted floppy (`A:`/`B:`) or CD-ROM (`D:`/`E:`) immediately. |
| **`Esc`** / **`F12 + D`** / **`F11 + O`** / **Gamepad `B` or `Guide`** | Closes the overlay and resumes live emulation (unpauses audio, restores relative mouse lock). |

---

## 4. Manual Test Walkthroughs by Phase

### Phase 1: Core Subsystems, SQLite Persistence & Config Synthesis
1. **In-Memory Fallback Test:**
   - Run `dosbox-x.exe -kiosk-mode -kiosk-db "Z:\nonexistent\bad.db" -fastlaunch`.
   - Confirm DOSBox-X does not crash; it logs a fallback to `:memory:` and starts normally.
2. **Hardware Archetype Synthesis:**
   - Seed machines using each of the 5 built-in archetypes: `ARCH_XT_8088`, `ARCH_AT_286`, `ARCH_386DX`, `ARCH_486DX2`, and `ARCH_PENTIUM_3DFX`.
   - Launch a machine from the Interactive Launcher and inspect `%TEMP%\kiosk_session.conf` while the session is running to verify the synthesized `[cpu]`, `[dosbox]`, `[sblaster]`, `[voodoo]`, `[ide, secondary]`, and `[autoexec]` blocks match the chosen archetype.

---

### Phase 2: Watchdog Countdown, Launcher Shell & Guest Return Lifecycle
1. **Auto-Boot Watchdog Countdown:**
   - Launch with `-kiosk-mode` (without `-kiosk-skip-countdown`).
   - Verify the bottom banner displays: `Auto-booting in N seconds... Press any key or controller button to enter Kiosk Menu`.
   - **Test A (Timeout):** Do not touch keyboard/mouse/gamepad; verify it automatically boots the configured `default_boot_action` (`DEFAULT_DOSBOX`, `LAST_PLAYED`, `SPECIFIC_MACHINE`, or `SPECIFIC_ITEM`) when the timer hits `0`.
   - **Test B (Cancel):** Relaunch and press a key or move the mouse $\ge 5$ pixels; verify the banner disappears and the Pre-Boot Launcher window opens.
2. **Clean Return to Kiosk Hub:**
   - Boot into an `INTEGRATED_DOS` session and type `EXIT` at the DOS prompt, **or** open the overlay (`F12+D`) and click **"Quit to Kiosk Hub"**.
   - Verify the session exits cleanly and relaunches directly into the Pre-Boot Interactive Launcher (`-kiosk-skip-countdown`) without showing the countdown again.
3. **Guest Hardware Shutdown Traps (`REAL_HDD_BOOT`):**
   - Inside a booted MS-DOS or Windows 9x guest OS, trigger a software power-off (`Shut Down` in Windows 95/98 via APM `INT 15h AX=5307h`, or running `B:\KIOSKEXT.COM` which writes `"Shutdown"` to Bochs I/O port `0x8900`).
   - Verify DOSBox-X intercepts the shutdown cleanly and returns to the Kiosk Hub instead of hanging at `"It is now safe to turn off your computer"`.

---

### Phase 3: Ephemeral Boot Injector (`REAL_HDD_BOOT`)
To test Phase 3 manually, add a machine in `machines` with `boot_mode = 'REAL_HDD_BOOT'` and `primary_hdd_path` pointing to a bootable FAT12/FAT16/FAT32 `.img` hard disk image (make a backup copy of your HDD image first).

1. **One-Time Non-Destructive `AUTOEXEC.BAT` Hook:**
   - Launch a `CatalogItemRecord` bound to that `REAL_HDD_BOOT` machine (Mode A).
   - Verify Kiosk injects the single-line hook `IF EXIST B:\KIOSKGO.BAT CALL B:\KIOSKGO.BAT` into `C:\AUTOEXEC.BAT` inside the HDD image (only once; subsequent launches do not duplicate the line).
2. **Ephemeral Dispatch Floppy (`B:`):**
   - While running, verify `%TEMP%\kiosk_dispatch.img` is generated and mounted on Drive `B:` (`IMGMOUNT 1`) containing `B:\KIOSKGO.BAT` and `B:\KIOSKEXT.COM`.
   - When the launched game/app exits (`return_on_exit = 1`), `B:\KIOSKGO.BAT` automatically executes `B:\KIOSKEXT.COM` to power off the guest and return to the Kiosk Hub.
   - Verify `%TEMP%\kiosk_dispatch.img` is deleted automatically upon session teardown.
3. **Mode B (Machine-Only Boot):**
   - Launch the `REAL_HDD_BOOT` machine directly from the **Machine Profiles** list (not a specific catalog item).
   - Verify the guest boots to its normal desktop/DOS prompt (`B:\KIOSKGO.BAT` is omitted; only `B:\KIOSKEXT.COM` is present if no user floppy is bound to `B:`).

---

### Phase 4: Skeuomorphic Media Caddy & Runtime Disk-Swap Overlay
1. **Opening & Audio/Mouse Freeze:**
   - While inside a running DOS game or shell (ideally with Sound Blaster or CD-Audio playing and mouse captured), press **`F12+D`** (or **`F11+O`** / **Gamepad Guide**).
   - Verify:
     - Audio pauses cleanly with no DMA ring-buffer buzz.
     - Mouse cursor unlocks and becomes visible.
     - The paused guest video frame remains visible behind a dimmed backdrop.
2. **Skeuomorphic Rendering & Header Inspection:**
   - Inspect your mounted/caddy disks in **Skeuomorphic Shelf View**:
     - **5.25" floppies** (360KB / 1.2MB) render as charcoal flexible jackets with hub ring, index hole, head slot, and right-edge write-protect notch (silver tab when write-protected).
     - **3.5" microfloppies** (720KB / 1.44MB / 2.88MB) render as hard plastic shells in deterministic Beige / Cobalt Blue / Matte Black with brushed metal shutters and write-protect window.
     - **CD-ROMs** (`.iso` / `.cue`) render in polycarbonate jewel cases with iridescent silver or gold discs and an **`AUDIO`** badge when `.cue` sheets contain `TRACK <N> AUDIO`.
     - Verify FAT volume labels (`0x2B` / root `0x08`) and ISO9660 volume labels (`0x8028`) appear on the paper stickers.
3. **Dual-Mode View Toggle:**
   - Press **`V`**, **`F2`**, or **Gamepad `Y`** (or click the top-right view toggle button).
   - Verify instant switching between **Skeuomorphic Shelf** and **Compact List Table**.
4. **Live Floppy Hot-Swap, 100ms Door-Open & Write Protection:**
   - Click another floppy card (or select it with `Left`/`Right` and press `Enter` / Gamepad `A`).
   - Verify the green **`IN DRIVE A:`** badge moves to the new disk and the status footer confirms `FDC 0x3F7 DSKCHG 100ms`.
   - Click **`WP: Writable` / `WP: LOCKED`** on Drive `A:` and resume (`Esc`). Try creating a file on `A:` (`ECHO TEST > A:\TEST.TXT`) in DOS and verify writes are blocked when `WP: LOCKED` and permitted when `WP: Writable`.
   - Click **`Eject Door`** on Drive `A:`, resume, verify `DIR A:` reports drive not ready, then re-open `F12+D` and re-insert a floppy to verify recovery.
   - *(During `REAL_HDD_BOOT` item launch)*: Verify Drive `B:` displays **`SYSTEM DISPATCH - LOCKED`** so `kiosk_dispatch.img` cannot be accidentally ejected or overwritten.
5. **Live ATAPI CD-ROM Hot-Swap & Redbook CD-Audio Volume:**
   - Insert or swap a `.iso` / `.cue` disc into Drive `D:` (either from the Session Caddy or by expanding **`[+] Global Media / USB`** and clicking **`Insert D: (CD)`**).
   - Verify the status footer reports `ATAPI Unit Attention 0x06/0x28`, resume emulation, and run `DIR D:` (in DOS) or check Windows Explorer (in Win9x) to confirm the guest OS refreshes the disc label and directory contents.
   - Drag the **`CD Vol`** slider (`0%–100%`) on the Drive `D:` card to verify live Redbook CD-Audio mixer scaling.
6. **Footer Actions:**
   - Click **`Send Ctrl+Alt+Del Reboot`** to send a warm reboot key sequence to the guest OS and resume.
   - Click **`Quit to Kiosk Hub`** to abort the session cleanly and return to the Pre-Boot Launcher.

---

## 5. Automated Verification Scripts (Developers & QA)

You can also run the automated Phase 1–4 verification suite at any time from the repository root:

```powershell
# Run all 37 Kiosk Google Tests (Phases 1-4)
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests -TestFilter "Kiosk*"

# Run only Phase 4 Disk Caddy & Runtime Swap tests
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1 -Tests -TestFilter "KioskDiskCaddyTest*:KioskRuntimeSwapTest*:*OverlayCachesSessionCaddy*"

# Run CLI & Subsystem Smoke Test
powershell -ExecutionPolicy Bypass -File .agents/skills/dosbox-config-tester/scripts/test-kiosk.ps1
```
