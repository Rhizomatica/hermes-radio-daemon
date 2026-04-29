include sources.mk

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS += $(RADEV2_EMBED_CPPFLAGS)
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter $(RADEV2_EMBED_CFLAGS)
LDLIBS += -lm

OBJS := $(patsubst $(RADEV2_EMBED_DIR)/%.c,build/%.o,$(RADEV2_EMBED_SRCS))
LIB := build/libradev2_embed.a

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

build/%.o: $(RADEV2_EMBED_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean
