# hermes-radio-daemon — unified HERMES radio control daemon
#
# Copyright (C) 2024-2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC      = gcc

# Sources are grouped by module, one directory each (as in mercury), and
# every module directory is on the include path, so #include "x.h" finds a
# header wherever its module lives:
#   radio/               the radio: core, backend dispatch, controls, pipeline, media
#   hamlib/  sbitx/      the two radio backends
#   dsp/                 DSP, digital modes and voice codecs
#   modem/               radio <-> data modem audio (modem_audio + transports)
#   audioio/             operator audio: browser bridge, headset
#   control_interfaces/  CAT (emulated), rigctld, SHM control servers
#   gui_interface/       websocket UI (radio_websocket + websocket/ server), web/
#   common/              configuration and shared-memory helpers
#   include/             public protocol headers (SHM command set)
#   utils/               radio_client, ws_client, D-STAR/AMBE and RADE tools
MODULE_INCLUDES = -Iradio -Imodem -Iaudioio -Icontrol_interfaces -Igui_interface -Iapi -Igps -Ivendor/hermes_log \
                  -Icommon -Ihamlib -Iinclude

# -MMD -MP auto-generates per-object header dependencies (*.d), so editing a
# shared header (e.g. radio/radio.h) rebuilds every object that includes it. Without
# this, changing a struct layout left stale objects with the old layout — fields
# read at mismatched offsets, corrupting memory (a very painful class of bug).
CFLAGS  = -Ofast -Wall -std=gnu11 -fstack-protector -MMD -MP \
          -I. $(MODULE_INCLUDES) -I/usr/include/iniparser -I/usr/include/csdr \
          -Wno-deprecated-declarations
LDFLAGS = -liniparser -lhamlib -lasound -lcrypto -lssl -lfftw3f -lfftw3 \
          -lpthread -lm -li2c -lcsdr -lspecbleach -lcw -lrt -lmbe-neo

# The in-tree gui_interface/websocket/ module serves as the transport in gui_interface/radio_websocket.c;
# wss:// is provided by OpenSSL.
CFLAGS += -DWS_HAVE_OPENSSL=1

include vendor/rade_c/sources.mk
include vendor/opus_dnn/sources.mk
include vendor/ft8_lib/sources.mk
include vendor/minimodem/sources.mk

uname_p := $(shell uname -m)
ifeq ($(uname_p),aarch64)
	CFLAGS += -moutline-atomics -march=armv8-a+crc
else
	CFLAGS += -march=x86-64-v2
endif

EXTRA_CPPFLAGS = $(FT8_LIB_CPPFLAGS) $(MM_FSK_CPPFLAGS)
EXTRA_CFLAGS   = $(FT8_LIB_CFLAGS)   $(MM_FSK_CFLAGS)

RADE_C_EMBED_OBJS = $(RADE_C_EMBED_SRCS:.c=.o)
OPUS_DNN_OBJS     = $(OPUS_DNN_SRCS:.c=.o)
FT8_LIB_OBJS      = $(FT8_LIB_SRCS:.c=.o)
MM_FSK_OBJS       = $(MM_FSK_SRCS:.c=.o)

.PHONY: all clean install test go-test integration-test check-symbols FORCE

all: radio_daemon radio_client

