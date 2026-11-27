#!/bin/sh
# Safe label-sanitizing USB/CDROM mount helper for eudev (no udisks2/polkit)
ACTION="$1"
DEVNODE="$2"
RAW_LABEL="$3"
FALLBACK_ID="$4"
FS_TYPE="$5"

MOUNT_BASE="/run/media/kiosk"
RAW_NAME="${RAW_LABEL:-${FALLBACK_ID:-$(basename "$DEVNODE")}}"
# Strip anything outside [A-Za-z0-9._-] and trim leading/trailing dots/underscores to block '.'/'..' traversal
SAFE_NAME="$(printf '%s' "$RAW_NAME" | tr -c 'A-Za-z0-9._-' '_' | sed 's/^[._]*//;s/[._]*$//')"
if [ -z "$SAFE_NAME" ] || [ "$SAFE_NAME" = "." ] || [ "$SAFE_NAME" = ".." ]; then
    SAFE_NAME="$(basename "$DEVNODE")"
fi

MOUNT_POINT="${MOUNT_BASE}/${SAFE_NAME}"

case "$ACTION" in
    add)
        mkdir -p "$MOUNT_POINT"
        case "$FS_TYPE" in
            vfat|msdos|exfat|ntfs|ntfs3)
                if mount -o rw,noexec,nosuid,nodev,flush,uid=1000,gid=1000 "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    exit 0
                fi
                ;;
            ext2|ext3|ext4|btrfs|xfs)
                if mount -o rw,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    exit 0
                fi
                ;;
            *)
                if mount -o rw,noexec,nosuid,nodev,flush,uid=1000,gid=1000 "$DEVNODE" "$MOUNT_POINT" 2>/dev/null || \
                   mount -o rw,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    exit 0
                fi
                ;;
        esac
        # Fallback for read-only filesystems (ISO9660 / dirty NTFS)
        mount -o ro,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null || rmdir "$MOUNT_POINT" 2>/dev/null || true
        ;;
    remove)
        if [ -d "$MOUNT_POINT" ]; then
            umount -l "$MOUNT_POINT" 2>/dev/null || true
            rmdir "$MOUNT_POINT" 2>/dev/null || true
        fi
        ;;
esac
exit 0
