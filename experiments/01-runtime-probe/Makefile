# SPDX-License-Identifier: GPL-3.0-or-later
#
# Experiment 01: runtime capability probe.
#
#   make host               build the probe for this machine (logic test only)
#   make run-host           build and run it on this machine
#   make ps5                build the PS5 payload (needs PS5_PAYLOAD_SDK + host LLVM/lld)
#   make ps5-compile-check  compile (no link) for x86_64-sie-ps5 with any clang
#                           that knows the target, e.g. Apple clang
#   make link-inventory     link-test libc/POSIX symbols CPython's configure probes
#   make deploy             send the PS5 payload using .local-config/console.env
#
# Outputs go to $(BUILD_DIR) (ignored by git).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_DIR ?= $(REPO_ROOT)/build/experiments/01-runtime-probe
SRC       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/probe.c

UNAME_S := $(shell uname -s)
HOST_CC ?= cc
HOST_LIBS :=
ifeq ($(UNAME_S),Linux)
  HOST_LIBS := -lpthread -ldl -lm
endif

WARN_FLAGS := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
CFLAGS_COMMON := -std=gnu11 -O1 -g $(WARN_FLAGS)
# The SDK's FreeBSD math.h isnan/isinf macros trigger this harmless warning.
PS5_EXTRA_FLAGS := -Wno-unknown-warning-option -Wno-unreachable-code-generic-assoc

PS5_ELF := $(BUILD_DIR)/ps5/ksre-ps5-probe.elf
HOST_BIN := $(BUILD_DIR)/host/ksre-ps5-probe

# Flags mirrored from the SDK's bin/prospero-clang wrapper (v0.45) for the
# compile-only check. Linking always goes through the real wrapper.
CROSS_CHECK_CC ?= clang
CROSS_CHECK_FLAGS = -target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default \
	-isysroot "$(PS5_PAYLOAD_SDK)" -isystem "$(PS5_PAYLOAD_SDK)/target/include" \
	-fno-stack-protector -fno-plt -femulated-tls

.PHONY: all host run-host ps5 ps5-compile-check link-inventory deploy clean check-sdk

all: host

host: $(HOST_BIN)

$(HOST_BIN): $(SRC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $< $(HOST_LIBS)

run-host: $(HOST_BIN)
	@mkdir -p $(BUILD_DIR)/host/probe-data
	KSRE_PS5_PROBE_DIR=$(BUILD_DIR)/host/probe-data $(HOST_BIN)

check-sdk:
	@if [ -z "$(PS5_PAYLOAD_SDK)" ]; then \
	  echo "PS5_PAYLOAD_SDK is not set. Run tools/fetch-sdk.sh and export the path it prints." >&2; \
	  exit 2; fi
	@if [ ! -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" ] || [ ! -f "$(PS5_PAYLOAD_SDK)/target/lib/libc.a" ]; then \
	  echo "PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) does not look like a ps5-payload-sdk install." >&2; \
	  exit 2; fi

ps5: check-sdk $(PS5_ELF)

$(PS5_ELF): $(SRC)
	@mkdir -p $(dir $@)
	@"$(PS5_PAYLOAD_SDK)/bin/prospero-llvm-config" --version >/dev/null 2>&1 || { \
	  echo "No host LLVM found by prospero-llvm-config. Install clang+lld (see docs/BUILDING.md)." >&2; \
	  exit 2; }
	"$(PS5_PAYLOAD_SDK)/bin/prospero-clang" $(CFLAGS_COMMON) $(PS5_EXTRA_FLAGS) -o $@ $<
	@echo "built $@"

ps5-compile-check: check-sdk
	@mkdir -p $(BUILD_DIR)/ps5-check
	$(CROSS_CHECK_CC) $(CROSS_CHECK_FLAGS) $(CFLAGS_COMMON) $(PS5_EXTRA_FLAGS) -c $(SRC) -o $(BUILD_DIR)/ps5-check/probe.o
	@echo "compile-only check OK: $(BUILD_DIR)/ps5-check/probe.o (not linked, not a payload)"

link-inventory: check-sdk
	"$(REPO_ROOT)/experiments/01-runtime-probe/link-inventory.sh" "$(BUILD_DIR)/link-inventory"

deploy: ps5
	"$(REPO_ROOT)/tools/deploy.sh" "$(PS5_ELF)"

clean:
	rm -rf "$(BUILD_DIR)"
