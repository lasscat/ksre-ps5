# SPDX-License-Identifier: GPL-3.0-or-later
#
# Experiment 02b: OpenGL context, Ren'Py-style GLSL shaders, 1080p benchmark.
#
#   make host          build for this machine (needs SDL2: brew/apt)
#   make run-host      real GL window on this machine, desktop GL dialect ("gl"), short run
#   make ps5           cross-build the payload (needs PS5_PAYLOAD_SDK, host LLVM,
#                      and tools/fetch-pacbrew.sh <variant> already run)
#   make deploy-hw     upload payload + Mesa shared libraries (v0.41.1 bundle) and run as homebrew
#   make deploy-sw     same for the OSMesa (software GL) bundle v0.40.3
#
#   PACBREW=hwgl|swgl  which pacbrew bundle to link/deploy (default hwgl; see upstreams.lock)
#   ARGS="..."         extra payload arguments for deploy-*, e.g. ARGS="--api gles --frames 120"
#   ENVV="A=1 B=2"     environment for the payload (e.g. ENVV="EGL_LOG_LEVEL=debug MESA_DEBUG=1")
#
# The payload links only SDL2 (static) and the SCE system libraries. Mesa is
# dlopen()ed by SDL at run time, so the Mesa .so files must sit where the SDK
# run-time linker looks: the payload's working directory, which the homebrew
# launcher sets to the directory the ELF was uploaded to. deploy-hw/deploy-sw
# upload them there (under /data/homebrew/<name>/, nothing else is written).

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(HERE)/../..)
BUILD_DIR ?= $(REPO_ROOT)/build/experiments/02b-gl-shaders
SRC       := $(HERE)/main.c

PACBREW ?= hwgl
PACBREW_KEY := $(if $(filter swgl,$(PACBREW)),PACBREW_SWGL,PACBREW_HWGL)
PACBREW_REL := $(shell sed -n 's/^$(PACBREW_KEY)_RELEASE=//p' $(REPO_ROOT)/upstreams.lock)
PS5_PACBREW_PREFIX ?= $(REPO_ROOT)/external/toolchains/pacbrew-$(PACBREW_REL)/user/homebrew
PS5_OUT := $(if $(filter swgl,$(PACBREW)),ps5-swgl,ps5)

WARN_FLAGS    := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
CFLAGS_COMMON := -std=gnu11 -O2 -g $(WARN_FLAGS)

PS5_ELF  := $(BUILD_DIR)/$(PS5_OUT)/ksre-ps5-02b-gl-shaders.elf
HOST_BIN := $(BUILD_DIR)/host/ksre-ps5-02b-gl-shaders

HOST_CC     ?= cc
SDL_CONFIG  ?= sdl2-config
HOST_CFLAGS  = $(shell $(SDL_CONFIG) --cflags)
HOST_LIBS    = $(shell $(SDL_CONFIG) --libs) -lm -pthread

# Bundle headers come BEFORE the SDK's so the bundle's SDL headers win.
PS5_INC := -I$(PS5_PACBREW_PREFIX)/include -I$(PS5_PACBREW_PREFIX)/include/SDL2 -D_REENTRANT -D_THREAD_SAFE
# Static link; SCE system libraries are the ones the bundle's sdl2-config --static-libs lists.
PS5_LIBS := -L$(PS5_PACBREW_PREFIX)/lib \
	-lSDL2 -liconv -lsamplerate -lSceSystemService -lSceUserService -lScePad -lSceAudioOut \
	-lSceVideoOut -lSceKeyboard -lSceImeDialog -lm
# -rdynamic exports the executable's symbols (notably kernel_mprotect from crt1.o). The bundles'
# shared libraries (libOSMesa) contain the SDK's mmap(), which maps executable memory by calling
# a WEAK import kernel_mprotect that the SDK's runtime linker resolves only against the
# executable's exported symbols; without it the import stays NULL and the first JIT/rtasm
# allocation calls address 0 (console 02b: SIGSEGV in glGenerateMipmap / first draw).
PS5_EXTRA_FLAGS := -Wno-unknown-warning-option -rdynamic

# Shared libraries to place next to eboot.elf: LOCAL:REMOTE-SONAME (the loader asks for the soname).
# Each bundle's own prefix is used, whichever PACBREW is selected for the build.
bundle_lib = $(REPO_ROOT)/external/toolchains/pacbrew-$(shell sed -n 's/^PACBREW_$(1)_RELEASE=//p' $(REPO_ROOT)/upstreams.lock)/user/homebrew/lib
HW_LIBDIR := $(call bundle_lib,HWGL)
SW_LIBDIR := $(call bundle_lib,SWGL)
HW_LIBS := $(HW_LIBDIR)/libEGL.so.1:libEGL.so.1 $(HW_LIBDIR)/libGL.so.1:libGL.so.1 \
	$(HW_LIBDIR)/libGLESv2.so.2:libGLESv2.so.2 $(HW_LIBDIR)/libgallium-26.2.0.so:libgallium-26.2.0.so \
	$(HW_LIBDIR)/libvulkan.so.1:libvulkan.so.1
SW_LIBS := $(SW_LIBDIR)/libOSMesa.so.8:libOSMesa.so.8
ENVV ?=
ARGS ?=

.PHONY: all host run-host ps5 deploy-hw deploy-sw clean check-sdk check-pacbrew

all: host

host: $(HOST_BIN)

$(HOST_BIN): $(SRC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) $(HOST_CFLAGS) -o $@ $(SRC) $(HOST_LIBS)

# Needs a desktop session with OpenGL (macOS: GL 2.1 legacy, GLSL 1.20; Linux: Mesa/NVIDIA).
run-host: $(HOST_BIN)
	$(HOST_BIN) --api gl --windowed --frames 60 --vsync 0 --hold-ms 1500 --screenshot $(BUILD_DIR)/host/frame

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

deploy-hw:
	$(MAKE) -C $(HERE) ps5 PACBREW=hwgl
	"$(REPO_ROOT)/tools/deploy-hb.sh" ksre-02b-hw "$(BUILD_DIR)/ps5/ksre-ps5-02b-gl-shaders.elf" \
	  $(foreach l,$(HW_LIBS),--extra $(l)) $(if $(ENVV),--env "$(ENVV)") $(if $(ARGS),--args "$(ARGS)")

deploy-sw:
	$(MAKE) -C $(HERE) ps5 PACBREW=swgl
	"$(REPO_ROOT)/tools/deploy-hb.sh" ksre-02b-sw "$(BUILD_DIR)/ps5-swgl/ksre-ps5-02b-gl-shaders.elf" \
	  $(foreach l,$(SW_LIBS),--extra $(l)) $(if $(ENVV),--env "$(ENVV)") $(if $(ARGS),--args "$(ARGS)")

clean:
	rm -rf "$(BUILD_DIR)"
