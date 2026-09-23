# hermes-radio-daemon — unified HERMES radio control daemon
#
# Copyright (C) 2024-2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC      = gcc
# -MMD -MP auto-generates per-object header dependencies (*.d), so editing a
# shared header (e.g. radio.h) rebuilds every object that includes it. Without
# this, changing a struct layout left stale objects with the old layout — fields
# read at mismatched offsets, corrupting memory (a very painful class of bug).
CFLAGS  = -Ofast -Wall -std=gnu11 -fstack-protector -MMD -MP \
          -I. -Ihamlib -I/usr/include/iniparser -I/usr/include/csdr -Iinclude \
          -Wno-deprecated-declarations
LDFLAGS = -liniparser -lhamlib -lasound -lcrypto -lssl -lfftw3f -lfftw3 \
          -lpthread -lm -li2c -lcsdr -lspecbleach -lcw -lrt -lmbe-neo

# Mongoose now serves as the websocket transport in radio_websocket.c.
CFLAGS += -DMG_ENABLE_OPENSSL=1 -DMG_TLS=MG_TLS_OPENSSL

include vendor/rade_c/sources.mk
include vendor/ft8_lib/sources.mk
include vendor/minimodem/sources.mk

uname_p := $(shell uname -m)
ifeq ($(uname_p),aarch64)
	CFLAGS += -moutline-atomics -march=armv8-a+crc
else
	CFLAGS += -march=x86-64-v2
endif

EXTRA_CPPFLAGS = $(RADE_C_EMBED_CPPFLAGS) $(FT8_LIB_CPPFLAGS) $(MM_FSK_CPPFLAGS)
EXTRA_CFLAGS   = $(RADE_C_EMBED_CFLAGS)   $(FT8_LIB_CFLAGS)   $(MM_FSK_CFLAGS)

RADE_C_EMBED_OBJS = $(RADE_C_EMBED_SRCS:.c=.o)
FT8_LIB_OBJS      = $(FT8_LIB_SRCS:.c=.o)
MM_FSK_OBJS       = $(MM_FSK_SRCS:.c=.o)

.PHONY: all clean install test compat-tests

all: radio_daemon radio_client

TEST_CFLAGS = -O0 -Wall -Wextra -std=gnu11 -fstack-protector \
              -I. -Ihamlib -I/usr/include/iniparser -Iinclude
TEST_BINS = tests/backend_selection_test tests/compat_surface_test tests/controls_test \
            tests/dstar_voice_test tests/upsample2_test tests/rig_server_test \
            tests/hamlib_ptt_test

# ── daemon-level objects ────────────────────────────────────────
DAEMON_TOP_OBJS = radio_daemon.o \
                  radio_backend.o \
                  radio_controls.o \
                  radio_daemon_core.o \
                  radio_pipeline.o \
                  hamlib/radio_hamlib.o \
                  hamlib/hamlib_digi.o \
                  hamlib/rig_server.o \
                  cat_server.o \
                  radio_media.o \
                  radio_shm.o \
                  radio_websocket.o \
                  audio_bridge.o \
                  audio_headset.o \
                  shm_audio.o \
                  loop_audio.o \
                  vendor/hermes_shm/ring_buffer_posix.o \
                  vendor/hermes_shm/shm_posix.o \
                  cfg_utils.o \
                  shm_utils.o \
                  mongoose.o

# ── embedded sBitx HW/DSP/ALSA objects (now plain .o, no objcopy) ──
SBITX_GPIOLIB_OBJS = sbitx/gpiolib/gpiolib.o \
                     sbitx/gpiolib/gpiochip_bcm2712.o \
                     sbitx/gpiolib/gpiochip_bcm2835.o \
                     sbitx/gpiolib/gpiochip_rp1.o \
                     sbitx/gpiolib/util.o

SBITX_OBJS = sbitx/sbitx_alsa.o \
             sbitx/sbitx_buffer.o \
             sbitx/sbitx_bridge.o \
             sbitx/sbitx_core.o \
             dsp/sbitx_dsp.o \
             dsp/upsample2.o \
             sbitx/sbitx_gpio.o \
             sbitx/sbitx_i2c.o \
             dsp/sbitx_radae.o \
             dsp/sbitx_drm.o \
             dsp/sbitx_ft8.o \
             dsp/sbitx_cw.o \
             dsp/sbitx_rtty.o \
             dsp/sbitx_dstar.o \
             dsp/dstar_voice.o \
             dsp/voice_crypto.o \
             sbitx/sbitx_si5351.o \
             sbitx/ring_buffer.o \
             $(SBITX_GPIOLIB_OBJS) \
             $(RADE_C_EMBED_OBJS) \
             $(FT8_LIB_OBJS) \
             $(MM_FSK_OBJS)

DAEMON_OBJS = $(DAEMON_TOP_OBJS) $(SBITX_OBJS)

radio_daemon: $(DAEMON_OBJS)
	$(CC) -o radio_daemon $(DAEMON_OBJS) $(LDFLAGS)

# Generic compile rule. Sbitx hw/dsp need extra include paths for csdr,
# vendored rade_c/ft8_lib/minimodem.
%.o: %.c
	$(CC) -c $(CFLAGS) $(EXTRA_CPPFLAGS) $(EXTRA_CFLAGS) \
	      -Isbitx -Idsp -Isbitx/gpiolib \
	      $< -o $@