# ── daemon-level objects ────────────────────────────────────────
DAEMON_TOP_OBJS = radio_daemon.o \
                  radio/radio_backend.o \
                  radio/radio_controls.o \
                  radio/radio_daemon_core.o \
                  radio/radio_pipeline.o \
                  hamlib/radio_hamlib.o \
                  hamlib/hamlib_digi.o \
                  control_interfaces/rig_server.o \
                  control_interfaces/cat_server.o \
                  radio/radio_media.o \
                  control_interfaces/radio_shm.o \
                  gui_interface/radio_websocket.o \
                  gui_interface/ui_hub.o \
                  api/radiod_api.o \
                  audioio/audio_bridge.o \
                  dsp/stream_resampler.o \
                  dsp/digi_log.o \
                  audioio/audio_headset.o \
                  modem/shm_audio.o \
                  modem/loop_audio.o \
                  modem/rtp_audio.o \
                  modem/modem_audio.o \
                  vendor/hermes_shm/ring_buffer_posix.o \
                  vendor/hermes_shm/shm_posix.o \
                  common/cfg_utils.o \
                  common/shm_utils.o \
                  common/radiod_log.o \
                  gps/gps.o \
                  gui_interface/websocket/ws_server.o \
                  gui_interface/websocket/ws_frame.o \
                  gui_interface/websocket/ws_crypto.o \
                  gui_interface/websocket/ws_tls.o

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
             dsp/radae_vocoder.o \
             dsp/sbitx_drm.o \
             dsp/sbitx_ft8.o \
             dsp/ft8_js8.o \
             dsp/ft8_subtract.o \
             dsp/sbitx_cw.o \
             dsp/sbitx_rtty.o \
             dsp/sbitx_dstar.o \
             dsp/dstar_voice.o \
             dsp/voice_crypto.o \
             dsp/rade_voice.o \
             dsp/mic_filter.o \
             sbitx/sbitx_si5351.o \
             sbitx/ring_buffer.o \
             $(SBITX_GPIOLIB_OBJS) \
             $(RADE_C_EMBED_OBJS) \
             $(OPUS_DNN_OBJS) \
             $(FT8_LIB_OBJS) \
             $(MM_FSK_OBJS)

# ── mercury, linked in (modem/mercury_link.c) ──────────────────
# vendor/mercury is the Rhizomatica/mercury submodule (host API in
# host/mercury_host.h); it is built there with MERCURY_HOST=daemon, which
# leaves out mercury's own audio, PTT, websocket server, iniparser and
# hermes_shm (the daemon has them).  With the submodule not checked out the
# daemon builds as before, and refuses main:enable_mercury at startup.
MERCURY_DIR  ?= vendor/mercury
HAVE_MERCURY ?= $(if $(wildcard $(MERCURY_DIR)/host/mercury_host.h),1,0)
ifeq ($(HAVE_MERCURY),1)
MERCURY_LIBS ?= $(MERCURY_DIR)/libmercury_host.a $(MERCURY_DIR)/modem/freedv/libfreedvdata.a
CFLAGS          += -DHAVE_MERCURY=1
DAEMON_TOP_OBJS += modem/mercury_link.o
modem/mercury_link.o: EXTRA_CPPFLAGS += -I$(MERCURY_DIR)/host
api/radiod_api.o: EXTRA_CPPFLAGS += -I$(MERCURY_DIR)/host
# One kiss_fft in the binary: freedv's (same structs, MAXFACTORS and float
# scalar as ft8_lib's).  The FT8 test links the same copy.
FT8_LIB_OBJS := $(filter-out %/fft/kiss_fft.o %/fft/kiss_fftr.o,$(FT8_LIB_OBJS))
FT8_TEST_SRCS = $(filter-out %/fft/kiss_fft.c %/fft/kiss_fftr.c,$(FT8_LIB_SRCS))
FT8_TEST_LIBS = $(MERCURY_DIR)/modem/freedv/libfreedvdata.a
$(MERCURY_LIBS) &: FORCE
	$(MAKE) -C $(MERCURY_DIR) libmercury_host.a MERCURY_HOST=daemon
else
FT8_TEST_SRCS = $(FT8_LIB_SRCS)
FT8_TEST_LIBS =
# The logger is mercury's hermes_log. With mercury linked in, its archive
# has it; without, the identical copy in vendor/hermes_log.
DAEMON_TOP_OBJS += vendor/hermes_log/hermes_log.o
endif

