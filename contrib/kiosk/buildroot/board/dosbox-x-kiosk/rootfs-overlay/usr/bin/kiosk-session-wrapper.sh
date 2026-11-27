#!/bin/sh
# DOSBox-X Kiosk Appliance Session Wrapper (Buildroot BusyBox / KMSDRM / ALSA)
set -e

export SDL_VIDEODRIVER=kmsdrm
export SDL_AUDIODRIVER=alsa
export SDL_RENDER_VSYNC=1
export XDG_RUNTIME_DIR=/run/user/1000
export MESA_SHADER_CACHE_DIR=/var/lib/dosbox-kiosk/cache/mesa
export DOSBOX_KIOSK_APPLIANCE=1

mkdir -p /run/user/1000/kiosk /run/media/kiosk /var/lib/dosbox-kiosk/cache/mesa
rm -f /run/user/1000/kiosk/kiosk_dispatch.img

exec /usr/bin/dosbox-x \
    -kiosk-mode \
    -kiosk-db /var/lib/dosbox-kiosk/catalog.db \
    -kiosk-conf /var/lib/dosbox-kiosk/config/kiosk_base.conf
