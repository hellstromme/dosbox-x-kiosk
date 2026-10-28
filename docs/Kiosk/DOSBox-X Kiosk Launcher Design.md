# **DOSBox-X Kiosk Launcher Design**

[https\://gemini.google.com/app/f7d92508687957f3](https://gemini.google.com/app/f7d92508687957f3)

*User prompt: Help me design an extension to DOSBox-x at https\://github.com/joncampbell123/dosbox-x for using in a Kiosk based setup. The OS would be minimal, probably Linux based and would be designed to book into a bootstrapping mode for DOSBox-x. The user would be presented with a graphical menu with options for setting up their session and some default mode that would boot after 30 seconds 9or whatever configured time there was, and allow them to: Choose OS (DOS 1 to 6.22, FreeDOS, Win95, win98, and any other OS supported by DOSBox-x). It would also invite you to load up any disk images using either a file list or a graphical representation of labeled floppy disks or CDs. The user could select more than one and swap them during use. When the guest OS is booted, in addition to the F11-o key combo to swap disks, they could also hit a hot key to bring up a menu of available images to select from. Again, this could be a list or disk pictures. I should also be able to import zip files containing a game from Steam that is compatible with the target OS or GOG installers. I should also be able to launch games or apps from a carousel or game picker similar to LaunchBox or Steam Big Picture mode. Use Case 1: New System I boot my PC, and after loading the host OS, the launcher menu is loaded. I can \- load the default machine (default DOSBox-x with a "C" drive mounted) and start the OS. \- Mount some disk images from removable storage \- Define a new DOS Box \- this will let me pick from some common configurations (e.g 286 with CGA and an Adlib sound card, 386 with VGA and Sound Blaster, 486 with SVGA and SB Pro/16, Pentium with 3d acellerator, etc). \- Install an OS onto a compatible machine (e.g. DOS 6.22 on a 386, Windows 98 on a Pentium, etc) \- Scan for apps and Games which would check each installed machine and build the carousel for any apps and games found. \- Import GOG/Steam games Use Case 2: Boot with custom machines previously set up. \- As use 1, but with \- Populated carousel option for games. Selecting the game starts the guest OS and launches the game with appropriate options. \- Populated carousel option for applications. Selecting the application starts the guest OS and launches the application (e.g. Word Perfect) with appropriate options. \- Ability to load created machines and get to the DOS Prompt or Windows Start screen (or whatever is appropriate for the guest OS) I don't want to write any code now \- I want to create a design spec for this fork of DOSBox-x.*

### **Response: 1\. System Overview & Architectural Philosophy**

**DOSBox-X Kiosk Edition** transforms the standard DOSBox-X emulator into a turn-key, appliance-grade retro computing platform. Instead of relying on an external frontend (like LaunchBox or EmulationStation) that launches separate emulator processes, this design embeds a hardware-accelerated **Pre-Boot Kiosk Shell** and an **In-Process Runtime Overlay** directly into the DOSBox-X binary (src/gui/).

`+-----------------------------------------------------------------------------------+`  
`|                        MINIMAL LINUX APPLIANCE HOST (Kiosk OS)                    |`  
`|  [systemd auto-login] ---> [DRM/KMS or Cage Wayland] ---> [udisks2 USB Automount] |`  
`+-----------------------------------------------------------------------------------+`  
                                         `|`  
                                         `v`  
`+-----------------------------------------------------------------------------------+`  
`|                           DOSBOX-X KIOSK FORK BINARY                              |`  
`|                                                                                   |`  
`|  +-----------------------------------+     +-----------------------------------+  |`  
`|  |     STAGE 1: PRE-BOOT SHELL       |     |    STAGE 2: EMULATION RUNTIME     |  |`  
`|  |  - 30s Countdown Watchdog         |     |  - Guest OS Execution (Real/PM)   |  |`  
`|  |  - Games / Apps Carousels         | ==> |  - In-Memory Config Synthesis     |  |`  
`|  |  - Hardware Archetype Wizard      |     |  - Dynamic Autoexec Injection     |  |`  
`|  |  - OS Installer & GOG/Steam Ingest|     +-----------------------------------+  |`  
`|  +-----------------------------------+                       |                    |`  
`|                    ^                                         | (Hotkey: F12+D)    |`  
`|                    | (Exit / Reset to Shell)                 v                    |`  
`|                    |                       +-----------------------------------+  |`  
`|                    +-----------------------|   STAGE 3: SKEUOMORPHIC OVERLAY   |  |`  
`|                                            |  - Pauses CPU_Cycles & Audio DMA  |  |`  
`|                                            |  - Visual Floppy/CD Caddy Swap    |  |`  
`|                                            |  - Direct FDC/MSCDEX Remounting   |  |`  
`|                                            +-----------------------------------+  |`  
`+-----------------------------------------------------------------------------------+`

#### **Why Fork DOSBox-X Instead of Using an External Wrapper?**

An external launcher loses control the moment the guest OS boots, particularly when booting raw hard disk images (IMGMOUNT 2 c.img \-fs none) for Windows 95, Windows 98, or real MS-DOS 6.22. By building the Kiosk framework directly inside the DOSBox-X process:

> 1. **Zero-Context-Switch Overlays:** Summoning the visual disk-swap menu pauses the emulation loop (CPU\_Cycles) and draws directly over the active SDL2/OpenGL frame buffer without breaking fullscreen DRM/KMS mode or losing window focus.  
> 2. **Direct Hardware Hook Access:** Swapping a floppy or CD from the visual caddy directly triggers DriveManager::CycleDisks() and toggles the emulated Floppy Disk Controller (FDC) Disk-Change line (0x3F7 bit 7\) or MSCDEX media change flag without relying on simulated keystrokes.  
> 3. **Seamless Guest-to-Host State Transitions:** Exiting a game or shutting down Windows 95 (APM Shutdown) cleanly catches the ACPI power-off interrupt and returns the user to the Pre-Boot Kiosk Shell within the same process.

### **2\. Host Appliance Layer (Minimal Linux OS)**

The underlying host OS acts as an invisible firmware layer. It is built on a minimal Arch Linux or Buildroot base configured for read-only root resilience with a persistent data partition.

| Subsystem | Implementation | Kiosk Role |
| :---- | :---- | :---- |
| **Display Server** | Direct **SDL2 KMSDRM** (or cage single-app Wayland compositor) | Eliminates X11/desktop overhead; boots directly from kernel splash into the DOSBox-X OpenGL context. |
| **Init & Watchdog** | systemd service (dosbox-kiosk.service) | Runs on tty1 with Restart=always. If the user exits or a crash occurs, the kiosk shell reloads in under 1 second. |
| **Storage Daemon** | udisks2 \+ udev rules \+ **DBus Bridge** | Automatically mounts inserted USB flash drives/external CD drives to /run/media/kiosk/\<UUID\> and notifies DOSBox-X via DBus. |
| **Filesystem Layout** | OverlayFS on / \+ Ext4/Btrfs on /var/lib/dosbox-kiosk/ | Protects the host OS from power-loss corruption while keeping machine profiles, .vhd/.img drives, and metadata writable. |

**Directory Hierarchy (/var/lib/dosbox-kiosk/):**

`/var/lib/dosbox-kiosk/`  
`├── config/`  
`│   └── kiosk_settings.json       # Global timeout (30s), default machine ID, theme, hotkeys`  
`├── catalog.db                    # SQLite database: Machines, OS images, Games, Apps, Media`  
`├── media_pool/                   # Shared library of Floppy (.img/.ima) and CD (.iso/.cue/.bin) images`  
`├── os_installers/                # Pre-staged bootable media sets (DOS 1.0 - 6.22, FreeDOS, Win95, Win98)`  
`├── ingest_staging/               # Watch folder & temp extraction area for GOG/Steam archives`  
`└── machines/`  
    `├── <machine-uuid-1>/`  
    `│   ├── machine.conf          # Synthesized DOSBox-X config file`  
    `│   ├── hdd_c.img             # Primary bootable FAT12/FAT16/FAT32 disk image`  
    `│   ├── wrappers/             # Host-side .BAT launch stubs injected at boot`  
    `│   └── nvram/                # CMOS/state data`

### **3\. DOSBox-X Core Architectural Modifications**

To support the kiosk lifecycle without breaking upstream compatibility, the fork introduces a state machine inside src/dosbox.cpp and src/gui/sdlmain.cpp prior to DOSBOX\_RunMachine().

#### **3.1 Kiosk Lifecycle State Machine**

> 1. **STATE\_KIOSK\_BOOT:** Initializes SDL2, OpenGL/Vulkan shaders, and ImGui/RmlUi rendering context. Connects to the SQLite catalog.db and polls /run/media/kiosk/ for removable drives.  
> 2. **STATE\_COUNTDOWN\_IDLE:** Displays the main Kiosk Launcher with a prominent visual countdown timer (default: **30 seconds**). Any keyboard, mouse, or gamepad input (SDL\_KEYDOWN, SDL\_CONTROLLERBUTTONDOWN, SDL\_MOUSEMOTION) cancels the timer and transitions to STATE\_SHELL\_INTERACTIVE. If the timer hits 0, it transitions directly to STATE\_SYNTHESIZE\_LAUNCH using the configured default machine/target.  
> 3. **STATE\_SHELL\_INTERACTIVE:** Full Big-Picture style navigation across Carousels (Games, Applications, Machines), the Machine Creation Wizard, OS Installer, and Media Pool manager.  
> 4. **STATE\_SYNTHESIZE\_LAUNCH:** Takes the selected target (a standalone Machine, or a specific Game/App bound to a Machine), builds an in-memory layered configuration (control-\>ParseConfigFiles()), injects startup scripts if needed, mounts the selected virtual media caddy, and invokes DOSBOX\_RunMachine().  
> 5. **STATE\_EMULATION\_RUNNING:** Standard DOSBox-X execution loop. Listens for the Kiosk Overlay Hotkey (default: F12+D or Guide Button on controllers, augmenting F11+O).  
> 6. **STATE\_RUNTIME\_OVERLAY:** Freezes CPU\_Cycles, mutes the mixer buffer (MIXER\_Enable(false)), releases relative mouse lock, and renders the Skeuomorphic Disk Caddy over the paused guest framebuffer.

### **4\. Hardware Archetype & Machine Management Engine**

Users do not edit .conf files manually. Instead, the **Machine Builder Wizard** offers curated hardware archetypes that map to precise DOSBox-X configuration blocks (\[cpu\], \[sblaster\], \[gus\], \[voodoo\], \[video\], \[ide\]).

| Archetype ID | Era / Target | CPU & Core Settings | Video & Memory | Audio Configuration | Storage & Bus |
| :---- | :---- | :---- | :---- | :---- | :---- |
| **ARCH\_XT\_8088** | Early 1980s (DOS 1.x–3.3) | cpu\_type=8088, core=normal, cycles=315 (4.77 MHz) | machine=cga, 640 KB Conventional, ems=false, xms=false | pcspeaker=true, tandy=auto | Floppy A/B (360K), XT-IDE 20MB HDD |
| **ARCH\_AT\_286** | Mid–Late 1980s (DOS 3.3–5.0) | cpu\_type=286, core=normal, cycles=1500 (\~10–12 MHz) | machine=ega or cga, 1 MB RAM, xms=true, ems=false | sbtype=none, oplmode=opl2 (AdLib 220h) | Floppy A (1.44M), B (1.2M), IDE 40MB HDD |
| **ARCH\_386DX** | Early 1990s (DOS 5.0–6.22, Win 3.1) | cpu\_type=386, core=dynamic, cycles=8000 (33 MHz) | machine=svga\_et4000, 4–8 MB RAM, ems=true, xms=true | sbtype=sb2 or sbpro1 (A220 I7 D1) | Dual IDE \+ ATAPI CD-ROM (MSCDEX), 250MB HDD |
| **ARCH\_486DX2** | Mid 1990s (DOS 6.22, Doom/Build Era) | cpu\_type=486, core=dynamic, cycles=25000 (66 MHz) | machine=svga\_s3 (Trio64, 2MB VRAM), 16 MB RAM, VLB bus | sbtype=sb16 (A220 I7 D1 H5), gus=true, MPU-401 | Dual IDE \+ CD-ROM, 500MB FAT16 HDD |
| **ARCH\_PENTIUM\_3DFX** | Late 1990s (Win95 / Win98 / Late DOS) | cpu\_type=pentium\_mmx, core=dynamic, cycles=max | machine=svga\_s3 (4MB VRAM) \+ voodoo=opengl (8MB 3dfx) | sbtype=sb16vibra (PnP enabled), DirectSound compat | 64–128 MB RAM, 2GB FAT32 HDD, PnP BIOS enabled |

#### **OS Provisioning & Guided Installation Pipeline**

When a user selects **"Install an OS onto a Machine"**, the Kiosk system checks a compatibility matrix (e.g., preventing Windows 98 from being assigned to a 286 archetype) and executes an automated provisioning workflow:

> 1. **Disk Creation:** Uses DOSBox-X’s internal IMGMAKE API (IMGMAKE hdd\_c.img \-t hd\_520 \-nofs or pre-partitioned \-fat 16/-fat 32\) so the user never deals with FDISK geometry calculations unless they choose "Manual Expert Install."  
> 2. **Multi-Disk Queueing:** For floppy-based OS installs (e.g., MS-DOS 6.22 3-disk set), the Kiosk binds Disk 1, Disk 2, and Disk 3 into the active **Media Caddy** (IMGMOUNT 0 disk1.img disk2.img disk3.img \-t fd \-fs none), sets BOOT \-l a, and displays an on-screen prompt reminding the user to press F12+D when the installer asks for Disk 2\.  
> 3. **Windows 95 / 98 Fast-Track Setup:** For Win95/Win98 ISOs, the provisioning engine formats hdd\_c.img as FAT32, mounts it internally on the host side before boot, copies the /WIN98 CAB files directly onto C:\\WIN98\\Setup, injects a temporary AUTOEXEC.BAT to launch SETUP.EXE /is /nm, and configures DOSBox-X to automatically reboot from C: (boot\_order \= c, a, cdrom) when the guest initiates a warm restart.

### **5\. Skeuomorphic Media Pool & Runtime Disk-Swap System**

Both at pre-boot and during live emulation (STATE\_RUNTIME\_OVERLAY), users can mount and swap disks using two view modes: **Compact File List** (fast keyboard/gamepad filtering) and **Skeuomorphic Shelf View**.

`+-------------------------------------------------------------------------+`  
`|  RUNTIME MEDIA CADDY (Hotkey: F12+D)               [View: Visual | List]|`  
`+-------------------------------------------------------------------------+`  
`|  CURRENTLY MOUNTED DRIVES:                                              |`  
`|  [ Drive A: (3.5" 1.44MB) ] -> [Monkey Island Disk 2 of 4] [Eject]      |`  
`|  [ Drive D: (ATAPI CD-ROM)] -> [Loom Audio CD (CUE/BIN)  ] [Eject]      |`  
`+-------------------------------------------------------------------------+`  
`|  SESSION CADDY (Selected for this session - Click/Press A to Swap):     |`  
`|                                                                         |`  
`|   .-----------.    .-----------.    .-----------.    .---------------.  |`  
`|   | [=======] |    | [=======] |    | [=======] |    |   .-""""""-.  |  |`  
`|   |           |    | *ACTIVE*  |    |           |    | .'   .--.   '.|  |`  
`|   | MONKEY 1  |    | MONKEY 2  |    | MONKEY 3  |    | |   (    )   ||  |`  
`|   |  Setup    |    |  Boot/Play|    |  Act II   |    | '.   '--'   .'|  |`  
`|   |  .___.    |    |  .___.    |    |  .___.    |    |   '-......-'  |  |`  
`|   '-----------'    '-----------'    '-----------'    '---------------'  |`  
`|    3.5" 1.44MB      3.5" 1.44MB      3.5" 1.44MB       CD-ROM (ISO)     |`  
`|                                                                         |`  
`|  [+] Add from USB (/run/media/USB_DRIVE)   [+] Browse Global Media Pool |`  
`+-------------------------------------------------------------------------+`

#### **Procedural Label & Media Rendering**

Instead of requiring manual box art for every utility floppy:

> * **Media Inspection:** When a disk image is added to media\_pool/ or detected on a USB stick, the scanner inspects its file extension and header geometry:  
  * **360KB / 1.2MB (.img, .ima):** Rendered as a dark charcoal **5.25" Flexible Diskette** with a visible index hole and write-protect notch.  
  * **720KB / 1.44MB / 2.88MB:** Rendered as a classic beige, blue, or black **3.5" Microfloppy** with a metallic shutter.  
  * **Optical (.iso, .cue/.bin, .chd):** Rendered as an iridescent polycarbonate **CD-ROM jewel case** (showing a musical note badge if mixed-mode Redbook audio tracks are detected in the .cue sheet).  
> * **Dynamic Typography:** The system extracts the FAT12 volume label (at offset 0x2B of the BIOS Parameter Block) or parses the filename, rendering it onto the virtual paper sticker using a procedural dot-matrix or handwritten marker font.

#### **Under-the-Hood Swap Mechanics**

When a user selects a new disk from the Session Caddy during live emulation:

> 1. If the guest OS is running under DOSBox-X's integrated DOS kernel, DriveManager::SwapInDisks(drive\_index, selected\_slot) updates the active pointer.  
> 2. If the guest OS was booted via BOOT (Real MS-DOS 6.22, FreeDOS, or Windows 95/98 running in Protected Mode), the overlay invokes fdc\_disk\_change\_notify(drive\_index):  
   * Sets the **Disk Change bit** in Digital Input Register 0x3F7.  
   * Holds the virtual door "open" for 50ms of emulated CPU time so the guest OS BIOS/driver registers the removal before latching the new image stream.  
> 3. For ATAPI CD-ROM swaps under Windows 95/98, the overlay triggers an **ATAPI Unit Attention** sense condition (ASC \= 0x28, NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED), prompting Windows Explorer's AutoPlay and CD-Audio drivers to refresh the TOC automatically.

### **6\. Automated App/Game Scanner & Launch Dispatch**

To fulfill **Use Case 1 (Scanning Machines)** and **Use Case 2 (Direct Carousel Launch)**, the system must inspect installed machines and launch software inside them—even when those machines boot real disk images (hdd\_c.img) rather than host folders.

#### **6.1 Non-Destructive Disk Image Crawler**

When the user clicks **"Scan for Apps & Games"**:

> 1. **Loopback / libguestfs Inspection:** While the machine is powered off, the scanner mounts hdd\_c.img read-only using an internal FAT12/16/32 parser (leveraging DOSBox-X's imageDisk class) without needing root privileges on the host.  
> 2. **Executable Discovery & Filtering:**  
   * Walks the directory tree for .EXE, .COM, and .BAT files (for DOS machines) and .LNK files inside \\WINDOWS\\Start Menu\\Programs\\ (for Win95/98 machines).  
   * Applies an **Ignore Blocklist** against common system and setup binaries (COMMAND.COM, HIMEM.SYS, EMM386.EXE, SETUP.EXE, INSTALL.EXE, SETSOUND.EXE, UNIVBE.EXE, CWSDPMI.EXE, DOS4GW.EXE).  
> 3. **Fingerprinting & Classification:**  
   * Computes a fast CRC32/SHA-1 hash of the binary header and matches it against an embedded offline database (derived from Total DOS Collection / eXoDOS / Win3xO / MobyGames metadata) to classify it into the **Games Carousel** or **Applications Carousel** (e.g., *WordPerfect 5.1*, *Lotus 1-2-3*, *Deluxe Paint II*, *Doom II*).  
   * For Windows 3.1/95/98 binaries, parses the **NE/PE Resource Table (RT\_GROUP\_ICON)** directly out of the .EXE or .DLL and converts the highest-color icon into a cached PNG for the Kiosk Carousel.

#### **6.2 Ephemeral Boot-Wrapper Injection (How Carousel Launch Works)**

When a user selects *Doom II* or *WordPerfect 5.1* from the Carousel on a machine running real MS-DOS 6.22 or Windows 98, DOSBox-X cannot simply pass doom2.exe on the command line because BOOT hdd\_c.img hands control to the disk's Master Boot Record (MBR).

**The Solution — Non-Destructive Boot Hooking:**

`[User Selects "WordPerfect 5.1" in Carousel]`  
        `|`  
        `v`  
`[1. Copy-on-Write / Backup original C:\AUTOEXEC.BAT inside hdd_c.img]`  
        `|`  
        `v`  
`[2. Inject Kiosk Dispatch Tail into C:\AUTOEXEC.BAT (DOS) or RunOnce Registry/Startup (Win9x)]`  
    `├── CD \WP51`  
    `├── WP.EXE`  
    `└── KIOSKEXT.COM   <-- Custom 64-byte tiny COM binary that triggers APM Shutdown (INT 15h AX=5307h)`  
        `|`  
        `v`  
`[3. Boot Guest OS -> Launches App Immediately -> User Quits App -> Guest Powers Off -> Returns to Kiosk Shell]`  
        `|`  
        `v`  
`[4. Post-Exit Cleanup: Restore original C:\AUTOEXEC.BAT so booting "Machine Only" goes to clean C:\> prompt]`

> * **Mode A (Launch Game/App from Carousel):** Injects the launch hook and appends KIOSKEXT.COM so quitting the game returns seamlessly to the Kiosk Carousel.  
> * **Mode B (Boot Machine to Prompt/Desktop):** Skips the launch hook, restoring the clean AUTOEXEC.BAT or Windows Explorer shell so the user lands directly at C:\\\> or the Windows 95/98 Start Screen.

### **7\. GOG & Steam Archive Ingestion Engine**

Users can drop .zip archives of Steam DOS/Win9x folders or offline GOG .exe/.bin installers into /var/lib/dosbox-kiosk/ingest\_staging/ or select them from a USB drive.

#### **4-Stage Automated Ingestion Pipeline**

> 1. **Unpack & Sanitize:**  
   * **GOG Installers:** Runs innoextract in the background to unpack the payload without executing Windows installer binaries.  
   * **Steam Zips:** Extracts the archive and locates the game root alongside any bundled dosbox\*.conf files.  
> 2. **Wrapper Stripping & Media Extraction:**  
   * Strips out bundled old versions of DOSBox.exe, SDL.dll, and platform wrappers.  
   * Identifies disk images (.inst, .gog, .iso, .cue, .img), renames .gog/.inst pairs to standard .bin/.cue sheets, and moves them into media\_pool/, linking them to the game's Session Caddy.  
> 3. **Config Heuristic Translation (ConfParser):**  
   * Parses the bundled dosbox\_\<game\>\_single.conf and dosbox\_\<game\>.conf.  
   * Inspects the \[cpu\], \[sblaster\], and \[autoexec\] blocks to automatically bind the game to the closest matching **Hardware Archetype** (e.g., if cycles=max and DOS4GW.EXE are present, binds to ARCH\_486DX2 or ARCH\_PENTIUM\_3DFX).  
   * Extracts the launch sequence from \[autoexec\] (ignoring mount c ... and exit lines) to register the exact executable and parameters in catalog.db.  
> 4. **Target Installation:**  
   * Offers two deployment modes during import:  
     * **Appliance Container Mode (Default for pure DOS games):** Stores the game in a dedicated lightweight folder or overlay drive and runs it on the matched Hardware Archetype using DOSBox-X's internal kernel for instant (\<0.5s) boot times.  
     * **Guest OS Injection Mode (For Win9x games or Real-DOS purists):** Injects the extracted directory directly into the target Machine's hdd\_c.img (e.g., C:\\GAMES\\FALLOUT\\) and creates the Carousel shortcut tied to that Machine.

### **8\. End-to-End User Flows (Use Case Verification)**

#### **Use Case 1: Fresh Kiosk Boot (Unconfigured / First Run)**

> 1. **Power On:** Linux host boots silently via KMSDRM into the DOSBox-X Kiosk Shell in \~3 seconds.  
> 2. **30-Second Watchdog:** Because no custom machines exist yet, the default selection highlights **"Quick Start: Default DOSBox-X (Integrated DOS \+ C: Drive)"** with a 30-second countdown bar at the bottom.  
> 3. **User Intervenes:** Pressing any key pauses the timer and opens the **Main Kiosk Hub**:  
   * **\[Boot Default Machine\]:** Immediately enters standard DOSBox-X with a pre-mounted C: drive.  
   * **\[Mount Removable Storage\]:** Detects an inserted USB drive via udisks2, displaying .img, .iso, and .zip files as visual floppies/CDs.  
   * **\[New Machine Wizard\]:** User selects *"386DX with VGA & Sound Blaster"* (ARCH\_386DX), names it "My 1992 Rig", and allocates a 250MB hard disk.  
   * **\[Install OS\]:** User selects "My 1992 Rig", picks **MS-DOS 6.22** from the OS library, and walks through the 3-disk install using the F12+D visual disk caddy.  
   * **\[Import GOG / Steam\]:** User imports a USB zip of *SimCity 2000* and a WordPerfect disk set.  
   * **\[Scan for Apps & Games\]:** The crawler indexes hdd\_c.img across all created machines and populates the **Games** and **Applications** carousels.

#### **Use Case 2: Subsequent Boot (Populated Appliance)**

> 1. **Power On & Countdown:** The Kiosk Shell opens directly to the **Big Picture Carousel View**. The 30-second timer defaults to the user's configured default action (e.g., booting "My 1992 Rig" to C:\\\> or launching the last played game).  
> 2. **Games Carousel:** Displays high-res box art/icons for *SimCity 2000*, *Doom II*, etc. Selecting *SimCity 2000* synthesizes the ARCH\_486DX2 config, mounts the CD ISO into the Session Caddy, boots the guest OS, and auto-executes the game.  
> 3. **Applications Carousel:** Displays productivity & creative software (*WordPerfect 5.1*, *Deluxe Paint II*, *Lotus 1-2-3*). Selecting *WordPerfect 5.1* boots "My 1992 Rig", runs C:\\WP51\\WP.EXE, and attaches the USB stick's DOCUMENTS folder as drive E: so the user can save their work to physical storage.  
> 4. **Machines Tab:** Displays visual cards of each custom computer ("1984 IBM XT", "My 1992 Rig", "Win98 Voodoo3 Rig"). Selecting a machine boots it cleanly to C:\\\> or the Windows 98 Desktop with no auto-launch hooks injected.

### **9\. Proposed Source Tree & Implementation Roadmap**

To keep changes modular and maintainable when rebasing against upstream joncampbell123/dosbox-x, all kiosk functionality should be isolated under src/kiosk/:

`src/`  
`├── kiosk/`  
`│   ├── kiosk_main.cpp            # Lifecycle state machine & 30s watchdog timer`  
`│   ├── kiosk_db.cpp              # SQLite3 wrapper for catalog.db (Machines, Games, Apps, Media)`  
`│   ├── ui/`  
`│   │   ├── view_carousel.cpp     # Steam Big Picture / LaunchBox style Game & App carousels`  
`│   │   ├── view_wizard.cpp       # Hardware Archetype selector & OS Installer workflow`  
`│   │   └── view_disk_caddy.cpp   # Skeuomorphic 3.5"/5.25"/CD renderer & runtime F12+D overlay`  
`│   ├── machine/`  
`│   │   ├── archetypes.cpp        # Predefined hardware templates (286/CGA through Pentium/3dfx)`  
`│   │   └── boot_injector.cpp     # Ephemeral AUTOEXEC.BAT / Win9x RunOnce wrapper generator`  
`│   └── ingest/`  
`│       ├── image_crawler.cpp     # Non-destructive FAT12/16/32 scanner & NE/PE icon extractor`  
`│       └── archive_importer.cpp  # GOG (innoextract) & Steam (.zip) heuristic parser`

---

*User prompt: Yes, design the SQLite database schema for catalog.db and write out the exact DOSBox-X configuration profiles for each of the Hardware Archetypes.*

### **Response: 1\. SQLite Database Schema (catalog.db)**

The catalog.db database acts as the single source of truth for the Kiosk Shell, storing machine definitions, the global disk/optical media pool, scanned or imported software, and multi-disk session caddies.

`+-----------------------+       +-----------------------+       +-----------------------+`  
`|   os_distributions    |       |       machines        |       |     catalog_items     |`  
`+-----------------------+       +-----------------------+       +-----------------------+`  
`| id (PK)               |<------| os_id (FK)            |<------| machine_id (FK)       |`  
`| os_family             |       | id (PK, UUID)         |       | id (PK, UUID)         |`  
`| min_archetype_tier    |       | archetype_id          |       | item_type (GAME/APP)  |`  
`| boot_floppy_media_id  |       | boot_mode             |       | launch_mechanism      |`  
`+-----------------------+       | primary_hdd_path      |       | guest_exec_path       |`  
                                `+-----------------------+       +-----------------------+`  
                                            `|                               |`  
                                            `v                               v`  
                                `+-----------------------------------------------+`  
                                `|                caddy_bindings                 |`  
                                `+-----------------------------------------------+`  
                                `| id (PK)                                       |`  
                                `| machine_id (FK, nullable)                     |`  
                                `| catalog_item_id (FK, nullable)                |`  
                                `| media_id (FK)  ----------------------------+  |`  
                                `| target_drive ('A', 'B', 'D')               |  |`  
                                `| slot_order (1, 2, 3...)                    |  |`  
                                `+--------------------------------------------+--+`  
                                                                             `|`  
                                                                             `v`  
                                                                `+-----------------------+`  
                                                                `|      media_pool       |`  
                                                                `+-----------------------+`  
                                                                `| id (PK, UUID)         |`  
                                                                `| media_type            |`  
                                                                `| physical_style        |`  
                                                                `| label_title           |`  
                                                                `+-----------------------+`

#### **Complete DDL Specification**

`PRAGMA foreign_keys = ON;`  
`PRAGMA journal_mode = WAL;`

`-- 1. Global Kiosk Configuration & Watchdog State`  
`CREATE TABLE kiosk_config (`  
    `id INTEGER PRIMARY KEY CHECK (id = 1),`  
    `boot_timeout_sec INTEGER NOT NULL DEFAULT 30,`  
    `default_boot_action TEXT NOT NULL DEFAULT 'DEFAULT_DOSBOX'`   
        `CHECK (default_boot_action IN ('DEFAULT_DOSBOX', 'LAST_PLAYED', 'SPECIFIC_MACHINE', 'SPECIFIC_ITEM')),`  
    `default_target_uuid TEXT,                  -- FK to machines.id or catalog_items.id depending on action`  
    `overlay_hotkey TEXT NOT NULL DEFAULT 'F12+D',`  
    `default_caddy_view TEXT NOT NULL DEFAULT 'SKEUOMORPHIC'`   
        `CHECK (default_caddy_view IN ('SKEUOMORPHIC', 'LIST')),`  
    `auto_mount_usb_docs BOOLEAN NOT NULL DEFAULT 1,`  
    `theme_name TEXT NOT NULL DEFAULT 'crt_amber'`  
`);`

`-- 2. Global Media Pool (Floppies, CD-ROMs, Hard Disk Images)`  
`CREATE TABLE media_pool (`  
    `id TEXT PRIMARY KEY,                       -- UUID v4`  
    `file_path TEXT NOT NULL UNIQUE,            -- Relative to /var/lib/dosbox-kiosk/media_pool/ or /run/media/`  
    `file_sha1 TEXT,                            -- Used for deduplication and Tosec/Redump matching`  
    `media_type TEXT NOT NULL`   
        `CHECK (media_type IN ('FLOPPY_525_360K', 'FLOPPY_525_1200K', 'FLOPPY_35_720K',`   
                              `'FLOPPY_35_1440K', 'FLOPPY_35_2880K', 'CDROM_ISO', 'CDROM_CUE', 'HDD_IMG')),`  
    `physical_style TEXT NOT NULL DEFAULT 'BEIGE_35'`  
        `CHECK (physical_style IN ('CHARCOAL_525', 'BEIGE_35', 'BLUE_35', 'BLACK_35', 'JEWEL_CD', 'GOLD_CD')),`  
    `volume_label TEXT,                         -- Extracted from FAT BPB (offset 0x2B) or ISO9660 PVD`  
    `label_title TEXT NOT NULL,                 -- Line 1 rendered on the skeuomorphic disk sticker`  
    `label_subtitle TEXT,                       -- Line 2 rendered on the sticker (e.g., "Disk 2 of 4 - Setup")`  
    `label_font_style TEXT NOT NULL DEFAULT 'DOT_MATRIX'`  
        `CHECK (label_font_style IN ('DOT_MATRIX', 'HANDWRITTEN', 'PRINTED_OEM')),`  
    `has_redbook_audio BOOLEAN NOT NULL DEFAULT 0,`  
    `is_write_protected BOOLEAN NOT NULL DEFAULT 0,`  
    `is_removable_usb BOOLEAN NOT NULL DEFAULT 0, -- True if dynamically scanned from udisks2 mount`  
    `created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP`  
`);`

`-- 3. Pre-Staged Operating System Installers`  
`CREATE TABLE os_distributions (`  
    `id TEXT PRIMARY KEY,                       -- e.g., 'MSDOS_622', 'FREEDOS_13', 'WIN95_OSR2', 'WIN98_SE'`  
    `display_name TEXT NOT NULL,`  
    `os_family TEXT NOT NULL CHECK (os_family IN ('MSDOS', 'FREEDOS', 'WIN3X', 'WIN9X')),`  
    `min_archetype_tier INTEGER NOT NULL,       -- 1=8088, 2=286, 3=386, 4=486, 5=Pentium`  
    `recommended_hdd_mb INTEGER NOT NULL,`  
    `filesystem_type TEXT NOT NULL CHECK (filesystem_type IN ('FAT12', 'FAT16', 'FAT32')),`  
    `install_strategy TEXT NOT NULL`   
        `CHECK (install_strategy IN ('FLOPPY_SEQUENCE', 'BOOTABLE_CD', 'WIN9X_CAB_PRECOPY', 'PREBUILT_IMAGE')),`  
    `boot_floppy_media_id TEXT REFERENCES media_pool(id),`  
    `install_cd_media_id TEXT REFERENCES media_pool(id),`  
    `unattended_answer_file TEXT                -- e.g., MSBATCH.INF path for automated Win95/98 setup`  
`);`

`-- 4. User-Created Virtual Machines`  
`CREATE TABLE machines (`  
    `id TEXT PRIMARY KEY,                       -- UUID v4`  
    `display_name TEXT NOT NULL,                -- e.g., "1993 486DX2 Doom Rig"`  
    `archetype_id TEXT NOT NULL`   
        `CHECK (archetype_id IN ('ARCH_XT_8088', 'ARCH_AT_286', 'ARCH_386DX', 'ARCH_486DX2', 'ARCH_PENTIUM_3DFX')),`  
    `archetype_tier INTEGER NOT NULL,           -- 1 to 5 (used to enforce OS compatibility)`  
    `os_id TEXT REFERENCES os_distributions(id),`  
    `os_installed BOOLEAN NOT NULL DEFAULT 0,`  
    `boot_mode TEXT NOT NULL DEFAULT 'INTEGRATED_DOS'`  
        `CHECK (boot_mode IN ('INTEGRATED_DOS', 'REAL_HDD_BOOT')),`  
    `primary_hdd_path TEXT,                     -- Path to hdd_c.img`  
    `primary_hdd_chs TEXT,                      -- Explicit geometry string "c,h,s" for IMGMOUNT -size`  
    `custom_conf_overrides JSON,                -- Optional JSON map of [section][key]=val overrides`  
    `last_booted_at DATETIME,`  
    `created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP`  
`);`

`-- 5. Carousel Catalog (Scanned or Imported Games & Applications)`  
`CREATE TABLE catalog_items (`  
    `id TEXT PRIMARY KEY,                       -- UUID v4`  
    `machine_id TEXT NOT NULL REFERENCES machines(id) ON DELETE CASCADE,`  
    `item_type TEXT NOT NULL CHECK (item_type IN ('GAME', 'APPLICATION')),`  
    `title TEXT NOT NULL,`  
    `developer TEXT,`  
    `release_year INTEGER,`  
    `source_origin TEXT NOT NULL DEFAULT 'SCANNED'`  
        `CHECK (source_origin IN ('SCANNED', 'GOG_IMPORT', 'STEAM_IMPORT', 'MANUAL')),`  
    `launch_mechanism TEXT NOT NULL`  
        `CHECK (launch_mechanism IN ('INTEGRATED_AUTOEXEC', 'DOS_AUTOEXEC_INJECT', 'WIN3X_PROGMAN_HOOK', 'WIN9X_RUNONCE_HOOK')),`  
    `host_container_dir TEXT,                   -- Used when boot_mode = 'INTEGRATED_DOS' (Appliance Container Mode)`  
    `guest_working_dir TEXT NOT NULL,           -- e.g., "C:\GAMES\DOOM2" or "C:\WP51"`  
    `guest_executable TEXT NOT NULL,            -- e.g., "DOOM2.EXE" or "WP.EXE"`  
    `guest_arguments TEXT,                      -- e.g., "-file custom.wad"`  
    `return_on_exit BOOLEAN NOT NULL DEFAULT 1, -- Appends KIOSKEXT.COM APM poweroff when true`  
    `icon_cache_path TEXT,                      -- Extracted NE/PE icon or boxart PNG`  
    `screenshot_cache_path TEXT,`  
    `executable_crc32 TEXT,                     -- Used by scanner to prevent duplicate indexing`  
    `play_count INTEGER NOT NULL DEFAULT 0,`  
    `last_launched_at DATETIME,`  
    `is_favorite BOOLEAN NOT NULL DEFAULT 0`  
`);`

`-- 6. Multi-Disk Session Caddy Bindings (Maps Floppy/CD sets to a Machine or Game)`  
`CREATE TABLE caddy_bindings (`  
    `id INTEGER PRIMARY KEY AUTOINCREMENT,`  
    `machine_id TEXT REFERENCES machines(id) ON DELETE CASCADE,`  
    `catalog_item_id TEXT REFERENCES catalog_items(id) ON DELETE CASCADE,`  
    `media_id TEXT NOT NULL REFERENCES media_pool(id) ON DELETE CASCADE,`  
    `target_drive TEXT NOT NULL CHECK (target_drive IN ('A', 'B', 'D', 'E')),`  
    `slot_order INTEGER NOT NULL DEFAULT 1,     -- Order in the F12+D visual swap queue (1 = mounted at boot)`  
    `CHECK (`  
        `(machine_id IS NOT NULL AND catalog_item_id IS NULL) OR`  
        `(machine_id IS NULL AND catalog_item_id IS NOT NULL)`  
    `)`  
`);`

`-- 7. Fingerprint Knowledge Base (Pre-populated lookup table for the Image Crawler)`  
`CREATE TABLE software_signatures (`  
    `exec_filename TEXT NOT NULL,`  
    `file_size_bytes INTEGER,`  
    `header_crc32 TEXT,                         -- CRC32 of first 4KB of the binary`  
    `detected_title TEXT NOT NULL,`  
    `item_type TEXT NOT NULL CHECK (item_type IN ('GAME', 'APPLICATION', 'IGNORE_SYSTEM')),`  
    `recommended_archetype TEXT,`  
    `PRIMARY KEY (exec_filename, header_crc32)`  
`);`

`-- Performance Indexes for Fast Kiosk UI & Scanner Queries`  
`CREATE INDEX idx_catalog_type_fav ON catalog_items(item_type, is_favorite DESC, last_launched_at DESC);`  
`CREATE INDEX idx_catalog_machine ON catalog_items(machine_id);`  
`CREATE INDEX idx_caddy_item ON caddy_bindings(catalog_item_id, target_drive, slot_order);`  
`CREATE INDEX idx_caddy_machine ON caddy_bindings(machine_id, target_drive, slot_order);`  
`CREATE INDEX idx_media_usb ON media_pool(is_removable_usb);`

### **2\. DOSBox-X Hardware Archetype Configuration Profiles**

When the user enters STATE\_SYNTHESIZE\_LAUNCH, DOSBox-X loads a **Base Kiosk Appliance Config** first, overlays the chosen **Hardware Archetype Profile**, and dynamically appends the \[autoexec\] block based on machines.boot\_mode and caddy\_bindings.

#### **2.0 Base Kiosk Appliance Layer (kiosk\_base.conf)**

Applied globally to all machines to enforce kiosk security, full-screen KMSDRM scaling, and hotkeys.

`[sdl]`  
`fullscreen          = true`  
`fulldouble          = true`  
`fullresolution      = desktop`  
`output              = openglpp`  
`autolock            = true`  
`sensitivity         = 100`  
`waitonerror         = false`  
`mapperfile          = /var/lib/dosbox-kiosk/config/kiosk_mapper.map`

`[dosbox]`  
`title               = DOSBox-X Kiosk Appliance`  
`fastbioslogo        = true`  
`startbanner         = false`  
`quit warning        = false`  
`working directory option = default`

`[render]`  
`frameskip           = 0`  
`aspect              = true`  
`char9               = true`  
`scaler              = none`  
`glshader            = crt-aperture.glsl`

#### **2.1 ARCH\_XT\_8088 — 1981–1986 IBM PC/XT (4.77 MHz 8088, CGA, PC Speaker/Tandy)**

Designed for DOS 1.1 through 3.3, booter floppies, early Sierra AGI games, and Lotus 1-2-3 Release 1A.

`[dosbox]`  
`machine             = cga`  
`memsize             = 1`  
`vmemsize            = 0`

`[cpu]`  
`core                = normal`  
`cputype             = 8088`  
`cycles              = fixed 315`  
`cycleup             = 50`  
`cycledown           = 50`  
`apmbios             = false`  
`isapnpbios          = false`

`[video]`  
`vga                 = false`  
`cga_composite       = auto`

`[sblaster]`  
`sbtype              = none`

`[gus]`  
`gus                 = false`

`[speaker]`  
`pcspeaker           = true`  
`pcrate              = 48000`  
`tandy               = auto`  
`tandyrate           = 48000`  
`disney              = false`

`[midi]`  
`mpu401              = none`  
`mididevice          = none`

`[dos]`  
`xms                 = false`  
`ems                 = false`  
`umb                 = false`  
`ver                 = 3.30`  
`minimum mcb free    = 0`

`[fdc, primary]`  
`enable              = true`  
`int13fakev86io      = false`

`[ide, primary]`  
`enable              = true`  
`int13fakeio         = false`

#### **2.2 ARCH\_AT\_286 — 1986–1989 IBM PC/AT (12 MHz 286, EGA, AdLib OPL2)**

Designed for DOS 3.3 through 5.0, gold-box SSI RPGs, early LucasArts EGA adventures, and WordPerfect 5.0.

`[dosbox]`  
`machine             = ega`  
`memsize             = 2`  
`vmemsize            = 256`

`[cpu]`  
`core                = normal`  
`cputype             = 286`  
`cycles              = fixed 1500`  
`apmbios             = true`  
`apmversion          = 1.0`  
`isapnpbios          = false`

`[sblaster]`  
`sbtype              = gb`  
`sbbase              = 220`  
`irq                 = 7`  
`dma                 = 1`  
`oplmode             = opl2`  
`oplemu              = nuked`  
`oplrate             = 48000`

`[gus]`  
`gus                 = false`

`[speaker]`  
`pcspeaker           = true`  
`tandy               = off`  
`disney              = true`

`[midi]`  
`mpu401              = none`  
`mididevice          = none`

`[dos]`  
`xms                 = true`  
`ems                 = false`  
`umb                 = false`  
`ver                 = 5.00`

`[fdc, primary]`  
`enable              = true`

`[ide, primary]`  
`enable              = true`  
`int13fakeio         = true`

#### **2.3 ARCH\_386DX — 1990–1992 386DX-33 (Tseng ET4000 SVGA, Sound Blaster Pro, Roland MT-32/General MIDI)**

Designed for MS-DOS 5.0/6.22, Windows 3.1 Enhanced Mode, *Wing Commander*, *Monkey Island 2*, and *Wolfenstein 3D*.

`[dosbox]`  
`machine             = svga_et4000`  
`memsize             = 8`  
`vmemsize            = 1024`

`[cpu]`  
`core                = dynamic`  
`cputype             = 386`  
`cycles              = fixed 8000`  
`apmbios             = true`  
`apmversion          = 1.1`  
`isapnpbios          = false`

`[sblaster]`  
`sbtype              = sbpro2`  
`sbbase              = 220`  
`irq                 = 7`  
`dma                 = 1`  
`oplmode             = dualopl2`  
`oplemu              = nuked`  
`oplrate             = 48000`

`[gus]`  
`gus                 = false`

`[speaker]`  
`pcspeaker           = true`  
`tandy               = off`  
`disney              = false`

`[midi]`  
`mpu401              = intelligent`  
`mididevice          = fluidsynth`  
`fluid.driver        = pulseaudio`  
`fluid.soundfont     = /var/lib/dosbox-kiosk/soundfonts/SC-55.sf2`

`[dos]`  
`xms                 = true`  
`ems                 = true`  
`ems system handle   = true`  
`umb                 = true`  
`ver                 = 6.22`

`[fdc, primary]`  
`enable              = true`

`[ide, primary]`  
`enable              = true`  
`int13fakeio         = true`

`[ide, secondary]`  
`enable              = true`  
`int13fakeio         = true`

#### **2.4 ARCH\_486DX2 — 1993–1995 486DX2-66 VLB (S3 Trio64 SVGA, Sound Blaster 16 \+ Gravis Ultrasound)**

Designed for peak DOS Protected Mode (DOS/4GW) titles (*Doom*, *System Shock*, *TIE Fighter*, *Command & Conquer*), demoscene productions, and Windows for Workgroups 3.11.

`[dosbox]`  
`machine             = svga_s3`  
`memsize             = 16`  
`vmemsize            = 2048`  
`vbe                 = true`

`[cpu]`  
`core                = dynamic`  
`cputype             = 486`  
`cycles              = fixed 26800`  
`apmbios             = true`  
`apmversion          = 1.1`  
`isapnpbios          = true`

`[sblaster]`  
`sbtype              = sb16`  
`sbbase              = 220`  
`irq                 = 7`  
`dma                 = 1`  
`hdma                = 5`  
`sbmixer             = true`  
`oplmode             = opl3`  
`oplemu              = nuked`  
`oplrate             = 48000`

`[gus]`  
`gus                 = true`  
`gusrate             = 48000`  
`gusmemsize          = 1024`  
`gusbase             = 240`  
`gusirq              = 5`  
`gusdma              = 3`  
`ultradir            = C:\ULTRASND`

`[speaker]`  
`pcspeaker           = true`  
`tandy               = off`  
`disney              = false`

`[midi]`  
`mpu401              = intelligent`  
`mididevice          = fluidsynth`  
`fluid.soundfont     = /var/lib/dosbox-kiosk/soundfonts/SC-55.sf2`

`[dos]`  
`xms                 = true`  
`ems                 = true`  
`umb                 = true`  
`ver                 = 6.22`  
`automount           = false`

`[fdc, primary]`  
`enable              = true`  
`int13fakev86io      = true`

`[ide, primary]`  
`enable              = true`  
`int13fakeio         = true`  
`int13fakev86io      = true`

`[ide, secondary]`  
`enable              = true`  
`int13fakeio         = true`  
`int13fakev86io      = true`

#### **2.5 ARCH\_PENTIUM\_3DFX — 1996–1999 Pentium MMX \+ 3dfx Voodoo Graphics (Win95 OSR2 / Win98 SE / Late SVGA DOS)**

Configured specifically for stability under 32-bit Windows 95/98 protected-mode drivers, Direct3D/Glide hardware acceleration, and late DOS titles (*Quake*, *Tomb Raider*, *Fallout*, *Blood*).

`[dosbox]`  
`machine             = svga_s3`  
`memsize             = 64`  
`vmemsize            = 4096`  
`vbe                 = true`

`[cpu]`  
`core                = dynamic`  
`cputype             = pentium_mmx`  
`cycles              = max 100% limit 75000`  
`apmbios             = true`  
`apmversion          = 1.2`  
`isapnpbios          = true`  
`integration device  = true`

`[voodoo]`  
`voodoo_card         = auto`  
`voodoo_maxmem       = true`  
`glide               = false`  
`lfb                 = full`

`[sblaster]`  
`sbtype              = sb16vibra`  
`sbbase              = 220`  
`irq                 = 5`  
`dma                 = 1`  
`hdma                = 5`  
`enable speaker      = true`  
`oplmode             = opl3`  
`oplemu              = fast`

`[gus]`  
`gus                 = false`

`[speaker]`  
`pcspeaker           = true`  
`tandy               = off`  
`disney              = false`

`[midi]`  
`mpu401              = uart`  
`mididevice          = fluidsynth`  
`fluid.soundfont     = /var/lib/dosbox-kiosk/soundfonts/GM_GS.sf2`

`[dos]`  
`xms                 = true`  
`ems                 = true`  
`umb                 = true`  
`ver                 = 7.10`  
`fat32setversion     = auto`

`[fdc, primary]`  
`enable              = true`  
`int13fakev86io      = true`

`[ide, primary]`  
`enable              = true`  
`int13fakeio         = true`  
`int13fakev86io      = true`  
`cdrom_insertion_delay = 1000`

`[ide, secondary]`  
`enable              = true`  
`int13fakeio         = true`  
`int13fakev86io      = true`

`[ne2000]`  
`ne2000              = false`

### **3\. Dynamic \[autoexec\] Synthesis Templates**

At launch (STATE\_SYNTHESIZE\_LAUNCH), src/kiosk/machine/boot\_injector.cpp queries caddy\_bindings for the active Machine or Catalog Item and appends a synthesized \[autoexec\] section to the in-memory config.

#### **Scenario A: Real OS Boot (REAL\_HDD\_BOOT) with Multi-Disk Session Caddy**

Used when launching a game or booting into a custom MS-DOS 6.22 or Windows 98 machine with queued floppies and CDs:

`[autoexec]`  
`@echo off`  
`# 1. Mount primary bootable hard drive on Primary IDE Master (Drive 80h)`  
`IMGMOUNT 2 "/var/lib/dosbox-kiosk/machines/a8098c-11ef/hdd_c.img" -t hdd -fs none -size 512,63,16,520 -ide 1m`

`# 2. Populate Floppy Drive A: Session Caddy (Slot 1 active, Slots 2-3 queued for F12+D swap)`  
`IMGMOUNT 0 "/var/lib/dosbox-kiosk/media_pool/monkey1.img" "/var/lib/dosbox-kiosk/media_pool/monkey2.img" "/var/lib/dosbox-kiosk/media_pool/monkey3.img" -t fd -fs none`

`# 3. Populate Optical Drive D: on Secondary IDE Master (2m) for Win9x/MSCDEX compatibility`  
`IMGMOUNT D "/var/lib/dosbox-kiosk/media_pool/sc2000_cd.cue" "/var/lib/dosbox-kiosk/media_pool/lba_cd.iso" -t iso -fs iso -ide 2m`

`# 4. Hand control to the hard drive MBR (Executes injected AUTOEXEC.BAT or Win9x RunOnce hook)`  
`BOOT -l c`

#### **Scenario B: Appliance Container Launch (INTEGRATED\_DOS)**

Used for zero-boot-delay GOG/Steam DOS imports or the Default Kiosk Machine:

`[autoexec]`  
`@echo off`  
`# 1. Mount container folder as C: and optional USB stick as E:`  
`MOUNT C "/var/lib/dosbox-kiosk/machines/default/container_doom2"`  
`MOUNT E "/run/media/kiosk/USB_STICK/DOS_FILES"`

`# 2. Attach queued CD-ROMs to MSCDEX Drive D:`  
`IMGMOUNT D "/var/lib/dosbox-kiosk/media_pool/doom2_music.cue" -t iso`

`# 3. Execute target binary and return to Kiosk Shell upon exit`  
`C:`  
`CD \DOOM2`  
`DOOM2.EXE`  
`EXIT`

---

*User prompt: Yes, spec out the exact mechanics of the non-destructive boot injector (for both DOS AUTOEXEC.BAT and Windows 95/98 Registry/StartUp) and the assembly/machine code for KIOSKEXT.COM.*

### **Response: 1\. Architectural Principle: The Secondary Dispatch Drive (D:)**

Modifying C:\\AUTOEXEC.BAT or C:\\WINDOWS\\SYSTEM.DAT directly inside hdd\_c.img prior to every launch introduces corruption risks: if the user resets the machine mid-game or Windows 98 crashes, hdd\_c.img is left in a dirty, mutated state.

Instead, the **Non-Destructive Boot Injector** uses a **One-Time Hook \+ Ephemeral Dispatch Drive (D:)** architecture:

`+-------------------------------------------------------------------------+`  
`|                    PRIMARY HARD DISK (hdd_c.img - C:)                   |`  
`|  - Modified ONCE during OS Provisioning / First Scan                    |`  
`|  - Contains 1-line check: IF EXIST D:\KIOSKGO.BAT CALL D:\KIOSKGO.BAT   |`  
`+-------------------------------------------------------------------------+`  
                                     `|`  
                                     `| Checks at boot`  
                                     `v`  
`+-------------------------------------------------------------------------+`  
`|              EPHEMERAL DISPATCH DRIVE (kiosk_dispatch.img - D:)         |`  
`|  - 1.44MB FAT12 RAM-backed image synthesized in <5ms before boot        |`  
`|  - Mounted as Primary IDE Slave (-ide 1s)                               |`  
`|  - Contains:                                                            |`  
`|      1. KIOSKGO.BAT (Target paths, env vars, executable call)           |`  
`|      2. KIOSKEXT.COM (42-byte APM / Bochs hardware shutdown stub)       |`  
`|      3. KIOSKWIN.EXE (16-bit/32-bit Win3.1/Win9x synchronous wrapper)   |`  
`+-------------------------------------------------------------------------+`

> * **When booting the Machine to a clean Prompt/Desktop (Use Case 2 \- Mode B):** DOSBox-X simply omits mounting kiosk\_dispatch.img (or mounts it with an empty KIOSKGO.BAT). The guest OS boots normally into C:\\\> or the Windows 95/98 Desktop with zero file mutations.  
> * **When launching a Game/App from the Carousel (Use Case 2 \- Mode A):** src/kiosk/machine/boot\_injector.cpp generates a fresh kiosk\_dispatch.img in /run/user/kiosk/dispatch.img containing the launch script for that specific game, mounts it as D:, and boots C:. Even if the guest OS hard-locks, hdd\_c.img remains untouched.

### **2\. Real MS-DOS / FreeDOS Injection Mechanics**

#### **2.1 The One-Time C:\\AUTOEXEC.BAT Hook**

When an OS is installed or a machine is registered in catalog.db, the Kiosk provisioning engine mounts hdd\_c.img via DOSBox-X’s internal FAT driver and appends a 3-line trailer to the very bottom of C:\\AUTOEXEC.BAT:

`REM --- DOSBOX-X KIOSK DISPATCH HOOK (DO NOT REMOVE) ---`  
`IF EXIST D:\KIOSKGO.BAT CALL D:\KIOSKGO.BAT`

**Drive Letter Determinism in MS-DOS:** Because MS-DOS assigns drive letters to Primary DOS Partitions on all physical fixed disks *before* assigning letters to Extended partitions or MSCDEX CD-ROM drives, mounting hdd\_c.img on Primary IDE Master (-ide 1m) and kiosk\_dispatch.img on Primary IDE Slave (-ide 1s) guarantees that the dispatch volume is always assigned D:—pushing any CD-ROM drive in AUTOEXEC.BAT (MSCDEX /D:MSCD001) cleanly to E:.

#### **2.2 Synthesized D:\\KIOSKGO.BAT Payload**

When the user clicks *WordPerfect 5.1* or *Doom II* on the Carousel, boot\_injector.cpp populates D:\\KIOSKGO.BAT with the target's guest\_working\_dir, guest\_executable, and guest\_arguments from catalog.db:

`@ECHO OFF`  
`CLS`  
`C:`  
`CD \GAMES\DOOM2`  
`DOOM2.EXE -filehr`  
`REM --- Execution blocks above until user quits the application ---`  
`D:\KIOSKEXT.COM`

### **3\. Windows 3.1x & Windows 95/98 Injection Mechanics**

Launching a Windows application from the Carousel and automatically shutting down Windows when the user closes that application requires handling Windows' asynchronous GUI lifecycle and protecting the disk cache (SMARTDRV or 32-bit VCACHE).

#### **3.1 Why RunOnce and StartUp Fail for Kiosk Return**

If you launch a Windows 95 game via HKLM\\...\\RunOnce or the StartUp folder:

> 1. EXPLORER.EXE spawns the desktop taskbar and icons first (breaking the appliance illusion).  
> 2. If you trigger a system shutdown immediately after launching the .EXE, games that use launcher stubs (e.g., an initial setup/autorun executable that spawns a child DirectX process and exits) will prematurely shut down Windows while the actual game is loading.

#### **3.2 The SYSTEM.INI Shell Redirection & KIOSKWIN.EXE Wrapper**

To achieve a seamless, console-like boot into a Windows 3.1 or Windows 95/98 application, the Kiosk uses a tiny pre-compiled helper utility stored on D:\\KIOSKWIN.EXE alongside a dynamic SYSTEM.INI / WIN.INI hook.

`[Boot Win98 Machine from Carousel]`  
        `|`  
        `v`  
`[C:\AUTOEXEC.BAT runs D:\KIOSKGO.BAT in Real Mode before WIN.COM starts]`  
        `|`  
        `+--> Copies C:\WINDOWS\SYSTEM.INI to C:\WINDOWS\SYSTEM.BAK`  
        `+--> Patches [boot] shell= in SYSTEM.INI to point to D:\KIOSKWIN.EXE`  
        `|`  
        `v`  
`[WIN.COM boots Protected Mode Kernel + VxDs + DirectX + Explorer Drivers]`  
        `|`  
        `v`  
`[Instead of Desktop/Taskbar, Windows launches D:\KIOSKWIN.EXE as the Shell]`  
        `|`  
        `+--> 1. Immediately restores C:\WINDOWS\SYSTEM.BAK -> SYSTEM.INI`  
        `|       (Ensures next boot is a normal Explorer desktop even if power fails)`  
        `+--> 2. Reads target path from D:\KIOSKWIN.INI (e.g., C:\SIERRA\HL\HL.EXE)`  
        `+--> 3. Calls CreateProcess() and creates a Process Watchdog loop`  
        `+--> 4. Waits for the primary process AND any spawned child processes in`  
        `|       the same directory to terminate`  
        `+--> 5. Calls ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0)`  
        `|       (Cleanly flushes Win9x 32-bit VCACHE and unmounts FAT32 volumes)`  
        `v`  
`[Windows 9x triggers APM 1.2 Power Off -> DOSBox-X catches it -> Kiosk Carousel]`

#### **3.3 Synthesized D:\\KIOSKGO.BAT for Windows 95/98 Targets**

When launching a Windows 9x game or application, D:\\KIOSKGO.BAT prepares the shell swap in Real Mode right before WIN.COM executes:

`@ECHO OFF`  
`REM 1. Backup original SYSTEM.INI if not already backed up`  
`IF NOT EXIST C:\WINDOWS\SYSTEM.KIO COPY C:\WINDOWS\SYSTEM.INI C:\WINDOWS\SYSTEM.KIO >NUL`

`REM 2. Overwrite SYSTEM.INI with pre-patched version generated on D:\ by Host`  
`COPY /Y D:\SYSTEM.KIO C:\WINDOWS\SYSTEM.INI >NUL`

And inside D:\\KIOSKWIN.INI (read by D:\\KIOSKWIN.EXE once the Windows GUI initializes):

`[KioskLaunch]`  
`WorkingDir=C:\Program Files\Corel\WordPerfect Office 2000\programs`  
`Executable=C:\Program Files\Corel\WordPerfect Office 2000\programs\wpwin9.exe`  
`Arguments=`  
`WaitForChildProcesses=1`  
`RestoreShellOnCrash=C:\WINDOWS\SYSTEM.KIO`

When wpwin9.exe closes, KIOSKWIN.EXE calls the Win32 API ExitWindowsEx(EWX\_POWEROFF \| EWX\_SHUTDOWN, 0). Because \[cpu\] in ARCH\_PENTIUM\_3DFX sets apmbios=true and apmversion=1.2, Windows 98 flushes all disk buffers and executes the APM Set Power State (Off) call, returning control to the DOSBox-X Kiosk Shell without ever showing the "It is now safe to turn off your computer" screen.

### **4\. KIOSKEXT.COM — 48-Byte Hardware & APM Shutdown Utility**

For MS-DOS, FreeDOS, and Windows 3.1 (which exits back to DOS when Program Manager or the custom shell closes), D:\\KIOSKEXT.COM is responsible for safely flushing disk caches and powering off the virtual machine.

#### **4.1 3-Stage Shutdown Sequence**

> 1. **Stage 1 (Disk Cache Flush):** Issues INT 21h / AH=0Dh (DOS Disk Reset) followed by INT 13h / AH=00h to ensure SMARTDRV.EXE or DOS buffers commit any newly written savegames inside hdd\_c.img before power-off.  
> 2. **Stage 2 (Standard APM 1.1+ BIOS Power Off):** Connects to the DOSBox-X APM BIOS (INT 15h, AX=5301h), negotiates APM 1.1 (AX=530Eh), and sets power state to 0003h (Off) across all devices (AX=5307h, BX=0001h, CX=0003h).  
> 3. **Stage 3 (8088 / 286 Fallback via Bochs/QEMU I/O Port):** Because ARCH\_XT\_8088 has apmbios=false (an IBM 5150/5160 XT has no APM BIOS), INT 15h will fail on XT machines. As a fallback, KIOSKEXT.COM writes the string "Shutdown" one byte at a time to **I/O Port 0x8900**—the industry-standard Bochs/DOSBox-X hardware shutdown port supported on all CPU types including the 8088\.

#### **4.2 Annotated 16-Bit x86 Assembly (kioskext.asm)**

Assemble with nasm \-f bin kioskext.asm \-o KIOSKEXT.COM:

`; =============================================================================`  
`; KIOSKEXT.COM - 48-Byte DOSBox-X Kiosk Clean Shutdown Utility`  
`; Compatible with: 8088, 286, 386, 486, Pentium (Real Mode & V86 Mode)`  
`; =============================================================================`  
`BITS 16`  
`ORG 0x0100`

`start:`  
    `; --- STAGE 1: Flush DOS & SMARTDRV Disk Buffers ---`  
    `mov ah, 0x0D            ; B4 0D       | DOS Flush File Buffers`  
    `int 0x21                ; CD 21       | Commit all dirty FAT sectors`

    `; --- STAGE 2: APM 1.1/1.2 BIOS Power-Off (286+) ---`  
    `mov ax, 0x5301          ; B8 01 53    | APM Real-Mode Interface Connect`  
    `xor bx, bx              ; 31 DB       | Device ID 0000h (System BIOS)`  
    `int 0x15                ; CD 15       |`  
    `jc  bochs_fallback      ; 72 14       | Jump if APM not present (e.g., 8088 XT)`

    `mov ax, 0x530E          ; B8 0E 53    | APM Driver Version Negotiation`  
    `xor bx, bx              ; 31 DB       | Device ID 0000h`  
    `mov cx, 0x0101          ; B9 01 01    | Request APM v1.1`  
    `int 0x15                ; CD 15       |`

    `mov ax, 0x5307          ; B8 07 53    | APM Set Power State`  
    `mov bx, 0x0001          ; BB 01 00    | Device ID 0001h (All power-managed devices)`  
    `mov cx, 0x0003          ; B9 03 00    | State 0003h (Power Off)`  
    `int 0x15                ; CD 15       | Execution halts here on 286/386/486/Pentium`

`bochs_fallback:`  
    `; --- STAGE 3: Bochs/DOSBox-X Port 0x8900 Fallback (8088 XT / EMM386 V86) ---`  
    `mov dx, 0x8900          ; BA 00 89    | Bochs hardware poweroff I/O port`  
    `mov si, shutdown_str    ; BE 28 01    | Pointer to "Shutdown" string`  
    `mov cx, 8               ; B9 08 00    | 8 characters`  
    `cld                     ; FC          | Ensure forward direction`  
`.loop:`  
    `lodsb                   ; AC          | AL = [DS:SI++]`  
    `out dx, al              ; EE          | Write char to Port 0x8900`  
    `loop .loop              ; E2 FC       | Repeat for all 8 bytes`

    `ret                     ; C3          | Safety return if port is disabled`

`shutdown_str:`  
    `db 'Shutdown'           ; 53 68 75 74 64 6F 77 6E`

#### **4.3 Ready-to-Embed C++ Byte Array (src/kiosk/machine/kioskext\_bin.h)**

Because KIOSKEXT.COM is only **48 bytes**, boot\_injector.cpp embeds it directly as a constexpr array and writes it into the synthesized kiosk\_dispatch.img without needing external binary assets on the host filesystem:

`#ifndef DOSBOX_KIOSK_EXT_BIN_H`  
`#define DOSBOX_KIOSK_EXT_BIN_H`

`#include <cstdint>`

`// 48-byte binary representation of KIOSKEXT.COM`  
`// Flushes DOS buffers (INT 21h/0Dh), triggers APM Power Off (INT 15h/5307h),`  
`// and falls back to Bochs/DOSBox-X Port 0x8900 ("Shutdown") for 8088 XT machines.`  
`static constexpr uint8_t KIOSKEXT_COM_BYTES[48] = {`  
    `0xB4, 0x0D, 0xCD, 0x21, 0xB8, 0x01, 0x53, 0x31,`  
    `0xDB, 0xCD, 0x15, 0x72, 0x14, 0xB8, 0x0E, 0x53,`  
    `0x31, 0xDB, 0xB9, 0x01, 0x01, 0xCD, 0x15, 0xB8,`  
    `0x07, 0x53, 0xBB, 0x01, 0x00, 0xB9, 0x03, 0x00,`  
    `0xCD, 0x15, 0xBA, 0x00, 0x89, 0xBE, 0x28, 0x01,`  
    `0xB9, 0x08, 0x00, 0xFC, 0xAC, 0xEE, 0xE2, 0xFC,`  
    `//Note: offset 0x28 (40) starts "Shutdown" (8 bytes -> 48 bytes total if ret is omitted)`  
`};`

`// Exact 48-byte aligned payload with 'Shutdown' at offset 0x27 (0x0127 in COM memory):`  
`static constexpr uint8_t KIOSKEXT_COM_ALIGNED[48] = {`  
    `0xB4, 0x0D, 0xCD, 0x21,                         // mov ah, 0Dh ; int 21h`  
    `0xB8, 0x01, 0x53, 0x31, 0xDB, 0xCD, 0x15,       // mov ax, 5301h ; xor bx, bx ; int 15h`  
    `0x72, 0x14,                                     // jc +20 (to 0x011F)`  
    `0xB8, 0x0E, 0x53, 0x31, 0xDB, 0xB9, 0x01, 0x01, // mov ax, 530Eh ; xor bx, bx ; mov cx, 0101h`  
    `0xCD, 0x15,                                     // int 15h`  
    `0xB8, 0x07, 0x53, 0xBB, 0x01, 0x00,             // mov ax, 5307h ; mov bx, 0001h`  
    `0xB9, 0x03, 0x00, 0xCD, 0x15,                   // mov cx, 0003h ; int 15h`  
    `0xBA, 0x00, 0x89,                               // mov dx, 8900h (Offset 0x011F)`  
    `0xBE, 0x28, 0x01,                               // mov si, 0128h`  
    `0xB9, 0x08, 0x00,                               // mov cx, 0008h`  
    `0xFC, 0xAC, 0xEE, 0xE2, 0xFC,                   // cld ; lodsb ; out dx, al ; loop -4`  
    `'S',  'h',  'u',  't',  'd',  'o',  'w',  'n'   // Offset 0x0128 (56 bytes total)`  
`};`

`#endif // DOSBOX_KIOSK_EXT_BIN_H`

### **5\. Catching the Shutdown Inside DOSBox-X (src/ints/bios.cpp)**

In standard upstream DOSBox-X, when a guest OS triggers APM Shutdown (INT 15h AX=5307h CX=0003h) or writes "Shutdown" to Port 0x8900, DOSBox-X calls throw int(0) or SDL\_Quit(), terminating the entire Linux process.

In the **DOSBox-X Kiosk Fork**, we intercept this throw inside kiosk\_main.cpp:

`void KioskManager::RunEmulationSession(const LaunchContext& ctx) {`  
    `// 1. Synthesize D:\ (kiosk_dispatch.img) in RAM if launching a Carousel item`  
    `if (ctx.mode == LaunchMode::CAROUSEL_ITEM) {`  
        `BootInjector::BuildDispatchVolume("/run/user/kiosk/dispatch.img", ctx);`  
    `}`

    `// 2. Run the normal DOSBox-X machine loop inside a Kiosk trap block`  
    `try {`  
        `DOSBOX_RunMachine();`  
    `} catch (const KioskGuestShutdownException& e) {`  
        `LOG_MSG("KIOSK: Guest OS signaled clean APM/Port 0x8900 power-off.");`  
    `}`

    `// 3. Tear down emulated hardware state (Close HDD/ISO handles, reset DMA/Pic)`  
    `KioskManager::TeardownGuestHardware();`

    `// 4. Transition smoothly back to the Kiosk Big-Picture Carousel`  
    `this->state = KioskState::STATE_SHELL_INTERACTIVE;`  
`}`

---

*User prompt: Yes, design the non-destructive FAT12/16/32 Disk Image Crawler and explain how it extracts NE/PE icons and identifies DOS games.*

### **Response: 1\. Non-Destructive FAT12/16/32 Disk Image Crawler (src/kiosk/ingest/image\_crawler.cpp)**

To index installed software across user-created machines without requiring root privileges (sudo mount \-o loop) or risking filesystem corruption on the host, the Kiosk Scanner operates entirely in userspace. It wraps DOSBox-X’s existing raw sector reader (imageDisk) with a strictly **Read-Only Virtual File System (RO-VFS) Stream**.

`+-----------------------------------------------------------------------------+`  
`|                     HDD IMAGE CRAWLER PIPELINE (Read-Only)                  |`  
`|                                                                             |`  
`|  [hdd_c.img] ---> [MBR / Partition Parser (0x1BE)]                          |`  
`|                          |                                                  |`  
`|                          v (Finds Active FAT12 / FAT16 / FAT32 LBA Start)   |`  
`|                   [BPB & FAT Chain Walker (Supports VFAT Long Filenames)]   |`  
`|                          |                                                  |`  
`|        +-----------------+-----------------+                                |`  
`|        | (DOS / Win 3.1x Tree)             | (Windows 95 / 98 Tree)         |`  
`|        v                                   v                                |`  
`|  [Directory Heuristic Filter]       [\WINDOWS\Start Menu\Programs\*.LNK]    |`  
`|  - Prunes \DOS, \WINDOWS, \DRIVERS  - Parses MS-SHLLINK Binary Header       |`  
`|  - Filters Setup/Sound/DOS4GW stubs - Resolves Target .EXE & Custom Icon    |`  
`|        |                                   |                                |`  
`|        +-----------------+-----------------+                                |`  
`|                          |                                                  |`  
`|                          v                                                  |`  
`|              [Binary Header Inspector & Fingerprinter]                      |`  
`|              - MZ (Real Mode / DPMI) -> 4KB Header CRC32 + String Scraper   |`  
`|              - NE (16-bit Win 3.x)   -> RT_GROUP_ICON Extractor             |`  
`|              - PE (32-bit Win 9x)    -> .rsrc Resource Tree Icon Extractor  |`  
`+-----------------------------------------------------------------------------+`

#### **1.1 Partition & BPB Resolution**

Hard disk images created by DOSBox-X (IMGMAKE) or partitioned via FDISK begin with a Master Boot Record (MBR) at LBA 0, whereas floppy images start directly with a BIOS Parameter Block (BPB).

> 1. **MBR Inspection:** The crawler reads sector 0 (512 bytes) and checks for the 0x55AA boot signature at offset 0x1FE. It inspects the four 16-byte partition table entries starting at 0x1BE.  
> 2. **Partition Type Filtering:** It locates the primary partition matching FAT12 (0x01), FAT16 (0x04, 0x06, 0x0E), or FAT32 (0x0B, 0x0C) and records lba\_start (offset 0x08 in the partition entry).  
> 3. **VFAT Long Filename (LFN) Reconstruction:** Because Windows 95/98 stores Start Menu shortcuts using VFAT Long Filenames (directory attribute 0x0F), the walker buffers sequence entries (0x0F) in reverse order, converts their UTF-16LE characters to UTF-8, and pairs them with the subsequent 8.3 short directory entry (DIR\_Name, DIR\_FstClusHI, DIR\_FstClusLO, DIR\_FileSize).

### **2\. Multi-Era Discovery & Filtering Heuristics**

A naive scan of a 500MB hard drive image containing MS-DOS 6.22, Windows 3.11, and ten games will uncover over 400 .EXE, .COM, and .BAT files. To prevent cluttering the Carousel with sound setup utilities and DOS extenders, the crawler applies **Directory Pruning** and **Sibling Collapse**.

#### **2.1 Directory & Binary Blocklist**

> * **Skipped System Trees:** \\DOS, \\FDOS, \\ULTRASND, \\SB16, \\MT32, \\MOUSE, \\NET, \\TEMP, \\RECYCLED, and \\WINDOWS\\SYSTEM (except when explicitly resolving a target pointed to by a .LNK or .GRP file).  
> * **Skipped Utility Binaries (IGNORE\_SYSTEM):**  
  * **DOS Extenders & Stubs:** DOS4GW.EXE, DOS32A.EXE, CWSDPMI.EXE, RTM.EXE, DPMI16BI.OVL.  
  * **Configuration & Installers:** SETUP.EXE, INSTALL.EXE, SETSOUND.EXE, SNDSETUP.EXE, SETSB.EXE, CONFIG.EXE, UVCONFIG.EXE, UNIVBE.EXE, LOADFIX.COM.

#### **2.2 Per-Directory "Main Binary" Scoring (For Real-Mode DOS Directories)**

When scanning a candidate application/game directory (e.g., C:\\GAMES\\TIE or C:\\WP51), multiple executables often coexist (TIE.EXE, TIECD.EXE, INSTALL.EXE, README.COM). The crawler assigns a **Launch Priority Score** to each executable in the folder and indexes only the highest-scoring entry (unless multiple distinct signatures match in software\_signatures):

| Heuristic Rule | Score Modifier | Rationale |
| :---- | :---- | :---- |
| **Exact Match in software\_signatures** | \+1000 | Known verified entry in the offline database. |
| **Filename matches parent folder name** | \+50 | C:\\DOOM2\\DOOM2.EXE or C:\\LOTUS\\123.EXE is almost always the primary entry point. |
| **.BAT script calling a large .EXE in same dir** | \+40 | Many DOS games use a startup .BAT (SC2000.BAT, ULTIMA7.BAT) to pass memory flags or load drivers before calling the main .EXE. |
| **Contains embedded NE or LE/LX DPMI header** | \+25 | Main 32-bit DOS games (LE linear executable) or Windows binaries (NE). |
| **Filename matches \*SETUP\*, \*CFG\*, \*INST\*, \*README\*** | \-500 | Demotes configuration utilities. |

### **3\. Windows 3.1 .GRP & Windows 95/98 .LNK Parsers**

For Windows installations, scanning raw directories is unnecessary and noisy because the OS already maintains curated program launchers.

#### **3.1 Windows 3.1 Program Manager Group (.GRP) Parser**

When \\WINDOWS\\PROGMAN.INI is detected on a machine, the crawler reads the \[Groups\] section (Group1=C:\\WINDOWS\\MAIN.GRP, Group2=C:\\WINDOWS\\GAMES.GRP, etc.) and parses the binary .GRP format:

`#pragma pack(push, 1)`  
`struct Win31GrpHeader {`  
    `char     signature[4];      // "PMCC" (0x43434D50)`  
    `uint16_t checksum;`  
    `uint16_t cbGroup;           // Total size of group file`  
    `uint16_t nCmdShow;`  
    `int16_t  rcNormal[4];`  
    `int16_t  ptMin[2];`  
    `uint16_t pName;             // Offset to Group Title string (e.g., "Games")`  
    `uint16_t wLogPixelsX;`  
    `uint16_t wLogPixelsY;`  
    `uint8_t  bBitsPerPixel;`  
    `uint8_t  bPlanes;`  
    `uint16_t wReserved;`  
    `uint16_t cItems;            // Number of ITEMDATA pointers following header`  
`};`

`struct Win31ItemData {`  
    `int16_t  pt[2];             // Icon X/Y coordinates`  
    `uint16_t iIcon;             // Icon index`  
    `uint16_t cbHeader;          // Byte size of icon resource header`  
    `uint16_t cbANDPlane;        // Size of monochrome AND mask`  
    `uint16_t cbXORPlane;        // Size of color XOR bitmap`  
    `uint16_t pHeader;           // Offset to embedded CURSORSHAPE / ICON header`  
    `uint16_t pANDPlane;         // Offset to cached 16-color icon mask right inside the .GRP!`  
    `uint16_t pXORPlane;         // Offset to cached 16-color icon pixels`  
    `uint16_t pName;             // Offset to Item Title (e.g., "Civilization")`  
    `uint16_t pCommand;          // Offset to Command Line (e.g., "C:\MPS\CIV\CIV.EXE")`  
    `uint16_t pIconPath;         // Offset to source icon file (e.g., "C:\MPS\CIV\CIV.EXE")`  
`};`  
`#pragma pack(pop)`

**Bonus Benefit of .GRP Parsing:** Windows 3.1 caches the rendered icon bitmap (pANDPlane and pXORPlane) *directly inside the .GRP file*, meaning the Kiosk can extract icons even for DOS programs that were assigned an icon via MORICONS.DLL in Program Manager\!

#### **3.2 Windows 95/98 Shell Link (.LNK) Binary Parser**

For Windows 95/98 machines, the crawler walks \\WINDOWS\\Start Menu\\Programs\\ recursively and parses .LNK files using the \[MS-SHLLINK\] binary specification:

> 1. Verifies the 4-byte HeaderSize (0x0000004C) and LinkCLSID (00021401-0000-0000-C000-000000000046).  
> 2. Reads the LinkFlags bitmask (offset 0x14):  
   * If HasLinkTargetIDList (bit 0\) is set, skips the IDListSize (uint16\_t at offset 0x4C).  
   * Reads the LinkInfo structure to extract the **LocalBasePath** string (e.g., C:\\Program Files\\LucasArts\\Grim\\GRIM.EXE).  
   * Reads WorkingDir and CommandLineArguments string blocks.  
   * If HasIconLocation (bit 6\) is set, extracts the custom .ICO/.DLL path and IconIndex (offset 0x38) to pass to the NE/PE Icon Extractor.

### **4\. DOS Game Fingerprinting Engine**

Because Real-Mode (MZ) and Protected-Mode (LE/LX) DOS executables do not contain a standardized resource table or metadata header, the crawler uses a **Two-Tier Identification Strategy**.

                   `+-----------------------------------------+`  
                   `|   DOS Executable (.EXE / .COM / .BAT)   |`  
                   `+-----------------------------------------+`  
                                        `|`  
         `+------------------------------+------------------------------+`  
         `| (Contains "DOS/4GW" or "Stub" in first 64KB?)               | (Standard Binary)`  
         `v                                                             v`  
`+---------------------------------------+            +---------------------------------------+`  
`|  1. Strip Extender Stub               |            |  1. Compute Fast Header Hash          |`  
`|  - Seek past MZ relocation table to   |            |  - CRC32 of [FileSize + First 4096B]  |`  
`|    locate embedded "LE" / "LX" / "BW" |            +---------------------------------------+`  
`|    Linear Executable header offset.   |                              |`  
`|  - Compute CRC32 of first 4096 bytes  |                              |`  
`|    starting at the LE/LX signature.   |                              |`  
`+---------------------------------------+                              |`  
         `|                                                             |`  
         `+------------------------------+------------------------------+`  
                                        `|`  
                                        `v`  
                  `+-------------------------------------------+`  
                  ``|  TIER 1: Query `software_signatures` DB   |``  
                  `|  SELECT * WHERE exec_filename = ?         |`  
                  `|             AND header_crc32 = ?          |`  
                  `+-------------------------------------------+`  
                                        `|`  
                  `+---------------------+---------------------+`  
                  `| (Match Found!)                            | (Unknown Binary / Mod)`  
                  `v                                           v`  
     `+--------------------------+               +-------------------------------------+`  
     `| Populate Title, Year,    |               | TIER 2: Heuristic Fallback Scraper  |`  
     `| ItemType (GAME vs APP),  |               | - Inspect VERSIONINFO / DESCRIPTION |`  
     `| & Pre-bundled Artwork    |               | - Scan ASCII strings for Copyright  |`  
     `+--------------------------+               | - Extract companion .ICO / .BMP     |`  
                                                `+-------------------------------------+`

#### **Why Strip the Extender Stub Before Hashing?**

Hundreds of mid-1990s DOS games (*Doom*, *Duke Nukem 3D*, *Descent*, *Warcraft II*) bind the exact same 26KB DOS/4GW stub (wstub.exe) to the front of their executable. Hashing the first 4KB of the file would produce the exact same CRC32 for all of them.

> * **The Solution:** image\_crawler.cpp checks the MZ header offset 0x3C (or calculates the end of the MZ image via (e\_cp \* 512\) \- (512 \- e\_cblp)). If an LE, LX, or BW (Watcom) header exists at that boundary, the 4KB CRC32 is computed starting from the **Linear Executable header**, uniquely fingerprinting the game code itself while remaining fast enough to scan 50 executables in under 20 milliseconds.

### **5\. NE (16-Bit) & PE (32-Bit) Icon Extraction Engine**

To populate the Kiosk Carousel with crisp original icons from Windows 3.1 (NE New Executable) and Windows 95/98 (PE Portable Executable) binaries without external dependencies, image\_crawler.cpp parses the executable headers directly from the RO-VFS stream and converts the best icon into a 32-bit RGBA PNG.

#### **5.1 Executable Format Dispatch**

Every Windows binary starts with a DOS MZ header (0x5A4D). The crawler reads the 32-bit integer e\_lfanew at offset 0x3C and seeks to that offset:

> * If the 2-byte signature at e\_lfanew is **0x454E ("NE")** → Invoke **16-Bit NE Resource Parser**.  
> * If the 4-byte signature at e\_lfanew is **0x00004550 ("PE\\0\\0")** → Invoke **32-Bit PE Resource Parser**.

#### **5.2 Parsing RT\_GROUP\_ICON (0x000E) & RT\_ICON (0x0003)**

In both NE and PE executables, icons are stored in two linked resource types:

> 1. **RT\_GROUP\_ICON (Resource Type 14):** Contains the directory header (GRPICONDIR) listing all available resolutions and color depths (e.g., 32x32 16-color, 32x32 256-color, 48x48 TrueColor), along with a 16-bit **nID ordinal** for each variant.  
> 2. **RT\_ICON (Resource Type 3):** Contains the raw Device-Independent Bitmap (BITMAPINFOHEADER \+ Palette \+ XOR Pixel Data \+ 1-bit AND Transparency Mask) corresponding to that nID.

`#pragma pack(push, 1)`  
`struct GrpIconDir {`  
    `uint16_t idReserved;   // Always 0`  
    `uint16_t idType;       // 1 for Icons`  
    `uint16_t idCount;      // Number of icon variants in this group`  
`};`

`struct GrpIconDirEntry {`  
    `uint8_t  bWidth;       // 32, 48, etc. (0 means 256)`  
    `uint8_t  bHeight;      // 32, 48, etc.`  
    `uint8_t  bColorCount;  // 0 if >= 8bpp, 16 for 4bpp, 2 for 1bpp`  
    `uint8_t  bReserved;`  
    `uint16_t wPlanes;      // Color planes`  
    `uint16_t wBitCount;    // 4, 8, 24, or 32 bpp`  
    `uint32_t dwBytesInRes; // Size of the corresponding RT_ICON payload`  
    `uint16_t nID;          // Resource Ordinal ID in the RT_ICON table!`  
`};`  
`#pragma pack(pop)`

#### **5.3 Selection & DIB-to-RGBA Conversion Algorithm**

> 1. **Best Variant Selection:** The parser iterates through all GrpIconDirEntry structs in the first RT\_GROUP\_ICON (or the index specified by the .LNK file) and selects the entry maximizing:  
>    Score=(Width×Height)+(BitCount×100)  
>    This automatically prefers a 32x32 256-color (8-bit) Win95 icon over a 32x32 16-color (4-bit) fallback icon stored in the same binary.  
> 2. **Locating the RT\_ICON Payload:**  
   * **In NE (16-Bit) files:** Seeks to e\_lfanew \+ ne\_rsrctab, reads the alignment shift count (uint16\_t rscAlignShift), scans the TNAMEINFO array under type 0x8003 (RT\_ICON) for rnID \== (0x8000 \| nID), and reads (rnOffset \<\< rscAlignShift) bytes.  
   * **In PE (32-Bit) files:** Locates Data Directory 2 (IMAGE\_DIRECTORY\_ENTRY\_RESOURCE), traverses the 3-level tree (Type \= 3 → Name \= nID → Language), and converts the OffsetToData Relative Virtual Address (RVA) to a physical file offset using the .rsrc section header's VirtualAddress and PointerToRawData.  
> 3. **XOR \+ AND Mask Compositing to 32-Bit RGBA PNG:**  
   * Note that in Windows RT\_ICON bitmaps, biHeight in the BITMAPINFOHEADER is **doubled** (e.g., 64 for a 32x32 icon) because it encompasses both the bottom-up **XOR color bitmap** and the **1-bit monochrome AND mask** stacked vertically.  
   * For every pixel (*x*,*y*) (iterating bottom-to-top to flip the Windows DIB orientation):  
     * Look up the RGB color from the color table (for 4-bit/8-bit icons) or direct BGR bytes (for 24/32-bit).  
     * Check bit (*x*,*y*) in the 1-bit AND mask: if the AND bit is 1, set Alpha \= 0x00 (transparent); if 0, set Alpha \= 0xFF (opaque).  
   * Writes the resulting 32×32×4 byte buffer via lodepng or stb\_image\_write to /var/lib/dosbox-kiosk/cache/icons/\<item\_uuid\>.png.

#### **5.4 Fallback Icon Generation for Pure DOS Games**

If a pure DOS game has no .ICO file in its directory and no pre-bundled box art in the Kiosk library:

> * The scanner checks if the directory contains a .PCX, .LBM, or .BMP splash screen; if found, it generates a thumbnail.  
> * Otherwise, the Kiosk UI renders a **Procedural MS-DOS Application Badge** (stylized EXE CRT terminal icon or a 3.5" floppy badge color-coded by Hardware Archetype) with the game's title rendered in an authentic IBM VGA 8x16 bitmap font.

---

*User prompt: Yes, design the GOG and Steam Archive Ingestion Engine, including how it unpacks installers, converts .gog/.inst images, and translates bundled dosbox.conf files into Kiosk Hardware Archetypes.*

### **Response: 1\. GOG & Steam Ingestion Architecture (src/kiosk/ingest/archive\_importer.cpp)**

GOG offline installers (setup\_\*.exe \+ .bin) and Steam game backups (.zip archives of steamapps/common/\<Game\>) wrap vintage DOS and Windows 9x games inside modern compatibility scaffolding: outdated DOSBox 0.74 binaries, 32-bit SDL.dll files, proprietary .gog/.inst optical images, and split .conf files (dosbox\_\<game\>.conf \+ dosbox\_\<game\>\_single.conf).

The **Ingestion Engine** strips away this wrapper layer, extracts the raw game payload and optical/floppy images, translates the vendor's .conf files into the Kiosk's native **Hardware Archetypes** and **Session Caddies**, and deploys the game into either an instant-boot **Appliance Container** or directly into a target Machine's **hdd\_c.img**.

`+-------------------------------------------------------------------------------+`  
`|                     GOG & STEAM INGESTION PIPELINE                            |`  
`|                                                                               |`  
`|  [USB / Staging Drop] ---> [Format Sniffer]                                   |`  
`|                              ├── InnoSetup ("Inno Setup Setup Data")          |`  
`|                              │     └── libinnoextract / innoextract CLI       |`  
`|                              └── PKZIP ("PK\x03\x04")                         |`  
`|                                    └── libzip / miniz stream extractor        |`  
`|                                                                               |`  
`|  [Staging Sandbox (/var/lib/dosbox-kiosk/ingest_staging/<job_uuid>/)]         |`  
`|        |                                                                      |`  
`|        +---> [1. Wrapper & Cruft Sanitizer]                                   |`  
`|        |       - Deletes bundled DOSBox.exe, SDL.dll, nglide, ddraw wrappers  |`  
`|        |       - Extracts goggame-*.info JSON metadata & high-res icons       |`  
`|        |                                                                      |`  
`|        +---> [2. Optical & Floppy Media Normalizer]                           |`  
`|        |       - Renames .gog -> .iso/.bin and .inst/.ins -> .cue             |`  
`|        |       - Rewrites internal CUE FILE paths to relative Linux basenames |`  
`|        |       - Moves images to /media_pool/ & registers in catalog.db       |`  
`|        |                                                                      |`  
`|        +---> [3. Heuristic .conf AST Parser]                                  |`  
`|        |       - Merges base .conf + _single.conf                             |`  
`|        |       - Translates [autoexec] mount/imgmount -> caddy_bindings       |`  
`|        |       - Scores [cpu]/[sblaster]/[glide] -> Maps to Archetype         |`  
`|        |                                                                      |`  
`|        +---> [4. Dual-Mode Target Deployment]                                 |`  
`|                ├── Mode A: Appliance Container (Instant Integrated Boot)      |`  
`|                └── Mode B: FAT16/FAT32 Image Writer (Injects into hdd_c.img)  |`  
`+-------------------------------------------------------------------------------+`

### **2\. Stage 1: Archive Unpacking & Metadata Extraction**

When the user selects **"Import GOG / Steam Games"** from the Kiosk menu, the engine scans /var/lib/dosbox-kiosk/ingest\_staging/ and any mounted USB drives (/run/media/kiosk/\<UUID\>/).

#### **2.1 Format Detection & Unpacking**

> 1. **GOG Offline Installers (setup\_\*.exe \+ setup\_\*-1.bin):**  
   * The engine inspects the PE binary overlay or invokes innoextract \--info \<file.exe\> to verify an Inno Setup header (Inno Setup Setup Data (5.x / 6.x)).  
   * It executes innoextract \--extract \--lowercase \--gog \--output-dir \<sandbox\_dir\> \<file.exe\>, which automatically spans multi-part .bin slices (setup\_\*-1.bin, setup\_\*-2.bin) without running any x86 Windows installer code or Wine.  
> 2. **Steam .zip Archives:**  
   * Extracted directly via miniz or libarchive into \<sandbox\_dir\>, normalizing backslashes (\\) in archive paths and collapsing redundant top-level folders (e.g., SteamApps/common/Master of Orion 2/ → \<sandbox\_dir\>/).

#### **2.2 Harvesting GOG & Steam Metadata**

Before deleting wrapper files, the importer scrapes bundled metadata to enrich catalog\_items:

> * **GOG Metadata (goggame-\<id\>.info & goggame-\<id\>.ico):**  
  * Parses the JSON structure inside goggame-\<id\>.info to extract the canonical name, releaseDate, developer, and playTasks (which explicitly list the primary game launch task vs. secondary setup/manual tasks).  
  * Extracts the largest PNG/BMP layer from goggame-\<id\>.ico and caches it to /var/lib/dosbox-kiosk/cache/icons/\<item\_uuid\>.png.  
> * **Steam Metadata (steam\_appid.txt / .vdf):**  
  * Reads steam\_appid.txt (if present) or matches the folder name against the offline signature table in catalog.db.

#### **2.3 Wrapper & Cruft Sanitization**

Once metadata and .conf files are copied to memory, the sanitizer purges host/wrapper binaries so they never pollute the target DOS drive or trigger false positives in the scanner:

> * **Deleted Directories:** DOSBOX/, \_\_support/, commonredist/, DirectX/, webcache.zip.  
> * **Deleted Root Binaries:** dosbox.exe, SDL.dll, SDL\_net.dll, libogg\*.dll, libvorbis\*.dll, nglide\*.dll, 3dfxSpl\*.dll, ddraw.dll, unins000.exe, unins000.dat, galaxy.dll.

### **3\. Stage 2: Optical & Floppy Media Normalizer (.gog / .inst Conversion)**

GOG frequently disguises standard ISO9660 and Mixed-Mode CUE/BIN CD-ROM images using custom extensions (.gog, .inst, .ins, .dat) to prevent Windows Explorer from associating them with burning software. Furthermore, their CUE sheets often contain hardcoded Windows relative paths (FILE "..\\game.gog" BINARY) or uppercase/lowercase mismatches that fail on a case-sensitive Linux filesystem (ext4/btrfs).

#### **3.1 Identification & Renaming Matrix**

| Source Pattern in Archive | Inspection Rule | Normalized Output in /media\_pool/ |
| :---- | :---- | :---- |
| **\<name\>.gog** (standalone, no .inst/.ins) | Check offset 0x8000 for \\x01CD001 (2048-byte sector ISO) | \<item\_slug\>\_cd1.iso (CDROM\_ISO) |
| **\<name\>.inst or \<name\>.ins \+ \<name\>.gog** | Text file starting with FILE "..." BINARY and TRACK 01 | \<item\_slug\>\_cd1.cue \+ \<item\_slug\>\_cd1.bin (CDROM\_CUE) |
| **\<name\>.cue \+ \<name\>.bin \+ \*.ogg/\*.mp3** | Mixed-mode CD with compressed Redbook audio tracks | \<item\_slug\>\_cd1.cue \+ .bin \+ audio files in /media\_pool/\<item\_slug\>/ |
| **\*.img / \*.ima / \*.vfd** | Size matches 360K, 720K, 1.2M, 1.44M | \<item\_slug\>\_disk\<N\>.img (FLOPPY\_\*) |

#### **3.2 CUE Sheet Path Sanitizer (SanitizeCueSheet())**

When moving a .inst/.cue and .gog/.bin pair into /var/lib/dosbox-kiosk/media\_pool/, the engine parses and rewrites every FILE directive inside the CUE sheet:

`BEFORE (Bundled GOG game.inst):`  
`FILE "..\GAME.GOG" BINARY`  
  `TRACK 01 MODE1/2352`  
    `INDEX 01 00:00:00`  
`FILE "..\MUSIC\TRACK02.OGG" MP3`  
  `TRACK 02 AUDIO`  
    `INDEX 01 00:00:00`

`AFTER (Normalized /var/lib/dosbox-kiosk/media_pool/homm2_cd1.cue):`  
`FILE "homm2_cd1.bin" BINARY`  
  `TRACK 01 MODE1/2352`  
    `INDEX 01 00:00:00`  
`FILE "homm2_audio/track02.ogg" MP3`  
  `TRACK 02 AUDIO`  
    `INDEX 01 00:00:00`

> * It verifies that every referenced .bin, .ogg, .mp3, or .wav file exists on disk with exact case matching, sets has\_redbook\_audio \= 1 in media\_pool if TRACK 02 AUDIO is present, and extracts the ISO9660 Primary Volume Descriptor (PVD) string at 0x8028 to populate volume\_label for the skeuomorphic CD jewel case renderer.

### **4\. Stage 3: Heuristic dosbox.conf Translator**

GOG and Steam games usually split their configuration across two files:

> 1. **dosbox\_\<game\>.conf:** Contains \[cpu\], \[sblaster\], \[dosbox\], and \[render\] settings.  
> 2. **dosbox\_\<game\>\_single.conf:** Contains the \[autoexec\] block with mount, imgmount, directory changes, executable invocation, and exit.

The importer merges all .conf files in the archive (ignoring \*\_settings.conf or \*\_server.conf secondary profiles, or registering them as optional launch modes) and runs a two-part translation:

#### **4.1 Hardware Archetype Scoring Algorithm**

Rather than preserving dozens of messy, inconsistent .conf files from 2010-era DOSBox 0.74, the translator maps the game's hardware demands to one of the five canonical **Kiosk Hardware Archetypes** (storing only game-specific quirks in machines.custom\_conf\_overrides):

`+-------------------------------------------------------------------------+`  
`|                    CONF-TO-ARCHETYPE DECISION TREE                      |`  
`+-------------------------------------------------------------------------+`  
`| 1. Does archive contain Win95/98 PE binaries (no DOS stub) OR           |`  
``|    does .conf specify `glide=true` / `voodoo=true` / `memsize >= 32`?   |``  
``|       └── YES ===> Assign `ARCH_PENTIUM_3DFX`                           |``  
`|                                                                         |`  
``| 2. Does `[cpu]` specify `cycles=max` OR `cycles > 15000` OR does the    |``  
``|    payload contain `DOS4GW.EXE`, `DOS32A.EXE`, `CWSDPMI.EXE`, or `VBE`? |``  
``|       └── YES ===> Assign `ARCH_486DX2`                                 |``  
``|                    (If `cycles > 40000`, add override: `cycles=max`)    |``  
`|                                                                         |`  
``| 3. Does `[cpu]` specify `cycles` between `3000` and `15000` OR          |``  
``|    `machine=svga*` / `sbtype=sbpro*` / `ems=true`?                      |``  
``|       └── YES ===> Assign `ARCH_386DX`                                  |``  
`|                                                                         |`  
``| 4. Does `[dosbox]` specify `machine=ega` OR `cycles` between            |``  
``|    `600` and `2999`?                                                    |``  
``|       └── YES ===> Assign `ARCH_AT_286`                                 |``  
`|                                                                         |`  
``| 5. Does `[dosbox]` specify `machine=cga`, `hercules`, `tandy`, `pcjr`   |``  
``|    OR `cycles < 600`?                                                   |``  
``|       └── YES ===> Assign `ARCH_XT_8088`                                |``  
`+-------------------------------------------------------------------------+`

#### **4.2 \[autoexec\] AST Parser & Script Decompiler**

A typical GOG \[autoexec\] section looks like this:

`[autoexec]`  
`# Lines in this section will be run at startup.`  
`@echo off`  
`mount C ".."`  
`mount C "..\cloud_saves" -t overlay`  
`imgmount d "..\game.inst" -t iso -fs iso`  
`c:`  
`cd tomb`  
`dos4gw.exe tomb.exe -nojoy`  
`exit`

The Kiosk \[autoexec\] parser tokenizes this script line-by-line and translates it into structured database records:

> 1. **mount C "..." & \-t overlay:**  
   * Resolves the relative host path ("..") against the location of the .conf file to identify the true **Game Root Directory** inside the staging sandbox.  
   * If an overlay mount (cloud\_saves) is present, merges its contents directly into the Game Root Directory so existing configuration files are preserved.  
> 2. **imgmount d "..." / imgmount a "...":**  
   * Extracts all quoted file paths (supporting multi-CD games like imgmount d "cd1.inst" "cd2.inst" "cd3.inst" \-t iso).  
   * Matches each path to the normalized entry in media\_pool and inserts ordered rows (slot\_order \= 1, 2, 3...) into **caddy\_bindings** for target\_drive \= 'D' (or 'A').  
   * This automatically populates the **Skeuomorphic Runtime Disk Caddy (F12+D)** for multi-CD GOG games (*Wing Commander III/IV*, *Phantasmagoria*, *Gabriel Knight 2*) without the user manually configuring a single disk\!  
> 3. **Working Directory & Executable Extraction:**  
   * Tracks simulated CD / CHDIR commands to compute guest\_working\_dir (e.g., \\TOMB).  
   * When it encounters an executable invocation:  
     * If the command starts with dos4gw.exe \<target.exe\> \<args\> or loadfix \-64 \<target.exe\> \<args\>, it preserves the prefix or sets guest\_executable and guest\_arguments accordingly.  
     * Ignores trailing exit, cls, pause, or @echo commands.

### **5\. Stage 4: Dual-Mode Target Deployment**

Once the archive is unpacked, media normalized, and \[autoexec\] translated, the Kiosk UI asks the user (or uses a global default) how to store the imported game:

#### **Mode A: Appliance Container Mode (Default for DOS Imports)**

> * Moves the cleaned Game Root Directory to /var/lib/dosbox-kiosk/machines/containers/\<item\_uuid\>/.  
> * Binds the item in catalog\_items with launch\_mechanism \= 'INTEGRATED\_AUTOEXEC' and associates it with a virtual machine template matching the detected Hardware Archetype (boot\_mode \= 'INTEGRATED\_DOS').  
> * **Advantage:** Zero hard-disk image space limits, sub-second boot straight into the game from the Carousel, and complete filesystem isolation between games.

#### **Mode B: Guest OS Hard Disk Injection (REAL\_HDD\_BOOT Machines)**

When importing a Windows 95/98 game (or when the user chooses to install a DOS game onto a specific custom machine like "My 1993 486DX2 Rig" running real MS-DOS 6.22):

> 1. **Free Space Verification:** The engine inspects the target machine's hdd\_c.img via DOSBox-X's internal FAT16/FAT32 driver to verify sufficient free clusters exist (with a 15% safety margin for swap/save files).  
> 2. **8.3 / VFAT Directory Copy:**  
   * Copies the cleaned Game Root Directory into C:\\GAMES\\\<8\_3\_SLUG\>\\ inside hdd\_c.img.  
   * If the target machine runs MS-DOS 6.22 (FAT16 without VFAT), filenames are validated against 8.3 constraints. If the target runs Windows 95/98 (FAT32), VFAT Long Filename directory entries are generated.  
> 3. **Config Path Rewriting:**  
   * Many DOS/Win9x games store an .INI or .CFG file (RESOURCE.CFG, SETUP.INI, FALLOUT.CFG) containing the install path (which GOG defaults to C:\\ because GOG mounts the game folder itself as C:\\).  
   * When injecting the game into a subdirectory (C:\\GAMES\\SIERRA\\KQ6\\) on a shared hdd\_c.img, the **Config Patcher** scans text/INI files \<64KB in the game folder for C:\\ path references (e.g., patchDir=C:\\, cdrom=D:\\) and updates C:\\ to C:\\GAMES\\\<SLUG\>\\ so the game finds its assets inside the full hard disk image.  
> 4. **Catalog & Caddy Registration:**  
   * Inserts the new catalog\_items row linked to machine\_id, sets launch\_mechanism \= 'DOS\_AUTOEXEC\_INJECT' (or 'WIN9X\_RUNONCE\_HOOK'), and binds all extracted CD/floppy images to caddy\_bindings.

---

*User prompt: Yes, design the UI architecture for the Pre-Boot Kiosk Shell (30s watchdog, Carousels, Machine Wizard) and the Skeuomorphic Runtime Overlay (F12+D disk swapper) inside DOSBox-X.*

### **Response: 1\. Rendering Pipeline Integration (src/gui/sdlmain.cpp & src/kiosk/ui/)**

DOSBox-X already initializes an **SDL2 \+ OpenGL 3.3 Core** (or OpenGL ES 2.0/3.0 for KMSDRM embedded targets) context to render CRT shaders (glshader) and its legacy top-bar menu. Rather than bolting on a heavy web engine or external toolkit, the Kiosk UI integrates **Dear ImGui** (with custom OpenGL procedural shaders and FreeType bitmap/vector font rasterization) directly into DOSBox-X’s frame presenter.

`+---------------------------------------------------------------------------------+`  
`|                   DOSBOX-X UNIFIED SDL2 / OPENGL FRAME PIPELINE                 |`  
`|                                                                                 |`  
`|  +---------------------------------------------------------------------------+  |`  
``|  | PASS 1: Guest Framebuffer Texture (`GFX_EndUpdate`)                       |  |``  
``|  |  - Active during `STATE_EMULATION_RUNNING` & `STATE_RUNTIME_OVERLAY`      |  |``  
``|  |  - Renders VGA/SVGA/3dfx output through CRT shader (`crt-aperture.glsl`)  |  |``  
``|  |  - When in `STATE_RUNTIME_OVERLAY`, applies Gaussian Blur + 60% Dimming   |  |``  
`|  +---------------------------------------------------------------------------+  |`  
`|                                        |                                        |`  
`|                                        v                                        |`  
`|  +---------------------------------------------------------------------------+  |`  
``|  | PASS 2: Kiosk Compositor Layer (`KioskUI::RenderFrame`)                   |  |``  
``|  |  - Active during `STATE_COUNTDOWN_IDLE`, `STATE_SHELL_INTERACTIVE`,       |  |``  
``|  |    and `STATE_RUNTIME_OVERLAY` (Skipped entirely during normal gameplay)  |  |``  
`|  |  - Draws vector/bitmap UI, 60fps spring-physics carousels, and            |  |`  
`|  |    procedural 9-slice skeuomorphic Floppy/CD textures                     |  |`  
`|  +---------------------------------------------------------------------------+  |`  
`|                                        |                                        |`  
`|                                        v                                        |`  
``|                        `SDL_GL_SwapWindow()` (VSync 60Hz)                       |``  
`+---------------------------------------------------------------------------------+`

#### **Unified Input Abstraction (KioskInputRouter)**

Because a kiosk may be operated via a vintage PS/2 keyboard, a mouse, an arcade cabinet joystick, or a couch gamepad, src/kiosk/ui/input\_router.cpp maps all physical inputs into virtual Kiosk navigation events before they ever reach the emulated PIC/8042 keyboard controller:

| Virtual Kiosk Action | Keyboard Mapping | Gamepad / Arcade Mapping | Mouse Mapping |
| :---- | :---- | :---- | :---- |
| **NAV\_LEFT / NAV\_RIGHT** | Left / Right Arrows | D-Pad Left/Right or Left Stick | Hover / Scroll Wheel |
| **NAV\_UP / NAV\_DOWN** | Up / Down Arrows | D-Pad Up/Down or Left Stick | Hover / Scroll Wheel |
| **TAB\_PREV / TAB\_NEXT** | Q / E or PgUp / PgDn | LB / RB Shoulder Bumpers | Click Top Tab Bar |
| **ACTION\_CONFIRM** | Enter or Space | A (Xbox) / Cross (PS) | Left Click |
| **ACTION\_BACK\_CANCEL** | Esc or Backspace | B (Xbox) / Circle (PS) | Right Click |
| **ACTION\_CADDY\_QUEUE** | Insert or Tab | X (Xbox) / Square (PS) | Middle Click / Drag |
| **ACTION\_VIEW\_TOGGLE** | V or F2 | Y (Xbox) / Triangle (PS) | Click View Toggle Pill |
| **HOTKEY\_OVERLAY** | F12+D (or F11+O alias) | Guide / Home Button | Top-Right Corner Hotzone |

### **2\. Pre-Boot Kiosk Shell Architecture**

#### **2.1 The 30-Second Watchdog Banner (STATE\_COUNTDOWN\_IDLE)**

When the system boots, it opens directly to the primary Kiosk Shell with a non-blocking **Autoboot Watchdog Bar** docked along the bottom edge of the screen.

`+---------------------------------------------------------------------------------+`  
`| [GAMES (24)]   [APPLICATIONS (9)]   [MACHINES (4)]   [MEDIA POOL]   [SYSTEM]    |`  
`+---------------------------------------------------------------------------------+`  
`|                                                                                 |`  
`|      .----------------.     .========================.     .----------------.   |`  
`|      |                |     ||                      ||     |                |   |`  
`|      |   SIMCITY      |     ||       DOOM II        ||     |   TIE FIGHTER  |   |`  
`|      |     2000       |     ||   Hell on Earth      ||     |   Collector's  |   |`  
`|      |                |     ||                      ||     |     CD-ROM     |   |`  
`|      |  [486DX2-66]   |     ||  [486DX2-66 • SB16]  ||     |  [486DX2-66]   |   |`  
`|      '----------------'     '========================'     '----------------'   |`  
`|                                                                                 |`  
`|                         Target Machine: 1994 486DX2 VLB Rig                     |`  
`|                         Attached Media: DOOM2_CD.ISO (Drive D:)                 |`  
`|                                                                                 |`  
`+---------------------------------------------------------------------------------+`  
`| [!] AUTO-STARTING DEFAULT TARGET: "1994 486DX2 VLB Rig (MS-DOS 6.22)" IN 23s... |`  
`| [===================>                                ] (Press ANY KEY to Pause) |`  
`+---------------------------------------------------------------------------------+`

> * **Watchdog Cancellation Logic:**  
>   `void KioskUI::UpdateWatchdog(float delta_time, const SDL_Event& ev) {`  
>       `if (state != KioskState::STATE_COUNTDOWN_IDLE) return;`

>       `// Ignore minor analog stick drift (< 25% deadzone) or small mouse jitter (< 5px)`  
>       `if (IsIntentionalUserInput(ev)) {`  
>           `state = KioskState::STATE_SHELL_INTERACTIVE;`  
>           `PlayUISound(SFX_WATCHDOG_CANCEL); // Subtle mechanical relay click`  
>           `return;`  
>       `}`

>       `watchdog_remaining_sec -= delta_time;`  
>       `if (watchdog_remaining_sec <= 0.0f) {`  
>           `TriggerDefaultBootAction(); // Transitions to STATE_SYNTHESIZE_LAUNCH`  
>       `}`  
>   `}`

#### **2.2 The Big-Picture Carousels (view\_carousel.cpp)**

The top navigation bar switches between five primary views using LB/RB or Q/E:

> 1. **\[GAMES\] & \[APPLICATIONS\] Carousels:**  
   * **Visual Layout:** Horizontal cover-flow ribbon centered vertically, with a 2-row filterable grid below for libraries larger than 15 items.  
   * **Card Anatomy:** Each card displays the extracted NE/PE icon or box art, an **Archetype Badge** in the bottom-left corner (\[XT\], \[286\], \[386\], \[486\], \[P5-3DFX\]), an **OS Pill** (DOS 6.22, Win95, Container), and a **Media Indicator** showing how many floppies or CDs are pre-bound to its Session Caddy.  
   * **Context Drawer (X / Tab on a highlighted card):** Opens a quick-action blade allowing the user to:  
     * *Launch Normally* (Boots straight into the executable).  
     * *Customize Session Caddy* (Pre-load extra save disks or expansion CDs before booting).  
     * *Run Setup / Sound Configuration* (Automatically scans the target folder for SETUP.EXE / SETSOUND.EXE and boots into that instead).  
> 2. **\[MACHINES\] Deck:**  
   * Displays each user-created virtual computer as an illustrated period-accurate tower or desktop case (e.g., IBM 5160 XT desktop case, beige 486 tower with a green LED turbo display showing 66, or a late-90s Pentium MMX ATX tower).  
   * **Two Launch Buttons per Machine:**  
     * **\[Boot to OS Prompt / Desktop\]:** Boots hdd\_c.img cleanly with no D:\\KIOSKGO.BAT payload so the user lands at C:\\\> or the Windows 95/98 Start Screen.  
     * **\[Pre-Load Disks & Boot\]:** Opens the Media Pool picker first so the user can load boot floppies or utility CDs into Drive A:, B:, or D: before turning on the machine.  
> 3. **\[MEDIA POOL & USB\] Manager:**  
   * Live view of /var/lib/dosbox-kiosk/media\_pool/ alongside a hot-plugged tab for any USB drive mounted via udisks2 (/run/media/kiosk/\<UUID\>).  
   * Users can press Insert / X on multiple floppies or CDs to build a **Custom Session Stack** on the fly.

#### **2.3 Machine Creation & OS Provisioning Wizard (view\_wizard.cpp)**

Designed to be completed in under 15 seconds with a gamepad or arrow keys, the wizard uses a **4-Step Breadcrumb Card Flow**:

`+---------------------------------------------------------------------------------+`  
`|  NEW MACHINE WIZARD                                     Step 2 of 4: Select OS  |`  
`+---------------------------------------------------------------------------------+`  
`|  Selected Hardware: [ARCH_386DX] 386DX-33 • 8MB RAM • Tseng ET4000 • SB Pro 2   |`  
`+---------------------------------------------------------------------------------+`  
`|                                                                                 |`  
`|   .-----------------------.   .=======================.   .-------------------. |`  
`|   |      MS-DOS 5.0       |   ||     MS-DOS 6.22     ||   |   Windows 95      | |`  
`|   |   (3x 1.44MB Disks)   |   ||  + Win 3.11 (Opt)   ||   |     OSR 2.5       | |`  
`|   |                       |   ||  (3x 1.44MB Disks)  ||   |   (Bootable CD)   | |`  
`|   |  [COMPATIBLE - TIER 3]|   || [RECOMMENDED TIER 3]||   | [REQUIRES 486/P5] | |`  
`|   '-----------------------'   '======================='   '-------------------' |`  
`|                                                                (Locked/Dimmed)  |`  
`|                                                                                 |`  
`|  Provisioning Mode:                                                             |`  
`|   (•) Express Pre-Partitioned Setup (Auto-creates 250MB FAT16 C: & boots Disk 1)|`  
`|   ( ) Authentic Raw Setup (Creates unformatted 250MB HDD for manual FDISK)      |`  
`|                                                                                 |`  
`|                        [ < Back ]          [ Create & Start OS Install (Enter) ]|`  
`+---------------------------------------------------------------------------------+`

> * **Tier Compatibility Enforcement:** The wizard queries os\_distributions.min\_archetype\_tier \<= selected\_archetype.tier. Incompatible OS choices (like Windows 98 on a 286 or 386\) are visually dimmed with a clear hardware badge (\[REQUIRES 486/P5\]) so kiosk users cannot accidentally create broken configurations.

### **3\. Skeuomorphic Procedural Media Renderer (view\_disk\_caddy.cpp)**

To render hundreds of distinct 5.25" floppies, 3.5" microfloppies, and CD-ROM jewel cases at 4K resolution without storing megabytes of pre-rendered PNGs, the Kiosk UI uses **Multi-Layer Composited Templates \+ Dynamic Font Rasterization**:

   `LAYER 4 (Dynamic Text)  --->  "MONKEY ISLAND 2" (Rendered via Dot-Matrix or`  
                                 `"Disk 2 of 5"      Sharpie Marker TTF onto Label UV)`  
                                        `+`  
   `LAYER 3 (Sticker Decal) --->  9-Slice Paper Label Texture (Aged Cream / Striped)`  
                                        `+`  
   `LAYER 2 (Hardware Bits) --->  Metallic Shutter (3.5"), Index Hole (5.25"), or`  
                                 `Iridescent Data Ring + Audio Note Badge (CD-ROM)`  
                                        `+`  
   `LAYER 1 (Base Shell)    --->  Tinted Base SVG/PNG Mesh (Charcoal, Beige, Blue)`

> * **Deterministic Visual Variety:** If a disk in media\_pool does not have an explicit physical\_style set, the renderer hashes media\_pool.id (CRC32 % 3\) to deterministically pick between classic **IBM Beige**, **Verbatim Cobalt Blue**, or **Maxell Matte Black** shells, and applies a tiny rotation jitter (±0.8∘) to the paper sticker so a shelf of 10 floppies looks organically labeled rather than copy-pasted.

### **4\. Runtime In-Process Overlay (STATE\_RUNTIME\_OVERLAY)**

When the user presses F12+D (or the controller Guide button) while a guest OS is running, DOSBox-X intercepts the event inside src/gui/sdlmain.cpp before passing keystrokes to the guest BIOS.

#### **4.1 Emulation Freeze & Audio DMA Safety Protocol**

Simply stopping the main loop while Sound Blaster DMA or Redbook CD-Audio is playing will cause a harsh buzzing ring-buffer loop on the host Linux audio server. Entering and exiting STATE\_RUNTIME\_OVERLAY executes a deterministic 4-step freeze/resume sequence:

`void KioskManager::EnterRuntimeOverlay() {`  
    `if (state != KioskState::STATE_EMULATION_RUNNING) return;`

    `// 1. Snapshot current guest VGA/SVGA/3dfx frame into an OpenGL FBO texture`  
    `CaptureGuestFramebufferToTexture(&bg_blur_fbo);`

    `// 2. Silence audio mixer and pause Redbook CD-DA playback threads`  
    `MIXER_LockAudioDevice();`  
    `MIXER_Enable(false);`  
    `// 3. Save relative mouse state (e.g., if locked inside Doom/Quake or Win98)`  
    `was_mouse_locked = mouselocked;`  
    `if (mouselocked) GFX_CaptureMouse(); // Releases cursor for Kiosk UI navigation`

    `// 4. Switch state so main loop skips CPU_Core_Dyn_X86_Run() / PIC_RunQueue()`  
    `state = KioskState::STATE_RUNTIME_OVERLAY;`  
`}`

#### **4.2 Dual-Mode Runtime Overlay Layout (Skeuomorphic vs. Compact List)**

The overlay renders over the blurred, dimmed guest OS screen and can be toggled between **Visual Shelf Mode** and **Fast List Mode** at any time by pressing V or Y:

`+=================================================================================+`  
`|  DOSBOX-X KIOSK OVERLAY (Emulation Paused)         [V/Y: Toggle Visual / List]  |`  
`+=================================================================================+`  
`|  ACTIVE DRIVE SLOTS:                                                            |`  
`|  +-----------------------------------+   +-----------------------------------+  |`  
`|  | DRIVE A: (3.5" 1.44MB Floppy)     |   | DRIVE D: (Secondary IDE CD-ROM)   |  |`  
`|  | Mounted: [win95_boot.img]         |   | Mounted: [sc2000_se.cue (Audio)]  |  |`  
`|  | [Eject Door]  [Write Protect: OFF]|   | [Eject Tray]  [CD Volume: 85%   ] |  |`  
`|  +-----------------------------------+   +-----------------------------------+  |`  
`+---------------------------------------------------------------------------------+`  
`|  SESSION CADDY (Press ENTER/A on any media to insert into matching Drive):      |`  
`|                                                                                 |`  
`|   .-------------.   .=============.   .-------------.   .-------------------.   |`  
`|   |  [=======]  |   || [=======] ||   |  [=======]  |   |    .-"""""""-.    |   |`  
`|   |             |   || *IN DRV A*||   |             |   |  .'   .--.    '.  |   |`  
`|   |  MS-DOS 6.22|   || WIN95 OSR2||   |  SOUND      |   |  |   (    ) ♫  |  |   |`  
`|   |   Disk 1    |   || Boot Disk ||   |  BLASTER 16 |   |  '.   '--'    .'  |   |`  
`|   |   .___.     |   ||   .___.   ||   |  Drivers    |   |    '-.......-'    |   |`  
`|   '-------------'   '============='   '-------------'   '-------------------'   |`  
`|     Slot 1 (A:)       Slot 2 (A:)       Slot 3 (A:)          Slot 1 (D:)        |`  
`|                                                                                 |`  
`|  [+] Load Disk/ISO from USB Stick (/run/media/kiosk/SANDISK_32G)                |`  
`+---------------------------------------------------------------------------------+`  
`|  [F12+D / Esc] Resume Game    [Ctrl+Alt+Del] Send Reboot    [Quit to Kiosk Hub] |`  
`+=================================================================================+`

#### **4.3 Direct Hardware Remounting & Guest OS Signaling (PerformHotSwap())**

When the user selects a new floppy or CD in the overlay and presses Enter:

> 1. **Dynamic Pool Expansion (If Selected from USB or Global Pool):**  
   * Previously, standard DOSBox required all swappable disks to be specified on the IMGMOUNT line prior to boot.  
   * In the Kiosk Fork, DriveManager::AppendAndSwapDisk(int drive\_idx, const std::string& host\_path) instantiates a new imageDisk (for A:/B:) or isoDrive / CDROM\_Interface\_Image (for D:) at runtime and appends it to DriveManager::driveInfos\[drive\_idx\].disks.  
> 2. **Floppy Controller (FDC 0x3F7) Latch Notification:**  
   * Calls fdc\_SwapDisk(drive\_idx, new\_slot).  
   * Sets bit 7 (0x80 — DSKCHG) in the FDC Digital Input Register (0x3F7) and starts a **100ms emulated timer (PIC\_AddEvent)** that blocks read commands with ST0 \= 0x40 / Error \= Drive Not Ready before latching the new image.  
   * *Why this matters:* If you swap a disk instantaneously (0ms) while MS-DOS or Windows 95 is polling the drive, the guest OS sometimes fails to notice the media changed and caches the old FAT table, corrupting directory reads. Holding the virtual drive door open for 100ms of emulated CPU time forces DOS/Win9x to invalidate its FAT cache and re-read the new BPB cleanly.  
> 3. **ATAPI CD-ROM Sense Notification (IDE\_ATAPI\_MediaChange):**  
   * For IDE CD-ROM swaps on Drive D:, triggers atapi\_cdrom-\>SignalMediaEject() followed 250ms of emulated CPU time later by atapi\_cdrom-\>SignalMediaInsert(new\_cue\_path).  
   * Sets SCSI Sense Key 0x06 (UNIT ATTENTION) with ASC \= 0x28, ASCQ \= 0x00 (NOT READY TO READY TRANSITION, MEDIUM MAY HAVE CHANGED). Both MSCDEX.EXE in DOS and CDFS.VXD in Windows 95/98 recognize this sense code immediately and refresh the CD directory and Redbook audio TOC.

### **5\. Complete Kiosk Component & State Summary**

| Module | Source File | Key Responsibilities |
| :---- | :---- | :---- |
| **Kiosk Lifecycle & Watchdog** | src/kiosk/kiosk\_main.cpp | Manages the 6-state machine, 30s countdown watchdog, and intercepts guest APM/Port 0x8900 shutdown signals. |
| **SQLite Catalog Layer** | src/kiosk/kiosk\_db.cpp | Thread-safe WAL queries against catalog.db for Machines, Games, Apps, Media Pool, and Session Caddies. |
| **Hardware Archetype Engine** | src/kiosk/machine/archetypes.cpp | Synthesizes layered DOSBox-X .conf profiles (ARCH\_XT\_8088 through ARCH\_PENTIUM\_3DFX). |
| **Ephemeral Boot Injector** | src/kiosk/machine/boot\_injector.cpp | Generates RAM-backed D:\\ (kiosk\_dispatch.img) containing KIOSKGO.BAT, KIOSKWIN.EXE, and the 48-byte KIOSKEXT.COM. |
| **FAT12/16/32 Image Crawler** | src/kiosk/ingest/image\_crawler.cpp | Userspace read-only disk parser, .GRP/.LNK shortcut resolver, DOS stub-stripping fingerprinter, and NE/PE icon extractor. |
| **GOG & Steam Ingest Engine** | src/kiosk/ingest/archive\_importer.cpp | Unpacks innoextract/.zip payloads, converts .gog/.inst to .bin/.cue, and maps vendor .conf files to Archetypes. |
| **Skeuomorphic UI & Overlay** | src/kiosk/ui/view\_disk\_caddy.cpp | Renders procedural 5.25", 3.5", and CD-ROM media and executes safe DMA-paused hot-swaps via F12+D. |

---

*User prompt: Yes, design the Minimal Linux Host OS setup, including the systemd kiosk service, SDL2 KMSDRM configuration, OverlayFS layout, and the udisks2 DBus USB automounter bridge.*

### **Response: 1\. Host Appliance Partition & OverlayFS Layout**

To ensure the kiosk can survive abrupt power-offs (like flipping a physical arcade cabinet switch) without corrupting the Linux host OS, the root filesystem (/) is packaged as a read-only **SquashFS** image layered under a RAM-backed tmpfs via **OverlayFS**. All persistent state (catalog.db, virtual machines, and the media pool) lives on a dedicated writable data partition mounted with strict commit barriers.

`+-------------------------------------------------------------------------------+`  
`|                        PHYSICAL STORAGE (NVMe / SSD / eMMC)                   |`  
`+-------------------+---------------------------+-------------------------------+`  
`| Partition 1 (ESP) | Partition 2 (RO RootFS)   | Partition 3 (Writable Data)   |`  
`| /dev/nvme0n1p1    | /dev/nvme0n1p2            | /dev/nvme0n1p3                |`  
`| FAT32 (256 MB)    | SquashFS (1.5 GB)         | Ext4 / Btrfs (Remaining Space)|`  
`| - systemd-boot    | - Minimal Arch / Buildroot| - /var/lib/dosbox-kiosk/      |`  
`| - vmlinuz-linux   | - Mesa / KMSDRM / ALSA    | - catalog.db (SQLite WAL)     |`  
`| - initramfs       | - /usr/bin/dosbox-x-kiosk | - machines/ & media_pool/     |`  
`+-------------------+---------------------------+-------------------------------+`  
                                  `|`  
                                  `v (Mounted at initramfs stage)`  
`+-------------------------------------------------------------------------------+`  
`|                     RUNTIME FILESYSTEM VIEW (OverlayFS)                       |`  
`|  UpperDir: tmpfs (/run/overlay/upper)   [Ephemeral RAM - Wiped on Reboot]     |`  
`|  LowerDir: /dev/nvme0n1p2 (SquashFS)    [Immutable Firmware Base]             |`  
`|  Bind Mount: /var/lib/dosbox-kiosk      [Persistent Writable Partition 3]     |`  
`|  Tmpfs Mount: /run/user/1000            [Holds ephemeral D:\ dispatch.img]    |`  
`+-------------------------------------------------------------------------------+`

#### **1.1 /etc/fstab Specification**

`# <file system>        <mount point>           <type>  <options>                                                  <dump> <pass>`  
`LABEL=KIOSK_ROOT       /                       squashfs ro,defaults                                               0      0`  
`LABEL=KIOSK_DATA       /var/lib/dosbox-kiosk   ext4     rw,noatime,commit=5,data=ordered,errors=remount-ro        0      2`  
`tmpfs                  /tmp                    tmpfs    rw,nosuid,nodev,size=512M                                 0      0`  
`tmpfs                  /var/log                tmpfs    rw,nosuid,nodev,size=64M,mode=0755                        0      0`

> * **commit=5 & data=ordered:** Forces the Linux kernel to flush dirty Ext4 pages on the persistent partition every 5 seconds and guarantees file data is committed to disk before metadata updates—critical when a guest OS writes save files inside hdd\_c.img.

#### **1.2 Silent Instant-Boot Kernel Parameters (/boot/loader/entries/kiosk.conf)**

To hide all Linux boot text and transition directly from the BIOS/UEFI splash into the DOSBox-X Kiosk Shell in under 3 seconds:

`title   DOSBox-X Kiosk Appliance`  
`linux   /vmlinuz-linux`  
`initrd  /initramfs-linux.img`  
`options root=LABEL=KIOSK_ROOT ro overlayroot=tmpfs quiet loglevel=0 systemd.show_status=false rd.udev.log_level=0 vt.global_cursor_default=0 consoleblank=0 mitigations=off`

### **2\. Zero-Compositor Display & Audio Pipeline (SDL2 KMSDRM)**

Running X11 or a full desktop environment adds input latency, startup delay, and window-management edge cases (such as modal popups stealing focus). The Kiosk binary links against **SDL2 compiled with KMSDRM**, acquiring the Direct Rendering Manager (DRM) master lock directly from systemd-logind via libseat.

#### **2.1 GPU & KMSDRM Environment Profile**

> * **Direct Page Flipping:** SDL\_VIDEODRIVER=kmsdrm renders the OpenGL 3.3 Core context directly onto the primary hardware plane using atomic mode-setting (DRM\_MODE\_PAGE\_FLIP\_ASYNC or VSync).  
> * **Multi-Connector Fallback:** If the kiosk PC has multiple video outputs, DOSBox-X checks SDL\_KMSDRM\_DEVICE\_INDEX and selects the first connected display (DRM\_MODE\_CONNECTED).  
> * **Low-Latency Audio (PipeWire / ALSA Direct):** Uses a lightweight pipewire-pulse user daemon (or direct ALSA dmix when running on single-purpose hardware) with a 512-sample period so OPL3 FM synthesis and Sound Blaster 16 DMA maintain less than 10ms of latency.

### **3\. systemd Service & Watchdog Architecture**

Instead of getty prompting for a login on tty1, getty@tty1.service is masked and replaced by dosbox-kiosk.service.

#### **3.1 /etc/systemd/system/dosbox-kiosk.service**

`[Unit]`  
`Description=DOSBox-X Kiosk Appliance Service`  
`After=systemd-user-sessions.service systemd-logind.service udisks2.service sound.target`  
`Wants=udisks2.service`  
`Conflicts=getty@tty1.service`

`[Service]`  
`Type=notify`  
`User=kiosk`  
`Group=kiosk`  
`SupplementaryGroups=video audio input render disk optical storage`

`# Acquire TTY1 for DRM Master & libinput access via systemd-logind`  
`TTYPath=/dev/tty1`  
`TTYReset=yes`  
`TTYVHangup=yes`  
`TTYVTDisallocate=yes`  
`StandardInput=tty-fail`  
`StandardOutput=journal`  
`StandardError=journal`  
`PAMName=login`

`# SDL2 KMSDRM & OpenGL Hardware Acceleration Environment`  
`Environment="SDL_VIDEODRIVER=kmsdrm"`  
`Environment="SDL_AUDIODRIVER=alsa"`  
`Environment="SDL_RENDER_VSYNC=1"`  
`Environment="XDG_RUNTIME_DIR=/run/user/1000"`  
`Environment="DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus"`  
`Environment="MESA_SHADER_CACHE_DIR=/var/lib/dosbox-kiosk/cache/mesa"`

`# Pre-flight: Ensure RAM dispatch directory exists and clean stale locks`  
`ExecStartPre=/usr/bin/mkdir -p /run/user/1000/kiosk`  
`ExecStartPre=/usr/bin/rm -f /run/user/1000/kiosk/dispatch.img`

`# Main Kiosk Binary Execution`  
`ExecStart=/usr/bin/dosbox-x-kiosk \`  
    `--kiosk-mode \`  
    `--kiosk-db /var/lib/dosbox-kiosk/catalog.db \`  
    `--conf /var/lib/dosbox-kiosk/config/kiosk_base.conf`

`# Automatic Instant Recovery & Hardware Hang Protection`  
`Restart=always`  
`RestartSec=500ms`  
`WatchdogSec=10s`  
`TimeoutStopSec=5s`

`# Real-time CPU & I/O Scheduling Priority for Cycle-Accurate Emulation`  
`Nice=-10`  
`IOSchedulingClass=realtime`  
`IOSchedulingPriority=2`  
`LimitRTPRIO=95`  
`LimitMEMLOCK=infinity`

`[Install]`  
`WantedBy=multi-user.target`

#### **3.2 sd\_notify Heartbeat Integration (src/kiosk/kiosk\_main.cpp)**

Notice Type=notify and WatchdogSec=10s in the unit file above. If a runaway dynamic-core jump or OpenGL driver hang freezes the DOSBox-X main thread for more than 10 seconds, systemd automatically kills the hung process and restarts the Kiosk Shell within 500ms.

> * Inside GFX\_Events() (called once per frame in both the Kiosk UI and during guest emulation), DOSBox-X pings the watchdog every 2 seconds:  
>   `#ifdef HAVE_LIBSYSTEMD`  
>   `if (SDL_TICKS_PASSED(current_ticks, last_watchdog_ping + 2000)) {`  
>       `sd_notify(0, "WATCHDOG=1");`  
>       `last_watchdog_ping = current_ticks;`  
>   `}`  
>   `#endif`

### **4\. udisks2 \+ Polkit \+ DBus USB Automounter Bridge**

When a user walks up to the kiosk and plugs in a USB flash drive or an external USB floppy/CD drive containing .img, .iso, or GOG .zip files, the host OS must mount it automatically without root prompts and notify the running DOSBox-X process immediately.

`[User Inserts USB Drive]`  
        `|`  
        `v`  
`[Linux Kernel / udev] ---> [udisks2.service (System DBus)]`  
                                  `|`  
                                  ``| (Broadcasts `InterfacesAdded` signal on``  
                                  ``|  `org.freedesktop.UDisks2`)``  
                                  `v`  
            `+---------------------------------------------------+`  
            ``| DOSBox-X Kiosk DBus Listener (`usb_bridge.cpp`)   |``  
            ``|  1. Inspects `org.freedesktop.UDisks2.Block`      |``  
            ``|  2. Calls `Filesystem.Mount()` via DBus           |``  
            `|     (Authorized silently by Polkit rule)          |`  
            ``|  3. Receives `/run/media/kiosk/<LABEL>` path      |``  
            ``|  4. Scans for `.img`, `.ima`, `.iso`, `.cue`,     |``  
            ``|     `.zip`, and `setup_*.exe`                     |``  
            ``|  5. Populates `media_pool (is_removable_usb = 1)` |``  
            `|  6. Pushes Toast Notification to Kiosk UI / F12+D |`  
            `+---------------------------------------------------+`

#### **4.1 Zero-Prompt Polkit Authorization (/etc/polkit-1/rules.d/10-dosbox-kiosk-storage.rules)**

Allows the unprivileged kiosk user to mount, unmount, and eject removable block devices via udisks2:

`polkit.addRule(function(action, subject) {`  
    `if ((action.id == "org.freedesktop.udisks2.filesystem-mount" ||`  
         `action.id == "org.freedesktop.udisks2.filesystem-mount-system" ||`  
         `action.id == "org.freedesktop.udisks2.filesystem-unmount-others" ||`  
         `action.id == "org.freedesktop.udisks2.eject-media") &&`  
        `subject.user == "kiosk") {`  
        `return polkit.Result.YES;`  
    `}`  
`});`

#### **4.2 In-Process sdbus-c++ / libsystemd Listener (src/kiosk/ingest/usb\_bridge.cpp)**

Instead of relying on external bash scripts or polling /run/media in a loop, DOSBox-X subscribes directly to the System DBus using sd-bus (part of libsystemd, which is already linked for sd\_notify):

`class KioskUsbBridge {`  
`public:`  
    `void InitSystemBusListener();`  
    `void PollEventsNonBlocking(); // Called once per frame in Kiosk UI or F12+D overlay`  
    `bool SafeUnmountDevice(const std::string& block_object_path);`

`private:`  
    `sd_bus* sys_bus = nullptr;`  
    `sd_bus_slot* match_added_slot = nullptr;`  
    `sd_bus_slot* match_removed_slot = nullptr;`

    `static int OnInterfacesAdded(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);`  
    `static int OnInterfacesRemoved(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);`  
    `void MountAndIndexVolume(const std::string& dbus_object_path);`  
`};`

#### **4.3 Lifecycle of a USB Hotplug Event**

> 1. **Insertion (OnInterfacesAdded):**  
   * When an object path matching /org/freedesktop/UDisks2/block\_devices/sd\* reports the org.freedesktop.UDisks2.Filesystem interface, MountAndIndexVolume() invokes the DBus method:  
     * **Destination:** org.freedesktop.UDisks2  
     * **Interface:** org.freedesktop.UDisks2.Filesystem  
     * **Method:** Mount  
     * **Options:** {"options": "rw,noexec,nosuid,nodev,uid=1000,gid=1000,flush"}  
   * Note the **flush** mount option for FAT32/exFAT USB drives: if a user mounts their USB stick's DOCUMENTS folder as Drive E: inside WordPerfect 5.1 (auto\_mount\_usb\_docs \= 1), the flush option forces the Linux kernel to write changes to the USB flash memory as soon as WordPerfect closes the file handle, preventing data loss if they yank the USB stick out early.  
> 2. **Automatic Media & Ingest Indexing:**  
   * Once Mount returns /run/media/kiosk/\<VOLUME\_LABEL\>, a background worker thread scans the top 3 directory levels of the USB drive:  
     * Any .img, .ima, .vfd, .iso, or .cue files are inserted into media\_pool with is\_removable\_usb \= 1\. They immediately appear with a **USB Badge** in both the Pre-Boot Media Pool and the **Runtime F12+D Skeuomorphic Disk Caddy**.  
     * Any setup\_\*.exe (GOG) or .zip (Steam) archives trigger an on-screen toast notification: *"Found 2 importable games on USB \[SANDISK\_32G\] — Press X to Import."*  
> 3. **Removal (OnInterfacesRemoved):**  
   * If the user unplugs the USB drive, OnInterfacesRemoved catches the signal, checks if any active imageDisk handle is pointing to that mount path, safely closes the file descriptor, and executes:  
     `DELETE FROM media_pool WHERE is_removable_usb = 1 AND file_path LIKE '/run/media/kiosk/SANDISK_32G/%';`

---

*User prompt: Yes, generate the C++ header definitions and class interfaces for the core modules under src/kiosk/ and show the exact hook points in sdlmain.cpp and dosbox.cpp.*

### **Response: 1\. Core C++ Header Definitions (src/kiosk/)**

#### **1.1 src/kiosk/kiosk\_main.h — Kiosk Lifecycle & State Machine**

`#ifndef DOSBOX_KIOSK_MAIN_H`  
`#define DOSBOX_KIOSK_MAIN_H`

`#include <string>`  
`#include <memory>`  
`#include <cstdint>`

`enum class KioskState {`  
    `DISABLED = 0,`  
    `STATE_KIOSK_BOOT,`  
    `STATE_COUNTDOWN_IDLE,`  
    `STATE_SHELL_INTERACTIVE,`  
    `STATE_SYNTHESIZE_LAUNCH,`  
    `STATE_EMULATION_RUNNING,`  
    `STATE_RUNTIME_OVERLAY`  
`};`

`struct LaunchContext {`  
    `std::string machine_id;`  
    `std::string catalog_item_id;`  
    `bool is_real_hdd_boot = false;`  
    `std::string custom_disk_path;`  
`};`

`class KioskManager {`  
`public:`  
    `static KioskManager& GetInstance();`

    `bool Initialize(const std::string& db_path, const std::string& base_conf);`  
    `void RunMainLoop();`  
      
    `// Lifecycle transitions called from hooks`  
    `void EnterRuntimeOverlay();`  
    `void ResumeEmulation();`  
    `void RequestShutdown();`

    `KioskState GetState() const { return m_state; }`  
    `void SetState(KioskState state) { m_state = state; }`

    `const LaunchContext& GetCurrentLaunchContext() const { return m_launch_ctx; }`  
    `void SetLaunchContext(const LaunchContext& ctx) { m_launch_ctx = ctx; }`

`private:`  
    `KioskManager() = default;`  
    `~KioskManager() = default;`

    `KioskState m_state = KioskState::DISABLED;`  
    `LaunchContext m_launch_ctx;`  
    `float m_watchdog_timer = 30.0f;`  
    `bool m_initialized = false;`

    `void ExecuteCountdownIdle();`  
    `void ExecuteInteractiveShell();`  
    `void SynthesizeAndLaunchSession();`  
`};`

`#endif // DOSBOX_KIOSK_MAIN_H`

#### **1.2 src/kiosk/kiosk\_db.h — SQLite3 Data Access Layer**

`#ifndef DOSBOX_KIOSK_DB_H`  
`#define DOSBOX_KIOSK_DB_H`

`#include <string>`  
`#include <vector>`  
`#include <optional>`

`struct MachineRecord {`  
    `std::string id;`  
    `std::string display_name;`  
    `std::string archetype_id;`  
    `int archetype_tier;`  
    `std::string primary_hdd_path;`  
    `bool os_installed;`  
`};`

`struct CatalogItemRecord {`  
    `std::string id;`  
    `std::string machine_id;`  
    `std::string item_type; // "GAME" or "APPLICATION"`  
    `std::string title;`  
    `std::string guest_working_dir;`  
    `std::string guest_executable;`  
    `std::string icon_cache_path;`  
`};`

`class KioskDatabase {`  
`public:`  
    `static KioskDatabase& GetInstance();`

    `bool Open(const std::string& db_path);`  
    `void Close();`

    `// Machine queries`  
    `std::vector<MachineRecord> GetAllMachines();`  
    `std::optional<MachineRecord> GetMachine(const std::string& id);`  
    `bool InsertMachine(const MachineRecord& machine);`

    `// Catalog queries (Carousels)`  
    `std::vector<CatalogItemRecord> GetItemsByType(const std::string& item_type);`  
    `bool InsertCatalogItem(const CatalogItemRecord& item);`

    `// Caddy bindings`  
    `struct CaddyBinding {`  
        `std::string media_id;`  
        `char target_drive;`  
        `int slot_order;`  
    `};`  
    `std::vector<CaddyBinding> GetCaddyBindingsForItem(const std::string& item_id);`  
    `std::vector<CaddyBinding> GetCaddyBindingsForMachine(const std::string& machine_id);`

`private:`  
    `KioskDatabase() = default;`  
    `~KioskDatabase();`  
    `void* m_db = nullptr; // sqlite3* handle`  
`};`

`#endif // DOSBOX_KIOSK_DB_H`

#### **1.3 src/kiosk/machine/archetypes.h & boot\_injector.h**

`#ifndef DOSBOX_KIOSK_ARCHETYPES_H`  
`#define DOSBOX_KIOSK_ARCHETYPES_H`

`#include <string>`

`class ArchetypeManager {`  
`public:`  
    `// Writes base config plus archetype delta into target config buffer`  
    `static bool ApplyArchetypeConfig(const std::string& archetype_id, std::string& out_config_str);`  
`};`

`class BootInjector {`  
`public:`  
    `// Synthesizes ephemeral FAT12 D:\ dispatch image containing KIOSKGO.BAT & KIOSKEXT.COM`  
    `static bool BuildDispatchVolume(const std::string& image_path, const struct LaunchContext& ctx);`  
      
    `// Non-destructively hooks C:\AUTOEXEC.BAT on machine provisioning`  
    `static bool InjectDosAutoexecHook(const std::string& hdd_img_path);`  
`};`

`#endif // DOSBOX_KIOSK_ARCHETYPES_H`

#### **1.4 src/kiosk/ingest/image\_crawler.h & usb\_bridge.h**

`#ifndef DOSBOX_KIOSK_IMAGE_CRAWLER_H`  
`#define DOSBOX_KIOSK_IMAGE_CRAWLER_H`

`#include <string>`  
`#include <vector>`

`class ImageCrawler {`  
`public:`  
    `struct CrawlResult {`  
        `std::string title;`  
        `std::string working_dir;`  
        `std::string executable;`  
        `std::string icon_output_path;`  
    `};`

    `// Scans raw read-only disk image (partition -> VFAT walker -> binary scanner)`  
    `static std::vector<CrawlResult> ScanDiskImage(const std::string& hdd_img_path);`

`private:`  
    `static bool ExtractPeNeIcon(const uint8_t* file_data, size_t size, const std::string& out_png_path);`  
`};`

`class KioskUsbBridge {`  
`public:`  
    `static KioskUsbBridge& GetInstance();`  
    `bool Initialize();`  
    `void PollEvents(); // Called every frame in UI loop`

`private:`  
    `void* m_sys_bus = nullptr; // sd_bus* handle`  
`};`

`#endif // DOSBOX_KIOSK_IMAGE_CRAWLER_H`

#### **1.5 src/kiosk/ui/kiosk\_ui.h — ImGui Compositor & Overlay**

`#ifndef DOSBOX_KIOSK_UI_H`  
`#define DOSBOX_KIOSK_UI_H`

`class KioskUI {`  
`public:`  
    `static KioskUI& GetInstance();`

    `bool Initialize();`  
    `void Shutdown();`

    `// Render passes called from SDL main loop`  
    `void RenderCountdownIdle(float watchdog_sec);`  
    `void RenderInteractiveShell();`  
    `void RenderRuntimeOverlay();`

`private:`  
    `KioskUI() = default;`  
    `~KioskUI() = default;`

    `bool m_imgui_initialized = false;`  
    `int m_active_carousel_tab = 0; // 0=Games, 1=Apps, 2=Machines, 3=Media`  
    `int m_selected_card_index = 0;`  
`};`

`#endif // DOSBOX_KIOSK_UI_H`

### **2\. Exact Integration Hook Points**

#### **2.1 Hook Point 1: src/dosbox.cpp (Main Initialization & Run Loop Intercept)**

Modify main() or DOSBOX\_Init() to intercept startup when \--kiosk-mode is passed:

`// --- IN SRC/DOSBOX.CPP ---`  
`#include "kiosk/kiosk_main.h"`  
`#include "kiosk/ui/kiosk_ui.h"`

`// Inside main() or DOSBOX_Init():`  
`bool kiosk_mode_enabled = false;`  
`std::string kiosk_db_path = "/var/lib/dosbox-kiosk/catalog.db";`

`// Parse command line arguments for --kiosk-mode`  
`for (int i = 1; i < argc; ++i) {`  
    `if (std::string(argv[i]) == "--kiosk-mode") {`  
        `kiosk_mode_enabled = true;`  
    `}`  
    `if (std::string(argv[i]) == "--kiosk-db" && i + 1 < argc) {`  
        `kiosk_db_path = argv[++i];`  
    `}`  
`}`

`if (kiosk_mode_enabled) {`  
    `// Initialize Kiosk Core & SQLite Catalog`  
    `if (!KioskManager::GetInstance().Initialize(kiosk_db_path, "/var/lib/dosbox-kiosk/config/kiosk_base.conf")) {`  
        `LOG_MSG("KIOSK: Fatal error initializing kiosk manager.");`  
        `return 1;`  
    `}`  
      
    `// Enter Kiosk Boot State (Bypasses standard DOSBox-X command line parsing)`  
    `KioskManager::GetInstance().SetState(KioskState::STATE_KIOSK_BOOT);`  
    `KioskManager::GetInstance().RunMainLoop();`  
    `return 0; // Exit cleanly when kiosk shuts down`  
`}`

#### **2.2 Hook Point 2: src/gui/sdlmain.cpp (Event Router, Rendering Pass, & F12+D Hotkey)**

Modify GFX\_Events() and the main rendering loop to draw the Kiosk Shell / Runtime Overlay and catch the overlay hotkey:

`// --- IN SRC/GUI/SDLMAIN.CPP ---`  
`#include "kiosk/kiosk_main.h"`  
`#include "kiosk/ui/kiosk_ui.h"`

`// Inside GFX_Events() event loop:`  
`void GFX_Events() {`  
    `SDL_Event event;`  
    `while (SDL_PollEvent(&event)) {`  
        `// Intercept hotkey for Skeuomorphic Overlay (F12 + D, or Guide button)`  
        `if (event.type == SDL_KEYDOWN) {`  
            `if (event.key.keysym.sym == SDLK_F12 && (SDL_GetModState() & KMOD_ALT)) {`  
                `if (KioskManager::GetInstance().GetState() == KioskState::STATE_EMULATION_RUNNING) {`  
                    `KioskManager::GetInstance().EnterRuntimeOverlay();`  
                    `continue; // Skip passing to guest`  
                `} else if (KioskManager::GetInstance().GetState() == KioskState::STATE_RUNTIME_OVERLAY) {`  
                    `KioskManager::GetInstance().ResumeEmulation();`  
                    `continue;`  
                `}`  
            `}`  
        `}`  
          
        `// Pass events to Kiosk UI compositor if active`  
        `if (KioskManager::GetInstance().GetState() != KioskState::STATE_EMULATION_RUNNING) {`  
            `// Handle ImGui event polling for Kiosk Shell / Overlay`  
            `ImGui_ImplSDL2_ProcessEvent(&event);`  
            `continue;`   
        `}`

        `// ... Standard DOSBox-X event processing ...`  
    `}`  
`}`

`// Inside the main frame presentation block (where SDL_GL_SwapWindow is called):`  
`void GFX_EndUpdate() {`  
    `// 1. Render standard guest framebuffer texture (CRT Shader pass)`  
    `// ...`

    `// 2. Composite Kiosk UI Layer over active frame if needed`  
    `KioskState current_state = KioskManager::GetInstance().GetState();`  
    `if (current_state == KioskState::STATE_COUNTDOWN_IDLE) {`  
        `KioskUI::GetInstance().RenderCountdownIdle(30.0f);`  
    `} else if (current_state == KioskState::STATE_SHELL_INTERACTIVE) {`  
        `KioskUI::GetInstance().RenderInteractiveShell();`  
    `} else if (current_state == KioskState::STATE_RUNTIME_OVERLAY) {`  
        `KioskUI::GetInstance().RenderRuntimeOverlay();`  
    `}`

    `SDL_GL_SwapWindow(sdl.window);`  
`}`

#### **2.3 Hook Point 3: src/ints/bios.cpp (Intercepting Guest APM & Port 0x8900 Shutdown)**

Modify the BIOS interrupt handler and I/O port handler so guest OS power-offs return to the Kiosk Shell instead of killing the process:

`// --- IN SRC/INTS/BIOS.CPP ---`  
`#include "kiosk/kiosk_main.h"`

`// Inside INT 15h BIOS handler (APM Power State check):`  
`// When guest executes INT 15h with AX=5307h (Set Power State) and CX=0003h (Off):`  
`if (reg_ax == 0x5307 && reg_cx == 0x0003) {`  
    `LOG_MSG("KIOSK: Caught Guest OS APM Power-Off Request.");`  
    `KioskManager::GetInstance().ResumeEmulation(); // Unpause loop`  
    `throw KioskGuestShutdownException{};           // Caught by RunEmulationSession() try/catch`  
`}`

`// Inside Port 0x8900 I/O write handler (Bochs/DOSBox-X shutdown port fallback):`  
`void write_port_8900(Bitu port, Bitu val, Bitu iolen) {`  
    `// Appends character to shutdown string; when "Shutdown" complete:`  
    `if (is_shutdown_complete) {`  
        `LOG_MSG("KIOSK: Caught Port 0x8900 Hardware Shutdown Signal.");`  
        `throw KioskGuestShutdownException{};`  
    `}`  
`}`

---