# ── Go API layer (api), linked in as a C archive ──────
# The /api/v2 REST API and the hermes.v1 websocket: radiod's C server owns
# the sockets and calls the Go handlers (docs/API-WRITERS.md).  Built when
# `go` is on the PATH (go.mod names the toolchain, 1.27.2); without it the
# daemon builds as before and answers /api/v2 with 503.
GO          ?= go
GO_API_DIR   = api
GO_API_LIB   = $(GO_API_DIR)/libhermes_api.a
HAVE_GO_API ?= $(if $(shell command -v $(GO) 2>/dev/null),1,0)
ifeq ($(HAVE_GO_API),1)
CFLAGS += -DHAVE_GO_API=1
GO_API_SRCS := $(shell find $(GO_API_DIR) -name '*.go' -o -name '*.yaml' -o -name '*.sql' -o -name go.mod -o -name go.sum)
$(GO_API_LIB) $(GO_API_DIR)/libhermes_api.h &: $(GO_API_SRCS) api/radiod_api.h vendor/hermes_log/hermes_log.h
	cd $(GO_API_DIR) && CGO_ENABLED=1 CC="$(CC)" $(GO) build -tags radiod -buildmode=c-archive \
	    -o libhermes_api.a ./cmd/hermesapi
gui_interface/radio_websocket.o: $(GO_API_DIR)/libhermes_api.h
endif

# The C API reports the commit it was built from; the stamp changes only
# with the commit, so the object is rebuilt then and only then.
RADIOD_GIT_HASH := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
ifneq ($(shell cat .radiod_version 2>/dev/null),$(RADIOD_GIT_HASH))
$(shell echo $(RADIOD_GIT_HASH) > .radiod_version)
endif
api/radiod_api.o: .radiod_version
api/radiod_api.o: EXTRA_CPPFLAGS += -DRADIOD_GIT_HASH=\"$(RADIOD_GIT_HASH)\"

DAEMON_OBJS = $(DAEMON_TOP_OBJS) $(SBITX_OBJS)

# Mercury's archives after the daemon's objects: they use its hermes_shm.
DAEMON_LIBS = $(if $(filter 1,$(HAVE_MERCURY)),$(MERCURY_LIBS)) $(if $(filter 1,$(HAVE_GO_API)),$(GO_API_LIB))
radio_daemon: $(DAEMON_OBJS) $(DAEMON_LIBS)
	$(CC) -o radio_daemon $(DAEMON_OBJS) $(DAEMON_LIBS) $(LDFLAGS)

# A library linked into the daemon must not define a global the daemon also
# defines, nor one of the shared-library symbols it uses: neither is a link
# error, one side just silently runs the other's code.  MERCURY_LIBS names
# the archives to check (utils/check_symbols.sh); with vendor/mercury it
# defaults to mercury's.
#
# Mercury's archive leaves out its hermes_shm rings and links against the
# daemon's (vendor/hermes_shm), but its objects were compiled against its
# own copy of the headers: the two copies must stay identical, or the ring
# structs differ with no error at all.
HERMES_SHM_FILES = ring_buffer_posix.c ring_buffer_posix.h shm_posix.c shm_posix.h
# Likewise the logger: radiod compiles against vendor/hermes_log/hermes_log.h
# and, with mercury, links mercury's hermes_log.o, so the copies must match.
HERMES_LOG_FILES = hermes_log.c hermes_log.h
check-symbols: $(DAEMON_OBJS) $(DAEMON_LIBS)
	utils/check_symbols.sh "$(DAEMON_OBJS)" "$(LDFLAGS)" $(MERCURY_LIBS) \
	    $(if $(filter 1,$(HAVE_GO_API)),$(GO_API_LIB))
