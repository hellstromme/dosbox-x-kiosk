#!/bin/sh
# Minimal BusyBox initramfs OverlayFS root mount script for DOSBox-X Kiosk Appliance
# Layers read-only SquashFS (GPT PARTLABEL=KIOSK_ROOT) under RAM-backed tmpfs upperdir.
set -e

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

mkdir -p /mnt/lower /mnt/overlay /mnt/newroot

# Wait briefly for NVMe/SATA/USB block devices to settle
ROOT_DEV=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    ROOT_DEV="$(blkid -t PARTLABEL=KIOSK_ROOT -o device 2>/dev/null | head -n 1 || true)"
    [ -n "$ROOT_DEV" ] && break
    sleep 0.2
done

if [ -z "$ROOT_DEV" ]; then
    echo "KIOSK INIT: PARTLABEL=KIOSK_ROOT not found, dropping to emergency shell"
    exec /bin/sh
fi

mount -t squashfs -o ro "$ROOT_DEV" /mnt/lower
mount -t tmpfs -o rw,size=50% tmpfs /mnt/overlay
mkdir -p /mnt/overlay/upper /mnt/overlay/work

mount -t overlay overlay \
    -o lowerdir=/mnt/lower,upperdir=/mnt/overlay/upper,workdir=/mnt/overlay/work \
    /mnt/newroot

mount --move /proc /mnt/newroot/proc
mount --move /sys /mnt/newroot/sys
mount --move /dev /mnt/newroot/dev

exec switch_root /mnt/newroot /sbin/init
