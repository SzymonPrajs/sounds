CC := xcrun clang

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS ?= -std=c11 -O3 $(WARNINGS) -Iinclude
TEST_CFLAGS ?= -std=c11 -O2 $(WARNINGS) -Iinclude

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

RECORDER := bin/mic-hal-vdsp
LIVE := bin/live-sound
SCOPE := bin/soundscope
TEST_ANALYSIS := build/test-analysis
TEST_RING_BUFFER := build/test-ring-buffer
TEST_COLORMAP := build/test-colormap

RECORDER_OBJECTS := \
	build/main.o \
	build/capture.o \
	build/analysis.o \
	build/args.o \
	build/error.o

LIVE_OBJECTS := \
	build/live_spectrogram.o \
	build/capture.o \
	build/ring_buffer.o \
	build/analysis.o \
	build/args.o \
	build/colormap.o \
	build/error.o

SCOPE_OBJECTS := \
	build/soundscope.o \
	build/capture.o \
	build/ring_buffer.o \
	build/analysis.o \
	build/args.o \
	build/colormap.o \
	build/error.o

.PHONY: all test clean

all: $(RECORDER) $(LIVE) $(SCOPE)

$(RECORDER): $(RECORDER_OBJECTS) | bin
	$(CC) $(RECORDER_OBJECTS) -framework CoreAudio -framework Accelerate -o $@

$(LIVE): $(LIVE_OBJECTS) | bin
	$(CC) $(LIVE_OBJECTS) -framework CoreAudio -framework Accelerate -o $@

$(SCOPE): $(SCOPE_OBJECTS) | bin
	$(CC) $(SCOPE_OBJECTS) -framework CoreAudio -framework Accelerate $(SDL_LIBS) -o $@

build/soundscope.o: src/soundscope.c | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_ANALYSIS): tests/test_analysis.c src/analysis.c src/error.c | build
	$(CC) $(TEST_CFLAGS) tests/test_analysis.c src/analysis.c src/error.c \
		-framework Accelerate -o $@

$(TEST_RING_BUFFER): tests/test_ring_buffer.c src/ring_buffer.c src/error.c | build
	$(CC) $(TEST_CFLAGS) tests/test_ring_buffer.c src/ring_buffer.c src/error.c -o $@

$(TEST_COLORMAP): tests/test_colormap.c src/colormap.c | build
	$(CC) $(TEST_CFLAGS) tests/test_colormap.c src/colormap.c -o $@

test: $(TEST_ANALYSIS) $(TEST_RING_BUFFER) $(TEST_COLORMAP)
	$(TEST_ANALYSIS)
	$(TEST_RING_BUFFER)
	$(TEST_COLORMAP)

bin build:
	mkdir -p $@

clean:
	rm -rf bin build
