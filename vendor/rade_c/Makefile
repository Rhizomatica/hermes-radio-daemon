include sources.mk
include ../opus_dnn/sources.mk

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS += $(RADE_C_EMBED_CPPFLAGS) $(OPUS_DNN_CPPFLAGS)
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter $(RADE_C_EMBED_CFLAGS)
LDLIBS += -lm

OBJS := $(patsubst $(RADE_C_EMBED_DIR)/%.c,build/%.o,$(RADE_C_EMBED_SRCS)) \
        $(patsubst $(OPUS_DNN_DIR)/%.c,build/opus_dnn/%.o,$(OPUS_DNN_SRCS))
LIB := build/librade_c_embed.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

build/opus_dnn/%.o: $(OPUS_DNN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/%.o: $(RADE_C_EMBED_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean
