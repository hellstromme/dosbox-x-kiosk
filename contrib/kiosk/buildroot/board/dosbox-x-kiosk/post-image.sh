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
         "${INITRAMFS_DIR}/sys" "${INITRAMFS_DIR}/dev" "${INITRAMFS_DIR}/lib" \
         "${INITRAMFS_DIR}/lib64" "${INITRAMFS_DIR}/usr/lib"
cp -a "${TARGET_DIR}/bin/busybox" "${INITRAMFS_DIR}/bin/busybox"

# Copy complete ELF NEEDED dependency closure for busybox (including libresolv.so.2, libc, ld-linux)
READELF_BIN="$(command -v readelf || true)"
if [ -z "${READELF_BIN}" ]; then
    echo "ERROR: readelf not found on build host; cannot resolve BusyBox initramfs shared library closure" >&2
    exit 1
fi

copy_elf_closure() {
    bin_path="$1"
    # Copy dynamic loader (PT_INTERP) if present
    interp="$("${READELF_BIN}" -l "${bin_path}" 2>/dev/null | sed -n 's/.*program interpreter: \(.*\)\].*/\1/p' | head -n 1)"
    if [ -n "${interp}" ]; then
        copy_target_lib "${interp#/}"
    fi
    # Queue NEEDED libraries iteratively
    queue="$("${READELF_BIN}" -d "${bin_path}" 2>/dev/null | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\].*/\1/p')"
    seen=""
    while [ -n "${queue}" ]; do
        next_queue=""
        for lib in ${queue}; do
            case " ${seen} " in
                *" ${lib} "*) continue ;;
            esac
            seen="${seen} ${lib}"
            found=""
            for search_dir in lib lib64 usr/lib usr/lib64; do
                if [ -e "${TARGET_DIR}/${search_dir}/${lib}" ] || [ -L "${TARGET_DIR}/${search_dir}/${lib}" ]; then
                    found="${search_dir}/${lib}"
                    break
                fi
            done
            if [ -z "${found}" ]; then
                echo "ERROR: Missing required initramfs library '${lib}' in ${TARGET_DIR}" >&2
                exit 1
            fi
            copy_target_lib "${found}"
            deps="$("${READELF_BIN}" -d "${TARGET_DIR}/${found}" 2>/dev/null | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\].*/\1/p')"
            for d in ${deps}; do
                next_queue="${next_queue} ${d}"
            done
        done
        queue="${next_queue}"
    done
}

copy_target_lib() {
    cur="$1"
    hops=0
    while [ "$hops" -lt 16 ]; do
        dir="$(dirname "${cur}")"
        if [ ! -e "${TARGET_DIR}/${cur}" ] && [ ! -L "${TARGET_DIR}/${cur}" ]; then
            echo "ERROR: Missing required initramfs library/target '${cur}' in ${TARGET_DIR}" >&2
            exit 1
        fi
        mkdir -p "${INITRAMFS_DIR}/${dir}"
        cp -a "${TARGET_DIR}/${cur}" "${INITRAMFS_DIR}/${cur}"
        if [ ! -L "${TARGET_DIR}/${cur}" ]; then
            return 0
        fi
        link_target="$(readlink "${TARGET_DIR}/${cur}")"
        case "${link_target}" in
            /*) cur="${link_target#/}" ;;
            *)  cur="${dir}/${link_target}" ;;
        esac
        hops=$((hops + 1))
    done
    echo "ERROR: Symlink loop while staging initramfs library '${1}'" >&2
    exit 1
}

copy_elf_closure "${TARGET_DIR}/bin/busybox"

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
