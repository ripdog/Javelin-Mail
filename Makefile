CMake ?= cmake
SOURCE_DIR ?= .
BUILD_DIR ?= out/build
DEBUG_PRESET ?= debug
ASAN_PRESET ?= asan
RELEASE_PRESET ?= release
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
	$(CMake) --preset $(DEBUG_PRESET)

build-debug:
	@test -f $(BUILD_DIR)/$(DEBUG_PRESET)/CMakeCache.txt || $(CMake) --preset $(DEBUG_PRESET)
	$(CMake) --build --preset $(DEBUG_PRESET)

run:
	@test -f $(BUILD_DIR)/$(DEBUG_PRESET)/CMakeCache.txt || $(CMake) --preset $(DEBUG_PRESET)
	$(CMake) --build --preset $(DEBUG_PRESET) --target $(GUI_TARGET) $(DAEMON_TARGET)
	. $(BUILD_DIR)/$(DEBUG_PRESET)/prefix.sh && exec $(GUI_BINARY) $(RUN_ARGS)

configure-asan:
	$(CMake) --preset $(ASAN_PRESET)

build-asan: configure-asan
	$(CMake) --build --preset $(ASAN_PRESET)

configure-release:
	$(CMake) --preset $(RELEASE_PRESET)

build-release: configure-release
	$(CMake) --build --preset $(RELEASE_PRESET)

test-debug: build-debug
	ctest --test-dir $(BUILD_DIR)/$(DEBUG_PRESET) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)
