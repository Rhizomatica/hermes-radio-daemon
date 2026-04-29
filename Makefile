# hermes-radio-daemon – Hamlib-based HERMES radio control daemon
#
# Copyright (C) 2024-2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC      = gcc
OBJCOPY ?= objcopy
CFLAGS  = -Ofast -Wall -std=gnu11 -fstack-protector \
          -I/usr/include/iniparser -Iinclude
LDFLAGS = -liniparser -lhamlib -lasound -lcrypto -lfftw3f -lpthread -lm

include vendor/radev2/sources.mk

# Detect architecture for tuning flags
uname_p := $(shell uname -m)
ifeq ($(uname_p),aarch64)
	CFLAGS += -moutline-atomics -march=armv8-a+crc
else
	CFLAGS += -march=x86-64-v2
endif

.PHONY: all clean install legacy_sbitx_controller

all: radio_daemon sbitx_client

# ── daemon ──────────────────────────────────────────────────────
DAEMON_OBJS = radio_daemon.o \
              radio_daemon_core.o \
              radio_backend.o \
              radio_pipeline.o \
              legacy_sbitx_bootstrap.o \
              radio_hamlib.o \
              radio_media.o  \
              radio_shm.o    \
              radio_websocket.o \
              cfg_utils.o    \
              shm_utils.o    \
              legacy_sbitx/embedded_prefixed.o
RADIO_DAEMON_LDFLAGS = $(LDFLAGS) -li2c -lssl -lcsdr -lfftw3

radio_daemon: $(DAEMON_OBJS)
	$(CC) -o radio_daemon $(DAEMON_OBJS) $(RADIO_DAEMON_LDFLAGS)

radio_daemon.o: radio_daemon.c radio.h radio_backend.h
	$(CC) -c $(CFLAGS) radio_daemon.c -o radio_daemon.o

radio_daemon_core.o: radio_daemon_core.c radio_daemon_core.h radio_backend.h radio.h \
                     radio_media.h radio_pipeline.h radio_shm.h radio_websocket.h cfg_utils.h
	$(CC) -c $(CFLAGS) radio_daemon_core.c -o radio_daemon_core.o

radio_backend.o: radio_backend.c radio_backend.h radio_daemon_core.h radio_hamlib.h \
                  legacy_sbitx_bootstrap.h cfg_utils.h
	$(CC) -c $(CFLAGS) radio_backend.c -o radio_backend.o

radio_pipeline.o: radio_pipeline.c radio_pipeline.h radio.h
	$(CC) -c $(CFLAGS) radio_pipeline.c -o radio_pipeline.o

legacy_sbitx_bootstrap.o: legacy_sbitx_bootstrap.c legacy_sbitx_bootstrap.h radio.h
	$(CC) -c $(CFLAGS) legacy_sbitx_bootstrap.c -o legacy_sbitx_bootstrap.o

radio_hamlib.o: radio_hamlib.c radio_hamlib.h radio.h cfg_utils.h radio_pipeline.h
	$(CC) -c $(CFLAGS) radio_hamlib.c -o radio_hamlib.o

radio_media.o: radio_media.c radio_media.h radio.h radio_pipeline.h
	$(CC) -c $(CFLAGS) radio_media.c -o radio_media.o

radio_shm.o: radio_shm.c radio_shm.h radio.h radio_backend.h \
             include/sbitx_io.h include/radio_cmds.h shm_utils.h
	$(CC) -c $(CFLAGS) radio_shm.c -o radio_shm.o

radio_websocket.o: radio_websocket.c radio_websocket.h radio.h \
                    radio_backend.h radio_media.h radio_pipeline.h
	$(CC) -c $(CFLAGS) radio_websocket.c -o radio_websocket.o

cfg_utils.o: cfg_utils.c cfg_utils.h radio.h
	$(CC) -c $(CFLAGS) cfg_utils.c -o cfg_utils.o

shm_utils.o: shm_utils.c shm_utils.h
	$(CC) -c $(CFLAGS) shm_utils.c -o shm_utils.o

# ── client ──────────────────────────────────────────────────────
sbitx_client: sbitx_client.c sbitx_io.c shm_utils.c help.h \
              include/sbitx_io.h include/radio_cmds.h
	$(CC) $(CFLAGS) sbitx_client.c sbitx_io.c shm_utils.c \
	      -o sbitx_client -lpthread