ifeq ($(HAVE_MERCURY),1)
	@for f in $(HERMES_SHM_FILES); do \
	    cmp -s vendor/hermes_shm/$$f $(MERCURY_DIR)/common/$$f || \
	        { echo "check-symbols: vendor/hermes_shm/$$f differs from $(MERCURY_DIR)/common/$$f" >&2; exit 1; }; \
	done; echo "check-symbols: hermes_shm matches mercury's"
	@for f in $(HERMES_LOG_FILES); do \
	    cmp -s vendor/hermes_log/$$f $(MERCURY_DIR)/common/$$f || \
	        { echo "check-symbols: vendor/hermes_log/$$f differs from $(MERCURY_DIR)/common/$$f" >&2; exit 1; }; \
	done; echo "check-symbols: hermes_log matches mercury's"
endif

# RADE: rade_c and the Opus DNN subset it runs on (LPCNet features, FARGAN)
# see their own include paths and config.h only, not ft8_lib's or
# minimodem's: some Opus header names (kiss_fft.h, common.h) exist in ft8_lib
# too. Opus is built as Opus builds itself, -O2 without -ffast-math.
RADE_OBJS = $(RADE_C_EMBED_OBJS) $(OPUS_DNN_OBJS) dsp/sbitx_radae.o dsp/radae_vocoder.o
$(RADE_OBJS): EXTRA_CPPFLAGS := $(RADE_C_EMBED_CPPFLAGS) $(OPUS_DNN_CPPFLAGS)
$(RADE_OBJS): EXTRA_CFLAGS := $(RADE_C_EMBED_CFLAGS)
$(OPUS_DNN_OBJS): CFLAGS := $(filter-out -Ofast,$(CFLAGS)) -O2

# Generic compile rule. Sbitx hw/dsp need extra include paths for csdr,
# vendored rade_c/ft8_lib/minimodem.
%.o: %.c
	$(CC) -c $(CFLAGS) $(EXTRA_CPPFLAGS) $(EXTRA_CFLAGS) \
	      -Isbitx -Idsp -Isbitx/gpiolib \
	      $< -o $@

# ── client ──────────────────────────────────────────────────────
radio_client: utils/sbitx_client.c utils/sbitx_io.c common/shm_utils.c common/help.h \
              include/sbitx_io.h include/radio_cmds.h
	$(CC) $(CFLAGS) utils/sbitx_client.c utils/sbitx_io.c common/shm_utils.c \
	      -o radio_client -lpthread

# ── websocket control CLI, a testing aid (not built by "all") ──
# Uses the same in-tree frame codec as the server (ws_frame/ws_crypto) plus
# OpenSSL directly for the wss:// client handshake.
WS_CLIENT_SRCS = utils/ws_client.c gui_interface/websocket/ws_frame.c gui_interface/websocket/ws_crypto.c
ws_client: $(WS_CLIENT_SRCS) gui_interface/websocket/ws_frame.h gui_interface/websocket/ws_crypto.h
	$(CC) $(CFLAGS) $(WS_CLIENT_SRCS) -o ws_client -lssl -lcrypto

# ── D-STAR voice encryption through the GMSK modem and a simulated FM
# channel, a testing aid (not built by "all"): utils/dstar_crypt_sim.c ──
DSTAR_CRYPT_SIM_OBJS = utils/dstar_crypt_sim.o dsp/dstar_voice.o dsp/voice_crypto.o \
                       dsp/sbitx_dstar.o vendor/hermes_log/hermes_log.o
dstar_crypt_sim: $(DSTAR_CRYPT_SIM_OBJS)
	$(CC) $(CFLAGS) $(DSTAR_CRYPT_SIM_OBJS) -o $@ -lmbe-neo -lcrypto -lpthread -lm

# ── RADE V2 voice scrambling over a simulated HF channel, a testing aid
# (not built by "all"): utils/rade_crypt_sim.c ──
RADE_CRYPT_SIM_OBJS = utils/rade_crypt_sim.o dsp/rade_voice.o dsp/voice_crypto.o \
                      dsp/radae_vocoder.o $(RADE_C_EMBED_OBJS) $(OPUS_DNN_OBJS) \
                      vendor/hermes_log/hermes_log.o
