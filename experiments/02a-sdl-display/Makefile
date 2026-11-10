# SPDX-License-Identifier: GPL-3.0-or-later
#
# Experiment 02a: SDL2 display + image + text.
#
#   make host        build for this machine (needs SDL2, SDL2_image, SDL2_ttf: brew/apt)
#   make run-host    headless logic test (SDL_VIDEODRIVER=dummy, 1.5 s)
#   make screenshot  host aid: render for 2 s with a real window, save frame as BMP
#   make ps5         cross-build the payload (needs PS5_PAYLOAD_SDK, host LLVM,
#                    and tools/fetch-pacbrew.sh <variant> already run)
#   make deploy      send the payload using .local-config/console.env
#
#   PACBREW=hwgl|swgl   which pacbrew bundle to link (default hwgl; see upstreams.lock)
#
# Assets (test image, DejaVu font) are generated/fetched at build time into
# $(BUILD_DIR)/assets and embedded into the binary; nothing is committed.
# The host build uses the system SDL2 and never feeds into a PS5 build.

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(HERE)/../..)
BUILD_DIR ?= $(REPO_ROOT)/build/experiments/02a-sdl-display
ASSETS    := $(BUILD_DIR)/assets
SRC       := $(HERE)/main.c

PACBREW ?= hwgl
PACBREW_KEY := $(if $(filter swgl,$(PACBREW)),PACBREW_SWGL,PACBREW_HWGL)
PACBREW_REL := $(shell sed -n 's/^$(PACBREW_KEY)_RELEASE=//p' $(REPO_ROOT)/upstreams.lock)
PS5_PACBREW_PREFIX ?= $(REPO_ROOT)/external/toolchains/pacbrew-$(PACBREW_REL)/user/homebrew

WARN_FLAGS    := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
CFLAGS_COMMON := -std=gnu11 -O2 -g $(WARN_FLAGS)

PS5_ELF  := $(BUILD_DIR)/ps5/ksre-ps5-02a-sdl-display.elf
HOST_BIN := $(BUILD_DIR)/host/ksre-ps5-02a-sdl-display

HOST_CC     ?= cc
SDL_CONFIG  ?= sdl2-config
HOST_CFLAGS  = $(shell $(SDL_CONFIG) --cflags)
HOST_LIBS    = $(shell $(SDL_CONFIG) --libs) -lSDL2_image -lSDL2_ttf -lm -pthread

# Bundle headers come BEFORE the SDK's so the bundle's SDL headers win.
PS5_INC := -I$(PS5_PACBREW_PREFIX)/include -I$(PS5_PACBREW_PREFIX)/include/SDL2 -D_REENTRANT -D_THREAD_SAFE
# Static link, dependency order. System libraries are the ones the bundle's
# sdl2-config --static-libs lists (SceVideoOut, ScePad, ...), provided by the SDK stubs.
PS5_LIBS := -L$(PS5_PACBREW_PREFIX)/lib \
	-lSDL2_ttf -lSDL2_image -lSDL2 \
	-lfreetype -lpng16 -lz -lbz2 -ljpeg -lwebpdemux -lwebp -lsharpyuv \
	-liconv -lsamplerate -lSceSystemService -lSceUserService -lScePad -lSceAudioOut \
	-lSceVideoOut -lSceKeyboard -lSceImeDialog -lm
PS5_EXTRA_FLAGS := -Wno-unknown-warning-option

IMAGE_PNG := $(ASSETS)/test-card.png
FONT_TTF  := $(ASSETS)/DejaVuSans.ttf
ASSETS_C  := $(ASSETS)/assets.c

.PHONY: all host run-host screenshot ps5 deploy clean check-sdk check-pacbrew assets

all: host

assets: $(ASSETS_C)

$(IMAGE_PNG): $(HERE)/tools/gen_image.py
	@mkdir -p $(ASSETS)
	python3 -I $< $@

$(FONT_TTF): $(HERE)/tools/fetch-font.sh $(REPO_ROOT)/upstreams.lock
	@mkdir -p $(ASSETS)
	$(HERE)/tools/fetch-font.sh $@

$(ASSETS_C): $(IMAGE_PNG) $(FONT_TTF) $(HERE)/tools/embed.py
	python3 -I $(HERE)/tools/embed.py $@ image=$(IMAGE_PNG) font=$(FONT_TTF)

host: $(HOST_BIN)

$(HOST_BIN): $(SRC) $(ASSETS_C)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) $(HOST_CFLAGS) -o $@ $(SRC) $(ASSETS_C) $(HOST_LIBS)

run-host: $(HOST_BIN)
	SDL_VIDEODRIVER=dummy $(HOST_BIN) --duration-ms 1500

screenshot: $(HOST_BIN)
	$(HOST_BIN) --duration-ms 2000 --screenshot $(BUILD_DIR)/host/frame.bmp

check-sdk:
	@if [ -z "$(PS5_PAYLOAD_SDK)" ]; then \
	  echo "PS5_PAYLOAD_SDK is not set. Run tools/fetch-sdk.sh and export the path it prints." >&2; exit 2; fi
	@if [ ! -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" ]; then \
	  echo "PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) does not look like a ps5-payload-sdk install." >&2; exit 2; fi

check-pacbrew:
	@if [ ! -f "$(PS5_PACBREW_PREFIX)/lib/libSDL2.a" ]; then \
	  echo "pacbrew bundle missing at $(PS5_PACBREW_PREFIX). Run: tools/fetch-pacbrew.sh $(PACBREW)" >&2; exit 2; fi

ps5: check-sdk check-pacbrew $(PS5_ELF)

$(PS5_ELF): $(SRC) $(ASSETS_C)
	@mkdir -p $(dir $@)
	"$(PS5_PAYLOAD_SDK)/bin/prospero-clang" $(CFLAGS_COMMON) $(PS5_EXTRA_FLAGS) $(PS5_INC) -o $@ $(SRC) $(ASSETS_C) $(PS5_LIBS)
	@echo "built $@"

deploy: ps5
	"$(REPO_ROOT)/tools/deploy.sh" "$(PS5_ELF)"

clean:
	rm -rf "$(BUILD_DIR)"
