# tests/tests.mk - radiod's unit tests, included by the top-level Makefile
#
# One binary per unit, tests/<module>/test_<unit>.c, on Unity
# (tests/unity) with fff (tests/fff) for fakes: the same framework as
# mercury's tests.  Each binary links the sources it tests directly, so a
# test needs nothing installed beyond the daemon's own libraries.  Run from
# the top of the tree (fixtures are tests/fixtures/...):
#
#   make test                  build and run them all
#   make test SANITIZE=asan    under AddressSanitizer + UndefinedBehaviorSanitizer
#   make test SANITIZE=tsan    under ThreadSanitizer
#   make tests/dsp/test_mic_filter && ./tests/dsp/test_mic_filter
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

UNITY_SRC = tests/unity/unity.c
# radiod's modules log through hermes_log (common/radiod_log.h): every test
# links the vendored copy. Nothing starts it, so what the code under test
# logs is dropped (hermes_log drops lines before hermes_log_init).
HERMES_LOG_SRC = vendor/hermes_log/hermes_log.c

SANITIZE ?=
ifeq ($(SANITIZE),asan)
SAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
else ifeq ($(SANITIZE),tsan)
SAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer
else ifneq ($(SANITIZE),)
$(error SANITIZE must be asan or tsan, not '$(SANITIZE)')
endif

TEST_CFLAGS = -O0 -g -Wall -Wextra -std=gnu11 -fstack-protector -pthread \
              -I. $(MODULE_INCLUDES) -I/usr/include/iniparser \
              -Itests -Itests/unity -Itests/fff -DUNITY_SUPPORT_64 -DUNITY_INCLUDE_DOUBLE \
              $(SAN_FLAGS)

# SANITIZE comes from the command line, which no prerequisite tracks: this
# stamp holds the value the binaries were built with and changes only with
# it, so switching rebuilds them instead of running stale ones.
TEST_STAMP := tests/.sanitize
ifneq ($(shell cat $(TEST_STAMP) 2>/dev/null),$(SANITIZE)-)
$(shell echo "$(SANITIZE)-" > $(TEST_STAMP))
endif

TEST_BINS = tests/radio/test_backend_selection tests/radio/test_compat_surface \
            tests/radio/test_controls tests/radio/test_dv_codec \
            tests/hamlib/test_hamlib_ptt tests/hamlib/test_hamlib_conf \
            tests/control_interfaces/test_rig_server \
            tests/gui_interface/test_ui_hub tests/gui_interface/test_ws_server \
            tests/modem/test_rtp_audio tests/modem/test_modem_audio \
            tests/audioio/test_audio_bridge \
            tests/dsp/test_upsample2 tests/dsp/test_stream_resampler tests/dsp/test_mic_filter \
            tests/dsp/test_digi_modes tests/dsp/test_digi_log tests/dsp/test_dstar_voice \
            tests/dsp/test_radae_vocoder \
            tests/sbitx/test_sbitx_buffer \
            tests/common/test_radiod_log

$(TEST_BINS): $(UNITY_SRC) $(HERMES_LOG_SRC) tests/unity/unity.h tests/test_util.h $(TEST_STAMP)

# The sanitizers' runtime options: stack traces for UBSan, and TSan's
# suppressions for races inside libraries (tests/sanitizer-suppressions).
TEST_ENV = UBSAN_OPTIONS=print_stacktrace=1 \
           TSAN_OPTIONS="suppressions=$(CURDIR)/tests/sanitizer-suppressions/tsan.supp second_deadlock_stack=1"

# Every binary runs, failing or not; the target fails if any did.
test: $(TEST_BINS)
	@failed=""; \
	for t in $(TEST_BINS); do \
	    echo "== $$t"; \
	    $(TEST_ENV) ./$$t || failed="$$failed $$t"; \
	done; \
	if [ -n "$$failed" ]; then echo "FAILED:$$failed"; exit 1; fi; \
	echo "== all $(words $(TEST_BINS)) test binaries passed"

# The Go API layer's unit tests (api): the stub build, no C,
# under the race detector.
# -race needs a 48-bit address space; on a Raspberry Pi kernel (39-bit VMA)
# run "make go-test GO_TEST_FLAGS=".
GO_TEST_FLAGS ?= -race
go-test:
	cd api && go test $(GO_TEST_FLAGS) -count=1 ./...

# Integration tests (tests/integration, Go): the real radio_daemon on
# Hamlib's dummy rig, driven over its websocket, rigctl and (with mercury
# linked in) mercury's TNC ports.  Needs go; go.mod names the toolchain.
integration-test: radio_daemon
	cd tests/integration && RADIOD_BIN=$(CURDIR)/radio_daemon go test -count=1 ./...

