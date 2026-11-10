# SPDX-License-Identifier: GPL-3.0-or-later
#
# Experiment 03: a payload that embeds the static CPython 3.12.8 build and runs hello.py.
#
#   make ps5        cross-build the payload (needs ports/cpython/build.sh host + ps5 first,
#                   PS5_PAYLOAD_SDK and a host LLVM)
#   make host       build the same launcher for this machine against build/host-python
#   make run-host   run it on the host (logic check only; not evidence about the PS5)
#
# Console run (03b): the payload expects the stdlib at
# /data/ksre-ps5/python/lib/python312.zip (build/cpython/ps5/python-home/lib/python312.zip).

HERE      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(HERE)/../..)
BUILD_DIR ?= $(REPO_ROOT)/build/experiments/03-python-hello
SRC       := $(HERE)/main.c
SCRIPT    := $(HERE)/hello.py
HDR       := $(BUILD_DIR)/gen/hello_py.h

# Written by ports/cpython/build.sh ps5: PY_PS5_INCLUDES, PY_PS5_LIBS, PY_PS5_STDLIB_ZIP.
PY_EMBED_MK := $(REPO_ROOT)/build/cpython/ps5/embed.mk
-include $(PY_EMBED_MK)

WARN_FLAGS    := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
CFLAGS_COMMON := -std=gnu11 -O2 -g $(WARN_FLAGS) -I$(BUILD_DIR)/gen

PS5_ELF  := $(BUILD_DIR)/ps5/ksre-ps5-03-python-hello.elf
HOST_BIN := $(BUILD_DIR)/host/ksre-ps5-03-python-hello

HOST_CC        ?= cc
HOST_PY_PREFIX := $(REPO_ROOT)/build/host-python
HOST_PY_CONFIG := $(HOST_PY_PREFIX)/bin/python3.12-config

# -rdynamic (R13) and the libraries come with PY_PS5_LIBS; FTZ/DAZ and _exit() in main.c.
PS5_EXTRA_FLAGS := -Wno-unknown-warning-option -rdynamic

.PHONY: all host run-host ps5 clean check-sdk check-python

all: ps5

$(HDR): $(SCRIPT)
	@mkdir -p $(dir $@)
	python3 -c 'import sys; d = open(sys.argv[1], "rb").read() + b"\0"; \
	  rows = ",\n".join(",".join(str(b) for b in d[i:i + 24]) for i in range(0, len(d), 24)); \
	  open(sys.argv[2], "w").write("/* generated from hello.py */\nstatic const unsigned char hello_py[] = {\n" + rows + "\n};\n")' \
	  $(SCRIPT) $@

check-sdk:
	@if [ -z "$(PS5_PAYLOAD_SDK)" ] || [ ! -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" ]; then \
	  echo "PS5_PAYLOAD_SDK is not set or not an SDK. Run tools/fetch-sdk.sh and export it." >&2; exit 2; fi

check-python:
	@if [ ! -f "$(PY_EMBED_MK)" ]; then \
	  echo "No PS5 CPython build. Run: ports/cpython/build.sh host && ports/cpython/build.sh ps5" >&2; exit 2; fi

ps5: check-sdk check-python $(PS5_ELF)

$(PS5_ELF): $(SRC) $(HDR) $(PY_EMBED_MK)
	@mkdir -p $(dir $@)
	"$(PS5_PAYLOAD_SDK)/bin/prospero-clang" $(CFLAGS_COMMON) $(PS5_EXTRA_FLAGS) $(PY_PS5_INCLUDES) \
	  -o $@ $(SRC) $(PY_PS5_LIBS)
	@echo "built $@"

host: $(HOST_BIN)

$(HOST_BIN): $(SRC) $(HDR)
	@if [ ! -x "$(HOST_PY_CONFIG)" ]; then echo "Run: ports/cpython/build.sh host" >&2; exit 2; fi
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) $$($(HOST_PY_CONFIG) --includes) -DKSRE_PY_HOME='"$(HOST_PY_PREFIX)"' \
	  -o $@ $(SRC) $$($(HOST_PY_CONFIG) --ldflags --embed) -lpthread

run-host: $(HOST_BIN)
	$(HOST_BIN)

clean:
	rm -rf "$(BUILD_DIR)"
