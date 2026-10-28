AM_CPPFLAGS = -I$(top_srcdir)/include

SUBDIRS = zmbv gui_tk passthroughio mt32 fluidsynth decoders/internal

if C_XBRZ
SUBDIRS += xBRZ
endif

if C_KIOSK
SUBDIRS += sqlite3 imgui
endif
