OPUS_DNN_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

OPUS_DNN_CPPFLAGS += -DHAVE_CONFIG_H -I$(OPUS_DNN_DIR) -I$(OPUS_DNN_DIR)/include \
                     -I$(OPUS_DNN_DIR)/celt -I$(OPUS_DNN_DIR)/dnn
OPUS_DNN_SRCS += \
$(OPUS_DNN_DIR)/celt_fatal.c \
$(OPUS_DNN_DIR)/celt/celt_lpc.c \
$(OPUS_DNN_DIR)/celt/kiss_fft.c \
$(OPUS_DNN_DIR)/celt/pitch.c \
$(OPUS_DNN_DIR)/dnn/burg.c \
$(OPUS_DNN_DIR)/dnn/fargan.c \
$(OPUS_DNN_DIR)/dnn/fargan_data.c \
$(OPUS_DNN_DIR)/dnn/freq.c \
$(OPUS_DNN_DIR)/dnn/lpcnet_enc.c \
$(OPUS_DNN_DIR)/dnn/lpcnet_tables.c \
$(OPUS_DNN_DIR)/dnn/nnet.c \
$(OPUS_DNN_DIR)/dnn/nnet_default.c \
$(OPUS_DNN_DIR)/dnn/parse_lpcnet_weights.c \
$(OPUS_DNN_DIR)/dnn/pitchdnn.c \
$(OPUS_DNN_DIR)/dnn/pitchdnn_data.c
