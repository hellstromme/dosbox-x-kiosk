#!/bin/sh
# Safe label-sanitizing USB/CDROM mount helper for eudev / systemd (no udisks2/polkit)
# Tracks mount ownership by block device name so colliding labels never unmount another device.
ACTION="$1"
DEVNODE="$2"
RAW_LABEL="$3"
FALLBACK_ID="$4"
FS_TYPE="$5"

MOUNT_BASE="/run/media/kiosk"
STATE_DIR="${MOUNT_BASE}/.mounts"
DEV_BASE="$(basename "$DEVNODE")"
STATE_FILE="${STATE_DIR}/${DEV_BASE}"

KIOSK_UID="$(id -u kiosk 2>/dev/null || echo 1000)"
KIOSK_GID="$(id -g kiosk 2>/dev/null || echo 1000)"

RAW_NAME="${RAW_LABEL:-${FALLBACK_ID:-$DEV_BASE}}"
# Strip anything outside [A-Za-z0-9._-] and trim leading/trailing dots/underscores to block '.'/'..' traversal
SAFE_NAME="$(printf '%s' "$RAW_NAME" | tr -c 'A-Za-z0-9._-' '_' | sed 's/^[._]*//;s/[._]*$//')"
if [ -z "$SAFE_NAME" ] || [ "$SAFE_NAME" = "." ] || [ "$SAFE_NAME" = ".." ]; then
    SAFE_NAME="$DEV_BASE"
fi

is_mountpoint_occupied() {
    target_mp="$1"
    while read -r _src mp _rest; do
        [ "$mp" = "$target_mp" ] && return 0
    done < /proc/mounts 2>/dev/null
    return 1
}

is_mounted_by_dev() {
    target_dev="$1"
    target_mp="$2"
    while read -r src mp _rest; do
        if [ "$src" = "$target_dev" ] && [ "$mp" = "$target_mp" ]; then
            return 0
        fi
    done < /proc/mounts 2>/dev/null
    return 1
}

case "$ACTION" in
    add)
        mkdir -p "$STATE_DIR"
        MOUNT_POINT="${MOUNT_BASE}/${SAFE_NAME}"
        # If another device already occupies MOUNT_POINT, disambiguate with device name
        if [ -d "$MOUNT_POINT" ] && is_mountpoint_occupied "$MOUNT_POINT"; then
            MOUNT_POINT="${MOUNT_BASE}/${SAFE_NAME}_${DEV_BASE}"
        fi
        mkdir -p "$MOUNT_POINT"
        MOUNTED=0
        case "$FS_TYPE" in
            vfat|msdos)
                if mount -o "rw,noexec,nosuid,nodev,flush,uid=${KIOSK_UID},gid=${KIOSK_GID}" "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    MOUNTED=1
                fi
                ;;
            exfat|ntfs|ntfs3)
                if mount -o "rw,noexec,nosuid,nodev,uid=${KIOSK_UID},gid=${KIOSK_GID}" "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    MOUNTED=1
                fi
                ;;
            ext2|ext3|ext4|btrfs|xfs)
                if mount -o rw,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    MOUNTED=1
                fi
                ;;
            *)
                if mount -o "rw,noexec,nosuid,nodev,uid=${KIOSK_UID},gid=${KIOSK_GID}" "$DEVNODE" "$MOUNT_POINT" 2>/dev/null || \
                   mount -o rw,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                    MOUNTED=1
                fi
                ;;
        esac
        if [ "$MOUNTED" -eq 0 ]; then
            if mount -o ro,noexec,nosuid,nodev "$DEVNODE" "$MOUNT_POINT" 2>/dev/null; then
                MOUNTED=1
            fi
        fi
        if [ "$MOUNTED" -eq 1 ]; then
            printf '%s\n' "$MOUNT_POINT" > "$STATE_FILE"
        else
            rmdir "$MOUNT_POINT" 2>/dev/null || true
        fi
        ;;
    remove)
        if [ -f "$STATE_FILE" ]; then
            RECORDED_MP="$(cat "$STATE_FILE" 2>/dev/null || true)"
            rm -f "$STATE_FILE"
            if [ -n "$RECORDED_MP" ] && [ -d "$RECORDED_MP" ]; then
                # Unmount only if this exact device still owns the recorded mountpoint (literal comparison)
                if is_mounted_by_dev "$DEVNODE" "$RECORDED_MP"; then
                    umount -l "$RECORDED_MP" 2>/dev/null || true
                    rmdir "$RECORDED_MP" 2>/dev/null || true
                fi
            fi
        fi
        ;;
esac
exit 0
