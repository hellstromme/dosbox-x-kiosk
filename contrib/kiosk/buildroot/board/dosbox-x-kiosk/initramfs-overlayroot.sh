#!/bin/sh
# Minimal BusyBox initramfs OverlayFS root mount script for DOSBox-X Kiosk Appliance
# Layers read-only SquashFS (GPT PARTLABEL=KIOSK_ROOT) under RAM-backed tmpfs upperdir.
set -e

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

mkdir -p /mnt/lower /mnt/overlay /mnt/newroot

# Discover GPT partition named KIOSK_ROOT via /sys/class/block/*/uevent (no udev / blkid -t needed)
find_kiosk_root() {
    for uevent in /sys/class/block/*/uevent; do
        [ -r "$uevent" ] || continue
        if grep -qx 'PARTNAME=KIOSK_ROOT' "$uevent" 2>/dev/null; then
            devname="$(sed -n 's/^DEVNAME=//p' "$uevent" | head -n 1)"
            if [ -n "$devname" ] && [ -b "/dev/$devname" ]; then
                printf '/dev/%s\n' "$devname"
                return 0
            fi
        fi
    done
    return 1
}

# Wait up to 20s for slower asynchronous USB/NVMe/SATA block devices (exits immediately once found)
ROOT_DEV=""
ATTEMPT=0
while [ "$ATTEMPT" -lt 100 ]; do
    ROOT_DEV="$(find_kiosk_root || true)"
    [ -n "$ROOT_DEV" ] && break
    ATTEMPT=$((ATTEMPT + 1))
    sleep 0.2
done

if [ -z "$ROOT_DEV" ]; then
    echo "KIOSK INIT: PARTNAME=KIOSK_ROOT not found in /sys/class/block/*/uevent, dropping to emergency shell"
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
