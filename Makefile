CMake ?= cmake
FLOCK ?= flock
SOURCE_DIR ?= .
BUILD_DIR ?= out/build
DEBUG_PRESET ?= debug
ASAN_PRESET ?= asan
RELEASE_PRESET ?= release
DEBUG_BUILD_LOCK ?= /tmp/javelin-mail-debug-build-$(USER).lock
ASAN_BUILD_LOCK ?= /tmp/javelin-mail-asan-build-$(USER).lock
RELEASE_BUILD_LOCK ?= /tmp/javelin-mail-release-build-$(USER).lock
GUI_TARGET ?= javelin
DAEMON_TARGET ?= javelind
GUI_BINARY ?= $(BUILD_DIR)/$(DEBUG_PRESET)/bin/$(GUI_TARGET)
RUN_ARGS ?=

.PHONY: help configure-debug build-debug run configure-asan build-asan configure-release build-release test-debug clean

help:
	@printf '%s\n' \
		'Available targets:' \
		'  make configure-debug   Configure the debug CMake preset' \
		'  make build-debug       Build the debug preset' \
		'  make run               Build javelind + javelin, then run the GUI' \
		'  make run RUN_ARGS=...  Pass command-line arguments to the GUI' \
		'  make configure-asan    Configure the ASAN preset' \
		'  make build-asan        Build the ASAN preset' \
		'  make configure-release Configure the release preset' \
		'  make build-release     Build the release preset' \
		'  make test-debug        Run tests for the debug preset' \
		'  make clean             Remove the build directory'

configure-debug:
	$(FLOCK) $(DEBUG_BUILD_LOCK) $(CMake) --preset $(DEBUG_PRESET)

build-debug: configure-debug
	$(FLOCK) $(DEBUG_BUILD_LOCK) $(CMake) --build --preset $(DEBUG_PRESET)

run: configure-debug
	$(FLOCK) $(DEBUG_BUILD_LOCK) $(CMake) --build --preset $(DEBUG_PRESET) --target $(GUI_TARGET) $(DAEMON_TARGET)
	. $(BUILD_DIR)/$(DEBUG_PRESET)/prefix.sh && export JAVELIN_FORWARD_DAEMON_STDIO=1 && exec $(GUI_BINARY) $(RUN_ARGS)

configure-asan:
	$(FLOCK) $(ASAN_BUILD_LOCK) $(CMake) --preset $(ASAN_PRESET)

build-asan: configure-asan
	$(FLOCK) $(ASAN_BUILD_LOCK) $(CMake) --build --preset $(ASAN_PRESET)

configure-release:
	$(FLOCK) $(RELEASE_BUILD_LOCK) $(CMake) --preset $(RELEASE_PRESET)

build-release: configure-release
	$(FLOCK) $(RELEASE_BUILD_LOCK) $(CMake) --build --preset $(RELEASE_PRESET)

test-debug: build-debug
	$(FLOCK) $(DEBUG_BUILD_LOCK) ctest --test-dir $(BUILD_DIR)/$(DEBUG_PRESET) --output-on-failure

clean:
	$(FLOCK) $(DEBUG_BUILD_LOCK) $(FLOCK) $(ASAN_BUILD_LOCK) $(FLOCK) $(RELEASE_BUILD_LOCK) rm -rf $(BUILD_DIR)
