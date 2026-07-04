CC := xcrun clang

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS ?= -std=c11 -O3 $(WARNINGS) -Iinclude

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

APP := bin/sounds
TEST_APP := bin/analysis_test

OBJECTS := \
	build/main.o \
	build/capture.o \
	build/ring_buffer.o \
	build/analysis.o \
	build/colormap.o \
	build/error.o

.PHONY: all clean test

all: $(APP)

$(APP): $(OBJECTS) | bin
	$(CC) $(OBJECTS) -framework CoreAudio -framework Accelerate $(SDL_LIBS) -o $@

test: $(TEST_APP)
	$(TEST_APP)

$(TEST_APP): build/analysis_test.o build/analysis.o build/error.o | bin
	$(CC) build/analysis_test.o build/analysis.o build/error.o -framework Accelerate -o $@

build/main.o: src/main.c | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

build/analysis_test.o: tests/analysis_test.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

bin build:
	mkdir -p $@

clean:
	rm -rf bin build
