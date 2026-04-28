# hermes-radio-daemon – Hamlib-based HERMES radio control daemon
#
# Copyright (C) 2024-2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC      = gcc
CFLAGS  = -Ofast -Wall -std=gnu11 -fstack-protector \
          -I/usr/include/iniparser -Iinclude
LDFLAGS = -liniparser -lhamlib -lasound -lcrypto -lfftw3f -lpthread -lm

# Detect architecture for tuning flags
uname_p := $(shell uname -m)
ifeq ($(uname_p),aarch64)
	CFLAGS += -moutline-atomics -march=armv8-a+crc
else
	CFLAGS += -march=x86-64-v2
endif

.PHONY: all clean install

all: radio_daemon sbitx_client

# ── daemon ──────────────────────────────────────────────────────
DAEMON_OBJS = radio_daemon.o \
              radio_hamlib.o \
              radio_media.o  \
              radio_shm.o    \
              radio_websocket.o \
              cfg_utils.o    \
              shm_utils.o

radio_daemon: $(DAEMON_OBJS)
	$(CC) -o radio_daemon $(DAEMON_OBJS) $(LDFLAGS)

radio_daemon.o: radio_daemon.c radio.h radio_hamlib.h radio_media.h \
                radio_shm.h radio_websocket.h cfg_utils.h
	$(CC) -c $(CFLAGS) radio_daemon.c -o radio_daemon.o

radio_hamlib.o: radio_hamlib.c radio_hamlib.h radio.h cfg_utils.h
	$(CC) -c $(CFLAGS) radio_hamlib.c -o radio_hamlib.o

radio_media.o: radio_media.c radio_media.h radio.h
	$(CC) -c $(CFLAGS) radio_media.c -o radio_media.o

radio_shm.o: radio_shm.c radio_shm.h radio.h radio_hamlib.h \
             include/sbitx_io.h include/radio_cmds.h shm_utils.h
	$(CC) -c $(CFLAGS) radio_shm.c -o radio_shm.o

radio_websocket.o: radio_websocket.c radio_websocket.h radio.h \
                   radio_hamlib.h radio_media.h
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
	rm -f radio_daemon sbitx_client $(DAEMON_OBJS)
