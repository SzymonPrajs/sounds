CC := xcrun clang

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
OPTFLAGS ?= -O3 -flto
CFLAGS ?= -std=c11 $(OPTFLAGS) $(WARNINGS) -Iinclude
LDFLAGS ?= -flto -Wl,-dead_strip

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

APP := bin/sounds
TEST_APP := bin/analysis_test
SPECTRUM_TEST_APP := bin/spectrum_test
RECORDING_TEST_APP := bin/recording_test
PLAYBACK_TEST_APP := bin/playback_test
BAND_RENDER_TEST_APP := bin/band_render_test

OBJECTS := \
	build/app/main.o \
	build/app/app_mode.o \
	build/app/clip.o \
	build/app/recording.o \
	build/app/settings.o \
	build/app/workbench.o \
	build/app/workspace.o \
	build/audio/capture.o \
	build/audio/playback.o \
	build/audio/ring_buffer.o \
	build/analysis/engine.o \
	build/analysis/algorithm.o \
	build/analysis/band_render.o \
	build/analysis/offline_spectrum.o \
	build/analysis/transient.o \
	build/analysis/tonal.o \
	build/analysis/spectral_mode.o \
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
	$(CC) $(LDFLAGS) $(OBJECTS) -framework CoreAudio -framework Accelerate $(SDL_LIBS) -o $@

test: $(TEST_APP) $(SPECTRUM_TEST_APP) $(RECORDING_TEST_APP) $(PLAYBACK_TEST_APP) $(BAND_RENDER_TEST_APP)
	$(TEST_APP)
	$(SPECTRUM_TEST_APP)
	$(RECORDING_TEST_APP)
	$(PLAYBACK_TEST_APP)
	$(BAND_RENDER_TEST_APP)

$(TEST_APP): build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o -framework Accelerate -o $@

$(SPECTRUM_TEST_APP): build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o -framework Accelerate -o $@

$(RECORDING_TEST_APP): build/tests/recording_test.o build/app/recording.o build/audio/ring_buffer.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/recording_test.o build/app/recording.o build/audio/ring_buffer.o build/support/error.o -o $@

$(PLAYBACK_TEST_APP): build/tests/playback_test.o build/audio/playback.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/playback_test.o build/audio/playback.o build/support/error.o -framework CoreAudio -o $@

$(BAND_RENDER_TEST_APP): build/tests/band_render_test.o build/analysis/band_render.o build/analysis/offline_spectrum.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/band_render_test.o build/analysis/band_render.o build/analysis/offline_spectrum.o build/support/error.o -framework Accelerate -o $@

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
