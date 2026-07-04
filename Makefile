CC := xcrun clang

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS ?= -std=c11 -O3 $(WARNINGS) -Iinclude

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

APP := bin/sounds
TEST_APP := bin/analysis_test
SPECTRUM_TEST_APP := bin/spectrum_test

OBJECTS := \
	build/app/main.o \
	build/app/app_mode.o \
	build/app/recording.o \
	build/audio/capture.o \
	build/audio/ring_buffer.o \
	build/analysis/engine.o \
	build/analysis/algorithm.o \
	build/analysis/transient.o \
	build/analysis/tonal.o \
	build/analysis/room_decay.o \
	build/analysis/wavelet.o \
	build/analysis/spectrum.o \
	build/ui/window.o \
	build/ui/render.o \
	build/ui/font.o \
	build/support/colormap.o \
	build/support/error.o

.PHONY: all clean test

all: $(APP)

$(APP): $(OBJECTS) | bin
	$(CC) $(OBJECTS) -framework CoreAudio -framework Accelerate $(SDL_LIBS) -o $@

test: $(TEST_APP) $(SPECTRUM_TEST_APP)
	$(TEST_APP)
	$(SPECTRUM_TEST_APP)

$(TEST_APP): build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o | bin
	$(CC) build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o -framework Accelerate -o $@

$(SPECTRUM_TEST_APP): build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o | bin
	$(CC) build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o -framework Accelerate -o $@

build/ui/%.o: src/ui/%.c | build
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

build/tests/%.o: tests/%.c | build
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c | build
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

bin build:
	mkdir -p $@

clean:
	rm -rf bin build
