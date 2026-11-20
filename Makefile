# Makefile - build rules for 0paint (c99 + sdl2)

# === compiler and flags ===
CC      ?= gcc
PREFIX  ?= /usr/local
TARGET  := 0paint

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null || echo "-lSDL2")

# keep it plain c99 so old gcc/clang and weird cpus build fine
CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter $(SDL_CFLAGS)
LDFLAGS += $(SDL_LIBS) -lm -ldl

# === sources ===
SRCS := \
	src/main.c \
	src/zp_raster.c \
	src/zp_ttf.c \
	src/zp_codec.c \
	src/zp_platform.c \
	src/zp_ui.c

OBJS := $(SRCS:.c=.o)

# === targets ===
.PHONY: all clean install uninstall strip

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

src/%.o: src/%.c src/zp_types.h src/zp_font.h src/zp_icons.h
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	strip --strip-unneeded $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	install -Dm644 0paint.desktop "$(DESTDIR)$(PREFIX)/share/applications/0paint.desktop"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/0paint.desktop"

clean:
	rm -f $(OBJS) $(TARGET)