# ── radio/ ──────────────────────────────────────────────────────
tests/radio/test_backend_selection: tests/radio/test_backend_selection.c common/cfg_utils.c common/cfg_utils.h \
                                    radio/radio_backend.c radio/radio_backend.h radio/radio_daemon_core.h \
                                    hamlib/radio_hamlib.h radio/radio.h \
                                    tests/fixtures/backend-default.ini tests/fixtures/backend-zbitx.ini
	$(CC) $(TEST_CFLAGS) $< control_interfaces/rig_server.c radio/radio_controls.c control_interfaces/cat_server.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -liniparser -lpthread -lm

tests/radio/test_compat_surface: tests/radio/test_compat_surface.c control_interfaces/radio_shm.c control_interfaces/radio_shm.h \
                                 radio/radio_pipeline.c radio/radio_pipeline.h radio/radio_backend.h radio/radio.h \
                                 common/shm_utils.h include/sbitx_io.h include/radio_cmds.h
	$(CC) $(TEST_CFLAGS) $< $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread

tests/radio/test_controls: tests/radio/test_controls.c radio/radio_controls.c radio/radio_controls.h \
                           radio/radio_backend.c radio/radio_backend.h control_interfaces/cat_server.c \
                           control_interfaces/cat_server.h common/cfg_utils.c common/cfg_utils.h radio/radio.h
	$(CC) $(TEST_CFLAGS) $< control_interfaces/rig_server.c control_interfaces/cat_server.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -liniparser -lpthread -lm

tests/radio/test_dv_codec: tests/radio/test_dv_codec.c common/cfg_utils.c common/cfg_utils.h \
                           radio/radio_backend.c radio/radio_backend.h radio/radio.h
	$(CC) $(TEST_CFLAGS) $< control_interfaces/rig_server.c radio/radio_controls.c control_interfaces/cat_server.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -liniparser -lpthread -lm

# ── hamlib/ ─────────────────────────────────────────────────────
tests/hamlib/test_hamlib_ptt: tests/hamlib/test_hamlib_ptt.c hamlib/radio_hamlib.c control_interfaces/rig_server.c \
                              control_interfaces/rig_server.h radio/radio_controls.c radio/radio_backend.c \
                              radio/radio_backend.h common/cfg_utils.c radio/radio.h
	$(CC) $(TEST_CFLAGS) $< control_interfaces/rig_server.c control_interfaces/cat_server.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lhamlib -liniparser -lpthread -lm

tests/hamlib/test_hamlib_conf: tests/hamlib/test_hamlib_conf.c hamlib/hamlib_conf.h
	$(CC) $(TEST_CFLAGS) $< $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@

# ── control_interfaces/ ─────────────────────────────────────────
tests/control_interfaces/test_rig_server: tests/control_interfaces/test_rig_server.c control_interfaces/rig_server.c \
                                          control_interfaces/rig_server.h radio/radio_controls.c \
                                          radio/radio_backend.c common/cfg_utils.c radio/radio.h
	$(CC) $(TEST_CFLAGS) $< control_interfaces/rig_server.c control_interfaces/cat_server.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -liniparser -lpthread -lm

# ── gui_interface/ ──────────────────────────────────────────────
tests/gui_interface/test_ui_hub: tests/gui_interface/test_ui_hub.c gui_interface/ui_hub.c gui_interface/ui_hub.h
	$(CC) $(TEST_CFLAGS) $< gui_interface/ui_hub.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread

WS_SERVER_SRCS = gui_interface/websocket/ws_server.c gui_interface/websocket/ws_frame.c \
                 gui_interface/websocket/ws_crypto.c gui_interface/websocket/ws_tls.c
tests/gui_interface/test_ws_server: tests/gui_interface/test_ws_server.c $(WS_SERVER_SRCS) \
                                    gui_interface/websocket/ws_server.h
	$(CC) $(TEST_CFLAGS) -DWS_HAVE_OPENSSL=1 -Igui_interface $< $(WS_SERVER_SRCS) $(UNITY_SRC) $(HERMES_LOG_SRC) \
	      -o $@ -lssl -lcrypto -lpthread

# ── modem/ ──────────────────────────────────────────────────────
tests/modem/test_rtp_audio: tests/modem/test_rtp_audio.c modem/rtp_audio.c modem/rtp_audio.h \
                            modem/modem_audio.c modem/modem_audio.h radio/radio.h
	$(CC) $(TEST_CFLAGS) $< modem/rtp_audio.c modem/modem_audio.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread -lm