# ── client ──────────────────────────────────────────────────────
radio_client: sbitx_client.c sbitx_io.c shm_utils.c help.h \
              include/sbitx_io.h include/radio_cmds.h
	$(CC) $(CFLAGS) sbitx_client.c sbitx_io.c shm_utils.c \
	      -o radio_client -lpthread

# ── websocket control CLI, a testing aid (not built by "all") ──
ws_client: ws_client.c mongoose.c mongoose.h
	$(CC) $(CFLAGS) ws_client.c mongoose.c -o ws_client -lssl -lcrypto

# ── regression tests ───────────────────────────────────────────────
test: compat-tests

compat-tests: $(TEST_BINS)
	./tests/backend_selection_test
	./tests/compat_surface_test
	./tests/controls_test
	./tests/dstar_voice_test
	./tests/upsample2_test
	./tests/rig_server_test
	./tests/hamlib_ptt_test

tests/backend_selection_test: tests/backend_selection_test.c cfg_utils.c cfg_utils.h \
                              radio_backend.c radio_backend.h radio_daemon_core.h \
                              hamlib/radio_hamlib.h radio.h \
                              tests/fixtures/backend-default.ini \
                              tests/fixtures/backend-zbitx.ini
	$(CC) $(TEST_CFLAGS) tests/backend_selection_test.c hamlib/rig_server.c radio_controls.c cat_server.c -o $@ -liniparser -lpthread -lm

tests/rig_server_test: tests/rig_server_test.c hamlib/rig_server.c hamlib/rig_server.h \
                      radio_controls.c radio_backend.c cfg_utils.c radio.h
	$(CC) $(TEST_CFLAGS) tests/rig_server_test.c hamlib/rig_server.c cat_server.c -o $@ -liniparser -lpthread -lm

tests/hamlib_ptt_test: tests/hamlib_ptt_test.c hamlib/radio_hamlib.c hamlib/rig_server.c \
                       hamlib/rig_server.h radio_controls.c radio_backend.c radio_backend.h \
                       cfg_utils.c radio.h
	$(CC) $(TEST_CFLAGS) tests/hamlib_ptt_test.c hamlib/rig_server.c cat_server.c -o $@ -lhamlib -liniparser -lpthread -lm

tests/controls_test: tests/controls_test.c radio_controls.c radio_controls.h \
                     radio_backend.c radio_backend.h cat_server.c cat_server.h \
                     cfg_utils.c cfg_utils.h radio.h
	$(CC) $(TEST_CFLAGS) tests/controls_test.c hamlib/rig_server.c cat_server.c -o $@ -liniparser -lpthread -lm

tests/dstar_voice_test: tests/dstar_voice_test.c dsp/dstar_voice.c dsp/dstar_voice.h \
                        dsp/voice_crypto.c dsp/voice_crypto.h dsp/sbitx_dstar.c dsp/sbitx_dstar.h
	$(CC) $(TEST_CFLAGS) -Idsp tests/dstar_voice_test.c dsp/dstar_voice.c dsp/voice_crypto.c \
	      dsp/sbitx_dstar.c -o $@ -lmbe-neo -lcrypto -lm

tests/upsample2_test: tests/upsample2_test.c dsp/upsample2.c dsp/upsample2.h
	$(CC) $(TEST_CFLAGS) -I/usr/include/csdr tests/upsample2_test.c dsp/upsample2.c -o $@ -lcsdr -lfftw3f -lm

tests/compat_surface_test: tests/compat_surface_test.c radio_shm.c radio_shm.h \
                           radio_pipeline.c radio_pipeline.h \
                           radio_backend.h radio.h \
                           shm_utils.h include/sbitx_io.h include/radio_cmds.h
	$(CC) $(TEST_CFLAGS) tests/compat_surface_test.c -o $@ -lpthread

# ── install ─────────────────────────────────────────────────────
prefix     ?= /usr
sysconfdir ?= /etc

install: radio_daemon radio_client
	install -D -m 755 radio_daemon  $(DESTDIR)$(prefix)/bin/radio_daemon
	install -D -m 755 radio_client  $(DESTDIR)$(prefix)/bin/radio_client
	install -D -m 644 radiod.service $(DESTDIR)/etc/systemd/system/radiod.service
	if [ ! -e $(DESTDIR)$(prefix)/bin/sbitx_client ]; then \
	  ln -sf radio_client $(DESTDIR)$(prefix)/bin/sbitx_client; \
	fi
	install -d $(DESTDIR)$(sysconfdir)/hermes
	test -f $(DESTDIR)$(sysconfdir)/hermes/core.ini || \
	  install -m 644 config/core.ini $(DESTDIR)$(sysconfdir)/hermes/core.ini
	test -f $(DESTDIR)$(sysconfdir)/hermes/user.ini || \
	  install -m 644 config/user.ini $(DESTDIR)$(sysconfdir)/hermes/user.ini
	install -d $(DESTDIR)$(sysconfdir)/hermes/web
	install -m 644 web/index.html $(DESTDIR)$(sysconfdir)/hermes/web/index.html
	install -D -m 644 config/avahi/hermes-radio.service \
	  $(DESTDIR)$(sysconfdir)/hermes/avahi/hermes-radio.service

# ── clean ───────────────────────────────────────────────────────
clean:
	rm -f radio_daemon radio_client ws_client \
	      $(DAEMON_OBJS) $(DAEMON_OBJS:.o=.d) $(TEST_BINS)

# Auto-generated header dependencies (from -MMD). Hyphen: ignore on first build.
-include $(DAEMON_OBJS:.o=.d)
