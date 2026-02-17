PROJECT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC_DIR     := $(PROJECT_DIR)src
BUILD_DIR   := $(SRC_DIR)/build
BIN_DIR     := $(PROJECT_DIR)whispercpp
MODELS_DIR  := $(PROJECT_DIR)models
NPROC       := $(shell nproc)

.PHONY: all configure build install clean rebuild

all: configure build install

configure:
	cmake -B $(BUILD_DIR) -S $(SRC_DIR) \
		-DGGML_VULKAN=1 \
		-DCMAKE_BUILD_TYPE=Release

build:
	cmake --build $(BUILD_DIR) --config Release -j$(NPROC)

install:
	@rm -f $(BIN_DIR)/*.so* $(BIN_DIR)/whisper-*
	@cp $(BUILD_DIR)/bin/whisper-* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/bin/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/src/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/ggml/src/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "Binaries installed to $(BIN_DIR)/"
	@ls $(BIN_DIR)/whisper-* 2>/dev/null | wc -l | xargs -I{} echo "  {} binaries ready"

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN_DIR)/*.so* $(BIN_DIR)/whisper-*

rebuild: clean all