# ── vendored legacy sBitx controller ────────────────────────────
LEGACY_CFLAGS = $(CFLAGS) -I. -Ilegacy_sbitx -Ilegacy_sbitx/gpiolib -I/usr/include/csdr \
                $(RADEV2_EMBED_CPPFLAGS) $(RADEV2_EMBED_CFLAGS) \
                -Wno-deprecated-declarations
LEGACY_LDFLAGS = -liniparser -li2c -lssl -lcrypto -lpthread -lasound -lm -lfftw3 -lcsdr
LEGACY_HDRS = $(wildcard legacy_sbitx/*.h legacy_sbitx/gpiolib/*.h include/*.h \
                vendor/radev2/src/*.h vendor/radev2/support/*.h) shm_utils.h
LEGACY_GPIOLIB_SRCS = legacy_sbitx/gpiolib/gpiolib.c \
                      legacy_sbitx/gpiolib/gpiochip_bcm2712.c \
                      legacy_sbitx/gpiolib/gpiochip_bcm2835.c \
                      legacy_sbitx/gpiolib/gpiochip_rp1.c \
                      legacy_sbitx/gpiolib/util.c
LEGACY_RADAE_SRCS = $(RADEV2_EMBED_SRCS)
LEGACY_SBITX_SRCS = legacy_sbitx/sbitx_controller.c \
                    legacy_sbitx/sbitx_alsa.c \
                    legacy_sbitx/sbitx_buffer.c \
                    legacy_sbitx/sbitx_core.c \
                    legacy_sbitx/sbitx_dsp.c \
                    legacy_sbitx/sbitx_gpio.c \
                    legacy_sbitx/sbitx_i2c.c \
                    legacy_sbitx/sbitx_radae.c \
                    legacy_sbitx/sbitx_shm.c \
                    legacy_sbitx/sbitx_si5351.c \
                    legacy_sbitx/sbitx_websocket.c \
                    legacy_sbitx/cfg_utils.c \
                    legacy_sbitx/mongoose.c \
                    legacy_sbitx/ring_buffer.c \
                    shm_utils.c \
                    $(LEGACY_RADAE_SRCS)
LEGACY_EMBED_RAW = legacy_sbitx/embedded_raw.o
LEGACY_EMBED_MAP = legacy_sbitx/embedded.syms
LEGACY_EMBED_OBJ = legacy_sbitx/embedded_prefixed.o

$(LEGACY_EMBED_OBJ): $(LEGACY_SBITX_SRCS) $(LEGACY_GPIOLIB_SRCS) $(LEGACY_HDRS)
	$(CC) -r $(LEGACY_CFLAGS) $(LEGACY_SBITX_SRCS) $(LEGACY_GPIOLIB_SRCS) -o $(LEGACY_EMBED_RAW)
	nm -g --defined-only $(LEGACY_EMBED_RAW) | awk '$$3 != "" { print $$3 " legacy_sbitx_" $$3; }' > $(LEGACY_EMBED_MAP)
	$(OBJCOPY) --redefine-syms=$(LEGACY_EMBED_MAP) $(LEGACY_EMBED_RAW) $(LEGACY_EMBED_OBJ)

legacy_sbitx_controller: $(LEGACY_SBITX_SRCS) $(LEGACY_GPIOLIB_SRCS) include/sbitx_io.h include/radio_cmds.h
	$(CC) $(LEGACY_CFLAGS) $(LEGACY_SBITX_SRCS) $(LEGACY_GPIOLIB_SRCS) -o legacy_sbitx_controller $(LEGACY_LDFLAGS)

# ── install ─────────────────────────────────────────────────────
prefix     ?= /usr/local
sysconfdir ?= /etc

install: radio_daemon sbitx_client
	install -D -m 755 radio_daemon  $(DESTDIR)$(prefix)/bin/radio_daemon
	install -D -m 755 sbitx_client  $(DESTDIR)$(prefix)/bin/sbitx_client
	install -d $(DESTDIR)$(sysconfdir)/hermes
	test -f $(DESTDIR)$(sysconfdir)/hermes/radio.ini || \
	  install -m 644 config/radio.ini $(DESTDIR)$(sysconfdir)/hermes/radio.ini
	test -f $(DESTDIR)$(sysconfdir)/hermes/user.ini || \
	  install -m 644 config/user.ini $(DESTDIR)$(sysconfdir)/hermes/user.ini

# ── clean ───────────────────────────────────────────────────────
clean:
	rm -f radio_daemon sbitx_client legacy_sbitx_controller $(DAEMON_OBJS) \
	      $(LEGACY_EMBED_RAW) $(LEGACY_EMBED_MAP) $(LEGACY_EMBED_OBJ)
