RADEV2_EMBED_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

RADEV2_EMBED_CPPFLAGS += -I$(RADEV2_EMBED_DIR)/src -I$(RADEV2_EMBED_DIR)/support
RADEV2_EMBED_CFLAGS += -DHIGH_ACCURACY
RADEV2_EMBED_SRCS += \
$(RADEV2_EMBED_DIR)/src/rade_api_pure_c.c \
$(RADEV2_EMBED_DIR)/src/rade_nnet.c \
$(RADEV2_EMBED_DIR)/src/complex_bpf.c \
$(RADEV2_EMBED_DIR)/src/frame_sync.c \
$(RADEV2_EMBED_DIR)/src/frame_sync_data.c \
$(RADEV2_EMBED_DIR)/src/rade_dec.c \
$(RADEV2_EMBED_DIR)/src/rade_dec_data.c \
$(RADEV2_EMBED_DIR)/src/rade_enc.c \
$(RADEV2_EMBED_DIR)/src/rade_enc_data.c \
$(RADEV2_EMBED_DIR)/src/rx2_coarse_sync.c \
$(RADEV2_EMBED_DIR)/src/rx2_demod.c \
$(RADEV2_EMBED_DIR)/src/rx2_eoo.c \
$(RADEV2_EMBED_DIR)/src/rx2_frame_sync.c \
$(RADEV2_EMBED_DIR)/src/rx2_frontend.c \
$(RADEV2_EMBED_DIR)/src/rx2_model_data.c \
$(RADEV2_EMBED_DIR)/src/rx2_receiver.c \
$(RADEV2_EMBED_DIR)/src/tx2_encode.c \
$(RADEV2_EMBED_DIR)/src/tx2_model_data.c
