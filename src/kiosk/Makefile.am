AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src/kiosk -I$(top_srcdir)/src/libs/sqlite3 -I$(top_srcdir)/src/libs/imgui -I$(top_srcdir)/src/libs/imgui/backends -I$(top_srcdir)/src/libs/stb

noinst_LIBRARIES = libkiosk.a
libkiosk_a_SOURCES = kiosk_main.cpp kiosk_main.h kiosk_types.h \
                     kiosk_db.cpp kiosk_db.h \
                     ui/kiosk_ui.cpp ui/kiosk_ui.h \
                     machine/archetypes.cpp machine/archetypes.h \
                     machine/config_synthesizer.cpp machine/config_synthesizer.h