tests/modem/test_modem_audio: tests/modem/test_modem_audio.c modem/modem_audio.c modem/modem_audio.h radio/radio.h
	$(CC) $(TEST_CFLAGS) $< modem/modem_audio.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread -lm

# ── audioio/ ────────────────────────────────────────────────────
tests/audioio/test_audio_bridge: tests/audioio/test_audio_bridge.c audioio/audio_bridge.c audioio/audio_bridge.h radio/radio.h
	$(CC) $(TEST_CFLAGS) -I/usr/include/csdr $< audioio/audio_bridge.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lcsdr -lfftw3f -lm

# ── dsp/ ────────────────────────────────────────────────────────
tests/dsp/test_upsample2: tests/dsp/test_upsample2.c dsp/upsample2.c dsp/upsample2.h
	$(CC) $(TEST_CFLAGS) -I/usr/include/csdr $< dsp/upsample2.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lcsdr -lfftw3f -lm

tests/dsp/test_stream_resampler: tests/dsp/test_stream_resampler.c dsp/stream_resampler.c dsp/stream_resampler.h
	$(CC) $(TEST_CFLAGS) -I/usr/include/csdr $< dsp/stream_resampler.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lcsdr -lfftw3f -lm

tests/dsp/test_mic_filter: tests/dsp/test_mic_filter.c dsp/mic_filter.c dsp/mic_filter.h
	$(CC) $(TEST_CFLAGS) -Idsp $< dsp/mic_filter.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lm

tests/dsp/test_digi_modes: tests/dsp/test_digi_modes.c dsp/stream_resampler.c dsp/sbitx_cw.c dsp/sbitx_rtty.c \
                           dsp/sbitx_ft8.c dsp/sbitx_ft8.h dsp/digi_log.c dsp/digi_log.h \
                           $(FT8_TEST_SRCS) $(FT8_TEST_LIBS) $(MM_FSK_SRCS)
	$(CC) $(TEST_CFLAGS) -w -I/usr/include/csdr -Idsp $(FT8_LIB_CPPFLAGS) $(MM_FSK_CPPFLAGS) \
	      $< dsp/stream_resampler.c dsp/sbitx_cw.c dsp/sbitx_rtty.c dsp/sbitx_ft8.c dsp/digi_log.c \
	      $(FT8_TEST_SRCS) $(MM_FSK_SRCS) $(FT8_TEST_LIBS) $(UNITY_SRC) $(HERMES_LOG_SRC) \
	      -o $@ -lcsdr -lfftw3f -lfftw3 -lcw -lpthread -lm

# The logger: mercury's hermes_log (vendor/hermes_log) as core.ini sets it up.
tests/common/test_radiod_log: tests/common/test_radiod_log.c common/radiod_log.c common/radiod_log.h \
                              $(HERMES_LOG_SRC) vendor/hermes_log/hermes_log.h
	$(CC) $(TEST_CFLAGS) $< common/radiod_log.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread

tests/dsp/test_digi_log: tests/dsp/test_digi_log.c dsp/digi_log.c dsp/digi_log.h
	$(CC) $(TEST_CFLAGS) -Idsp $< dsp/digi_log.c $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread -lm

tests/dsp/test_dstar_voice: tests/dsp/test_dstar_voice.c dsp/dstar_voice.c dsp/dstar_voice.h \
                            dsp/voice_crypto.c dsp/voice_crypto.h dsp/sbitx_dstar.c dsp/sbitx_dstar.h
	$(CC) $(TEST_CFLAGS) -Idsp $< dsp/dstar_voice.c dsp/voice_crypto.c dsp/sbitx_dstar.c \
	      $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lmbe-neo -lcrypto -lm

# Links the daemon's own objects: the Opus subset needs its per-object flags.
tests/dsp/test_radae_vocoder: tests/dsp/test_radae_vocoder.c dsp/radae_vocoder.h dsp/radae_vocoder.o $(OPUS_DNN_OBJS)
	$(CC) $(TEST_CFLAGS) $< dsp/radae_vocoder.o $(OPUS_DNN_OBJS) $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lm

# ── sbitx/ ──────────────────────────────────────────────────────
tests/sbitx/test_sbitx_buffer: tests/sbitx/test_sbitx_buffer.c sbitx/sbitx_buffer.c sbitx/sbitx_buffer.h \
                               sbitx/ring_buffer.c sbitx/ring_buffer.h
	$(CC) $(TEST_CFLAGS) -Isbitx $< $(UNITY_SRC) $(HERMES_LOG_SRC) -o $@ -lpthread
