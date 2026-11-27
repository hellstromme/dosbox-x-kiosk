# DOSBox-X Kiosk Edition — Dual-Target Host Deployment Guide

This directory contains the host-layer packaging for both **Buildroot Old-Laptop Appliances** and **systemd-based Linux Kiosk setups**, complementing **Desktop "Big Picture" Mode** on Windows and Linux.

---

## 1. Desktop "Big Picture" Mode (Windows 10/11 & Desktop Linux)

Launch DOSBox-X directly into the Kiosk Pre-Boot Shell without any daemon or special privileges:

```sh
# Windows
dosbox-x.exe -kiosk-mode

# Desktop Linux
dosbox-x -kiosk-mode
```

- **Data Root:**
  - Windows: `%LOCALAPPDATA%\DOSBox-Kiosk\` (`catalog.db`, `config/`, `media_pool/`, `os_installers/`, `ingest_staging/`, `machines/`, `cache/icons/`)
  - Linux: `$XDG_DATA_HOME/dosbox-kiosk/` or `~/.local/share/dosbox-kiosk/`
- **Removable USB / Optical Media:**
  - Windows automatically detects `DRIVE_REMOVABLE` and `DRIVE_CDROM` via `GetLogicalDrives()` + `GetDriveTypeW()`.
  - Desktop Linux watches `/run/media/$USER/` and `/media/$USER/` via non-blocking `inotify`.
  - Inserted volumes are scanned up to 3 levels deep for disk images (`.img`, `.ima`, `.vfd`, `.iso`, `.cue`) and importable archives (`setup_*.exe`, `.zip`).
- **Clean Exit:** Select **Exit Kiosk** in the header bar or **Settings** tab to return cleanly to the host desktop OS.

---

## 2. Buildroot Old-Laptop Appliance (`BR2_PACKAGE_DOSBOX_X_KIOSK=y`)

Designed for repurposed x86_64 laptops (ThinkPads, netbooks, integrated Intel/AMD/Nouveau GPUs) with `< 15 MB` host OS RAM footprint, zero X11/Wayland/PulseAudio/`udisks2`/`polkit` overhead, direct SDL2 `KMSDRM` + `ALSA`, read-only SquashFS + tmpfs OverlayFS root, and crash-safe Ext4 `/var/lib/dosbox-kiosk/`.

### Building the Appliance Image (on Linux / WSL2)

```bash
# 1. Fetch Buildroot 2025.02 LTS
git clone --branch 2025.02.x --depth 1 https://gitlab.com/buildroot.org/buildroot.git
cd buildroot

# 2. Configure using the DOSBox-X Kiosk BR2_EXTERNAL tree
make BR2_EXTERNAL=/path/to/dosbox-x-kiosk/contrib/kiosk/buildroot dosbox_x_kiosk_x86_64_defconfig

# 3. Build the 3-partition GPT image (ESP + SquashFS RO + Ext4 Data)
make -j"$(nproc)"
```

The resulting bootable image is written to `output/images/dosbox-x-kiosk-appliance.img`.

### Flashing to Internal SSD / USB Stick

```bash
sudo dd if=output/images/dosbox-x-kiosk-appliance.img of=/dev/sdX bs=4M status=progress conv=fsync
```

---

## 3. Optional `systemd` Profile (Arch / Debian / Ubuntu Appliance)

For full Linux distributions using `systemd`:

1. Build DOSBox-X with `--enable-sdl2 --enable-kiosk`. Optionally install `libsystemd-dev` (Debian/Ubuntu) or `systemd-libs` (Arch) before configuring to enable `HAVE_LIBSYSTEMD=1` (`sd_notify` readiness + watchdog heartbeats), then uncomment `Type=notify` and `WatchdogSec=30` in `dosbox-kiosk.service`.
2. Provision the `kiosk` system group/account, base config, udev automount rules, and systemd unit:
   ```bash
   getent group kiosk >/dev/null || sudo groupadd -r kiosk
   id -u kiosk >/dev/null 2>&1 || sudo useradd -r -g kiosk -m -d /var/lib/dosbox-kiosk -s /usr/sbin/nologin kiosk
   sudo install -d -o kiosk -g kiosk -m 0755 /var/lib/dosbox-kiosk /var/lib/dosbox-kiosk/config
   sudo install -o kiosk -g kiosk -m 0644 contrib/kiosk/buildroot/board/dosbox-x-kiosk/rootfs-overlay/etc/dosbox-kiosk/kiosk_base.conf /var/lib/dosbox-kiosk/config/
   sudo install -m 0644 contrib/kiosk/systemd/dosbox-kiosk.service /etc/systemd/system/
   sudo install -m 0644 contrib/kiosk/systemd/kiosk-usb-mount@.service /etc/systemd/system/
   sudo install -m 0644 contrib/kiosk/systemd/99-kiosk-automount-systemd.rules /etc/udev/rules.d/
   sudo install -m 0755 contrib/kiosk/buildroot/board/dosbox-x-kiosk/rootfs-overlay/usr/libexec/kiosk-usb-mount.sh /usr/libexec/
   sudo udevadm control --reload-rules
   sudo udevadm trigger --subsystem-match=block --action=add
   sudo systemctl daemon-reload
   sudo systemctl enable --now dosbox-kiosk.service
   ```
