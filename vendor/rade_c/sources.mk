RADE_C_EMBED_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

RADE_C_EMBED_CPPFLAGS += -I$(RADE_C_EMBED_DIR)/src -I$(RADE_C_EMBED_DIR)/support
RADE_C_EMBED_CFLAGS +=
RADE_C_EMBED_SRCS += \
$(RADE_C_EMBED_DIR)/src/rade_api.c \
$(RADE_C_EMBED_DIR)/src/rade_nnet.c \
$(RADE_C_EMBED_DIR)/src/rade_enc.c \
$(RADE_C_EMBED_DIR)/src/rade_enc_data.c \
$(RADE_C_EMBED_DIR)/src/rade_dec.c \
$(RADE_C_EMBED_DIR)/src/rade_dec_data.c \
$(RADE_C_EMBED_DIR)/src/rade_dsp.c \
$(RADE_C_EMBED_DIR)/src/rade_ofdm.c \
$(RADE_C_EMBED_DIR)/src/rade_bpf.c \
$(RADE_C_EMBED_DIR)/src/rade_acq.c \
$(RADE_C_EMBED_DIR)/src/rade_tx.c \
$(RADE_C_EMBED_DIR)/src/rade_rx.c \
$(RADE_C_EMBED_DIR)/src/rade_enc_v2.c \
$(RADE_C_EMBED_DIR)/src/rade_enc_v2_data.c \
$(RADE_C_EMBED_DIR)/src/rade_dec_v2.c \
$(RADE_C_EMBED_DIR)/src/rade_dec_v2_data.c \
$(RADE_C_EMBED_DIR)/src/rade_sync.c \
$(RADE_C_EMBED_DIR)/src/rade_sync_data.c \
$(RADE_C_EMBED_DIR)/src/rade_v2_ofdm.c \
$(RADE_C_EMBED_DIR)/src/rade_tx_v2.c \
$(RADE_C_EMBED_DIR)/src/rade_rx_v2.c
