LLVM_PREFIX ?= /opt/homebrew/opt/llvm
CC := $(LLVM_PREFIX)/bin/clang

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
STDFLAGS ?= -std=c2y -fdefer-ts
LTOFLAGS ?= -flto=full
OPTFLAGS ?= -O3 $(LTOFLAGS) -mcpu=native -ffp-contract=fast -fstrict-aliasing -fvisibility=hidden -DNDEBUG
CFLAGS ?= $(STDFLAGS) $(OPTFLAGS) $(WARNINGS) -Iinclude
LDFLAGS ?= $(LTOFLAGS) -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

APP := bin/sounds
TEST_APP := bin/analysis_test
SPECTRUM_TEST_APP := bin/spectrum_test
RECORDING_TEST_APP := bin/recording_test
PLAYBACK_TEST_APP := bin/playback_test
BAND_RENDER_TEST_APP := bin/band_render_test
IMUI_TEST_APP := bin/imui_test

BAND_RENDER_OBJECTS := \
	build/analysis/band_render/core.o \
	build/analysis/band_render/fft.o \
	build/analysis/band_render/filter.o

UI_OBJECTS := \
	build/ui/window.o \
	build/ui/imui.o \
	build/ui/imui_sdl.o \
	build/ui/render/core.o \
	build/ui/render/spectrogram.o \
	build/ui/render/workspace.o \
	build/ui/render/overlay.o \
	build/ui/font.o

UI_SDL_OBJECTS := $(filter-out build/ui/imui.o,$(UI_OBJECTS))

WORKBENCH_OBJECTS := \
	build/app/workbench/actions.o \
	build/app/workbench/analysis.o \
	build/app/workbench/playback.o \
	build/app/workbench/recordings.o \
	build/app/workbench/scan.o \
	build/app/workbench/state.o \
	build/app/workbench/trim.o

OBJECTS := \
	build/main.o \
	build/app/app_mode.o \
	build/app/clip.o \
	build/app/recording.o \
	build/app/settings.o \
	$(WORKBENCH_OBJECTS) \
	build/app/workspace.o \
	build/audio/capture.o \
	build/audio/playback.o \
	build/audio/ring_buffer.o \
	build/analysis/engine.o \
	build/analysis/algorithm.o \
	$(BAND_RENDER_OBJECTS) \
	build/analysis/offline_spectrum.o \
	build/analysis/transient.o \
	build/analysis/tonal.o \
	build/analysis/spectral_mode.o \
	build/analysis/wavelet.o \
	build/analysis/spectrum.o \
	$(UI_OBJECTS) \
	build/support/colormap.o \
	build/support/frequency_band.o \
	build/support/error.o

.PHONY: all clean test

all: $(APP)

$(APP): $(OBJECTS) | bin
	$(CC) $(LDFLAGS) $(OBJECTS) -framework CoreAudio -framework Accelerate $(SDL_LIBS) -o $@

test: $(TEST_APP) $(SPECTRUM_TEST_APP) $(RECORDING_TEST_APP) $(PLAYBACK_TEST_APP) $(BAND_RENDER_TEST_APP) $(IMUI_TEST_APP)
	$(TEST_APP)
	$(SPECTRUM_TEST_APP)
	$(RECORDING_TEST_APP)
	$(PLAYBACK_TEST_APP)
	$(BAND_RENDER_TEST_APP)
	$(IMUI_TEST_APP)

$(TEST_APP): build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/analysis_test.o build/analysis/wavelet.o build/support/error.o -framework Accelerate -o $@

$(SPECTRUM_TEST_APP): build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/spectrum_test.o build/analysis/spectrum.o build/audio/ring_buffer.o build/support/error.o -framework Accelerate -o $@

$(RECORDING_TEST_APP): build/tests/recording_test.o build/app/recording.o build/audio/ring_buffer.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/recording_test.o build/app/recording.o build/audio/ring_buffer.o build/support/error.o -o $@

$(PLAYBACK_TEST_APP): build/tests/playback_test.o build/audio/playback.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/playback_test.o build/audio/playback.o build/support/error.o -framework CoreAudio -o $@

$(BAND_RENDER_TEST_APP): build/tests/band_render_test.o $(BAND_RENDER_OBJECTS) build/analysis/offline_spectrum.o build/support/error.o | bin
	$(CC) $(LDFLAGS) build/tests/band_render_test.o $(BAND_RENDER_OBJECTS) build/analysis/offline_spectrum.o build/support/error.o -framework Accelerate -o $@

$(IMUI_TEST_APP): build/tests/imui_test.o build/ui/imui.o | bin
	$(CC) $(LDFLAGS) build/tests/imui_test.o build/ui/imui.o -o $@

$(UI_SDL_OBJECTS): build/ui/%.o: src/ui/%.c | build
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