utils/rade_crypt_sim.o: EXTRA_CPPFLAGS := $(RADE_C_EMBED_CPPFLAGS) $(OPUS_DNN_CPPFLAGS)
rade_crypt_sim: $(RADE_CRYPT_SIM_OBJS)
	$(CC) $(CFLAGS) $(RADE_CRYPT_SIM_OBJS) -o $@ -lcrypto -lpthread -lm

# ── unit tests (tests/tests.mk: make test, make test SANITIZE=asan|tsan) ──
include tests/tests.mk

# ── install ─────────────────────────────────────────────────────
prefix     ?= /usr
sysconfdir ?= /etc

install: radio_daemon radio_client
	install -D -m 755 radio_daemon  $(DESTDIR)$(prefix)/bin/radio_daemon
	install -D -m 755 radio_client  $(DESTDIR)$(prefix)/bin/radio_client
	install -D -m 644 radiod.service $(DESTDIR)/etc/systemd/system/radiod.service
	# radio_client answers to sbitx_client too. Forced: installing this
	# controller replaces hermes-net's sbitx_client binary, just as
	# hermes-net's install replaces this link (radio_client is untouched).
	ln -sf radio_client $(DESTDIR)$(prefix)/bin/sbitx_client
	install -d $(DESTDIR)$(sysconfdir)/hermes
	test -f $(DESTDIR)$(sysconfdir)/hermes/core.ini || \
	  install -m 644 config/core.ini $(DESTDIR)$(sysconfdir)/hermes/core.ini
	test -f $(DESTDIR)$(sysconfdir)/hermes/user.ini || \
	  install -m 644 config/user.ini $(DESTDIR)$(sysconfdir)/hermes/user.ini
	install -d $(DESTDIR)$(sysconfdir)/hermes/web
	install -m 644 gui_interface/web/index.html $(DESTDIR)$(sysconfdir)/hermes/web/index.html
	install -m 644 gui_interface/web/spectrum.html $(DESTDIR)$(sysconfdir)/hermes/web/spectrum.html
	install -D -m 644 config/avahi/hermes-radio.service \
	  $(DESTDIR)$(sysconfdir)/hermes/avahi/hermes-radio.service
	# One radio controller per station: on a real install (not a package
	# staging DESTDIR, and only under systemd), hand the radio over from
	# hermes-net's sbitx controller to radiod (restart: a reinstall runs the
	# new binary; ExecStopPost --ptt-off unkeys on the way down). hermes-net's
	# take_over_radio.sh does the mirror image.
	# hermes-net's hermes-radio.service alias names the station's controller;
	# the modem is PartOf it, so restarting radiod restarts the modem after it.
	if [ -z "$(DESTDIR)" ] && [ -d /run/systemd/system ]; then \
	  [ -x /usr/lib/hermes-net/set_radio_controller.sh ] && \
	    /usr/lib/hermes-net/set_radio_controller.sh radiod.service || true; \
	  systemctl daemon-reload || true; \
	  systemctl disable --now sbitx.service || true; \
	  systemctl enable radiod.service || true; \
	  systemctl restart radiod.service || true; \
	fi

# ── clean ───────────────────────────────────────────────────────
clean:
	rm -f radio_daemon radio_client ws_client dstar_crypt_sim rade_crypt_sim \
	      utils/dstar_crypt_sim.[od] utils/rade_crypt_sim.[od] \
	      $(DAEMON_OBJS) $(DAEMON_OBJS:.o=.d) $(TEST_BINS) $(TEST_STAMP) \
	      $(GO_API_LIB) $(GO_API_DIR)/libhermes_api.h .radiod_version

# Auto-generated header dependencies (from -MMD). Hyphen: ignore on first build.
-include $(DAEMON_OBJS:.o=.d) $(RADE_CRYPT_SIM_OBJS:.o=.d) $(DSTAR_CRYPT_SIM_OBJS:.o=.d)
