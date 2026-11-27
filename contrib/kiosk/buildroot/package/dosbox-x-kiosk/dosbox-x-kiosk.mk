################################################################################
#
# dosbox-x-kiosk
#
################################################################################

DOSBOX_X_KIOSK_VERSION = main
DOSBOX_X_KIOSK_SITE = $(BR2_EXTERNAL_DOSBOX_X_KIOSK_PATH)/../../..
DOSBOX_X_KIOSK_SITE_METHOD = local
DOSBOX_X_KIOSK_LICENSE = GPL-2.0+
DOSBOX_X_KIOSK_LICENSE_FILES = COPYING
DOSBOX_X_KIOSK_AUTORECONF = YES

DOSBOX_X_KIOSK_DEPENDENCIES = \
	host-pkgconf \
	sdl2 \
	alsa-lib \
	fluidsynth \
	zlib \
	libpng \
	eudev

DOSBOX_X_KIOSK_CONF_OPTS = \
	--enable-sdl2 \
	--enable-kiosk \
	--disable-x11 \
	--enable-alsa-midi \
	--disable-debug

ifeq ($(BR2_PACKAGE_SYSTEMD),y)
DOSBOX_X_KIOSK_DEPENDENCIES += systemd
endif

$(eval $(autotools-package))
