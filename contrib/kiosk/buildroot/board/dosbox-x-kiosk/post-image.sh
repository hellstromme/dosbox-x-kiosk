#!/bin/sh
# Buildroot post-image script: builds minimal overlayroot initramfs + GRUB config,
# then assembles dosbox-x-kiosk-appliance.img via genimage.
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
INITRAMFS_DIR="${BUILD_DIR}/kiosk-initramfs"

# 1. Stage systemd-boot entry and GRUB2 EFI silent instant-boot config
cp "${BOARD_DIR}/boot/loader/entries/kiosk.conf" "${BINARIES_DIR}/kiosk.conf"
cat > "${BINARIES_DIR}/grub.cfg" <<'EOF'
set default="0"
set timeout="0"
menuentry "DOSBox-X Kiosk Appliance" {
    linux /bzImage root=PARTLABEL=KIOSK_ROOT ro overlayroot=tmpfs quiet loglevel=0 systemd.show_status=false rd.udev.log_level=0 vt.global_cursor_default=0 consoleblank=0
    initrd /initramfs.cpio
}
EOF

# 2. Build a tiny BusyBox initramfs (< 4 MB) running initramfs-overlayroot.sh as /init
rm -rf "${INITRAMFS_DIR}"
mkdir -p "${INITRAMFS_DIR}/bin" "${INITRAMFS_DIR}/sbin" "${INITRAMFS_DIR}/proc" \
         "${INITRAMFS_DIR}/sys" "${INITRAMFS_DIR}/dev" "${INITRAMFS_DIR}/lib" "${INITRAMFS_DIR}/lib64"
cp -a "${TARGET_DIR}/bin/busybox" "${INITRAMFS_DIR}/bin/busybox"
cp -a "${TARGET_DIR}/lib"/ld-* "${TARGET_DIR}/lib"/libc.so* "${INITRAMFS_DIR}/lib/" 2>/dev/null || true
cp -a "${TARGET_DIR}/lib64"/ld-* "${INITRAMFS_DIR}/lib64/" 2>/dev/null || true
for applet in sh mount mkdir sleep head grep sed switch_root; do
    ln -sf busybox "${INITRAMFS_DIR}/bin/${applet}"
done
install -m 0755 "${BOARD_DIR}/initramfs-overlayroot.sh" "${INITRAMFS_DIR}/init"
(cd "${INITRAMFS_DIR}" && find . | cpio -H newc -o > "${BINARIES_DIR}/kiosk-initramfs.cpio")

# 3. Assemble 3-partition GPT disk image
rm -rf "${GENIMAGE_TMP}"
genimage \
    --rootpath "${TARGET_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"

echo "DOSBox-X Kiosk Appliance image ready: ${BINARIES_DIR}/dosbox-x-kiosk-appliance.img"
