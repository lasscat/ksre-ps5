# SPDX-License-Identifier: GPL-3.0-or-later
#
# Experiment 02c: controller events, generated 440 Hz tone, storage round-trip.
#
#   make host           build for this machine (needs SDL2: brew/apt)
#   make run-host       headless logic run: dummy video/audio drivers + an SDL virtual gamepad
#   make ps5            cross-build the payload (needs PS5_PAYLOAD_SDK, host LLVM and
#                       tools/fetch-pacbrew.sh <variant> already run)
#   make deploy         upload the payload and run it as homebrew (websrv /hbldr), see below
#
#   PACBREW=swgl|hwgl   which pacbrew bundle to link against (default swgl = the bundle the
#                       project uses since 02b; the payload needs no Mesa)
#   ARGS="..."          payload arguments for deploy, e.g. ARGS="--window-ms 60000"
#
# The payload links SDL2 (static) and the SCE system libraries only. It is linked with
# -rdynamic and ends with _exit() like 02b (project convention).

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(HERE)/../..)
BUILD_DIR ?= $(REPO_ROOT)/build/experiments/02c-input-audio
SRC       := $(HERE)/main.c

PACBREW ?= swgl
PACBREW_KEY := $(if $(filter swgl,$(PACBREW)),PACBREW_SWGL,PACBREW_HWGL)
PACBREW_REL := $(shell sed -n 's/^$(PACBREW_KEY)_RELEASE=//p' $(REPO_ROOT)/upstreams.lock)
PS5_PACBREW_PREFIX ?= $(REPO_ROOT)/external/toolchains/pacbrew-$(PACBREW_REL)/user/homebrew
PS5_OUT := $(if $(filter swgl,$(PACBREW)),ps5,ps5-hwgl)

WARN_FLAGS    := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
CFLAGS_COMMON := -std=gnu11 -O2 -g $(WARN_FLAGS)

PS5_ELF  := $(BUILD_DIR)/$(PS5_OUT)/ksre-ps5-02c-input-audio.elf
HOST_BIN := $(BUILD_DIR)/host/ksre-ps5-02c-input-audio

HOST_CC     ?= cc
SDL_CONFIG  ?= sdl2-config
HOST_CFLAGS  = $(shell $(SDL_CONFIG) --cflags)
HOST_LIBS    = $(shell $(SDL_CONFIG) --libs) -lm -pthread

PS5_INC := -I$(PS5_PACBREW_PREFIX)/include -I$(PS5_PACBREW_PREFIX)/include/SDL2 -D_REENTRANT -D_THREAD_SAFE
PS5_LIBS := -L$(PS5_PACBREW_PREFIX)/lib \
	-lSDL2 -liconv -lsamplerate -lSceSystemService -lSceUserService -lScePad -lSceAudioOut \
	-lSceVideoOut -lSceKeyboard -lSceImeDialog -lm
PS5_EXTRA_FLAGS := -Wno-unknown-warning-option -rdynamic

ARGS ?=

.PHONY: all host run-host ps5 deploy clean check-sdk check-pacbrew

all: host

host: $(HOST_BIN)

$(HOST_BIN): $(SRC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) $(HOST_CFLAGS) -o $@ $(SRC) $(HOST_LIBS)

# Needs no display, audio device or pad: SDL's dummy drivers plus a virtual gamepad that
# presses every control, then holds L1+R1+Options.
run-host: $(HOST_BIN)
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(HOST_BIN) --virtual-pad --require-coverage \
	  --window-ms 8000 --tone-ms 1000 --combo-ms 500 --data-dir $(BUILD_DIR)/host/data

check-sdk:
	@if [ -z "$(PS5_PAYLOAD_SDK)" ]; then \
	  echo "PS5_PAYLOAD_SDK is not set. Run tools/fetch-sdk.sh and export the path it prints." >&2; exit 2; fi
	@if [ ! -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" ]; then \
	  echo "PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) does not look like a ps5-payload-sdk install." >&2; exit 2; fi

check-pacbrew:
	@if [ ! -f "$(PS5_PACBREW_PREFIX)/lib/libSDL2.a" ]; then \
	  echo "pacbrew bundle missing at $(PS5_PACBREW_PREFIX). Run: tools/fetch-pacbrew.sh $(PACBREW)" >&2; exit 2; fi

ps5: check-sdk check-pacbrew $(PS5_ELF)

$(PS5_ELF): $(SRC)
	@mkdir -p $(dir $@)
	"$(PS5_PAYLOAD_SDK)/bin/prospero-clang" $(CFLAGS_COMMON) $(PS5_EXTRA_FLAGS) $(PS5_INC) -o $@ $(SRC) $(PS5_LIBS)
	@echo "built $@"

# The run stops by itself (end marker, EOF, idle timeout, --timeout). The default input
# window is 45 s plus a 3 s tone; the timeout leaves a margin.
deploy: ps5
	"$(REPO_ROOT)/tools/deploy-hb.sh" ksre-02c "$(PS5_ELF)" --timeout 110 --idle-timeout 40 \
	  $(if $(ARGS),--args "$(ARGS)")

clean:
	rm -rf "$(BUILD_DIR)"
