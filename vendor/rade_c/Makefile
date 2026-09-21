include sources.mk

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS += $(RADE_C_EMBED_CPPFLAGS)
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter $(RADE_C_EMBED_CFLAGS)
LDLIBS += -lm

OBJS := $(patsubst $(RADE_C_EMBED_DIR)/%.c,build/%.o,$(RADE_C_EMBED_SRCS))
LIB := build/librade_c_embed.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

build/%.o: $(RADE_C_EMBED_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean
