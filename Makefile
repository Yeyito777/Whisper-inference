PROJECT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC_DIR     := $(PROJECT_DIR)src
BUILD_DIR   := $(SRC_DIR)/build
BIN_DIR     := $(PROJECT_DIR)whispercpp
MODELS_DIR  := $(PROJECT_DIR)models
NPROC       := $(shell nproc)

# Dictate daemon paths
DICTATE_SRC := $(PROJECT_DIR)whisper-dictate.cpp
DICTATE_BIN := $(PROJECT_DIR)whisper-dictate
CTL_SRC     := $(PROJECT_DIR)whisper-ctl.c
CTL_BIN     := $(PROJECT_DIR)whisper-ctl

# Include/lib paths for dictate build
WHISPER_INC := $(SRC_DIR)/include
GGML_INC    := $(SRC_DIR)/ggml/include
SDL_SRC     := $(SRC_DIR)/examples/common-sdl.cpp
SDL_HDR_DIR := $(SRC_DIR)/examples
LIB_DIR     := $(BIN_DIR)

SDL_CFLAGS  := $(shell pkg-config --cflags sdl2)
SDL_LIBS    := $(shell pkg-config --libs sdl2)

# Install paths
PREFIX      := $(HOME)/.local
BIN_PREFIX  := $(PREFIX)/bin
LIB_PREFIX  := $(PREFIX)/lib
CONFIG_DIR  := $(HOME)/.config/whisper
SERVICE_DIR := $(HOME)/.config/systemd/user

.PHONY: all configure build libs clean rebuild dictate install uninstall

all: configure build libs dictate

configure:
	cmake -B $(BUILD_DIR) -S $(SRC_DIR) \
		-DGGML_VULKAN=1 \
		-DWHISPER_SDL2=ON \
		-DCMAKE_BUILD_TYPE=Release

build:
	cmake --build $(BUILD_DIR) --config Release -j$(NPROC)

libs:
	@rm -f $(BIN_DIR)/*.so* $(BIN_DIR)/whisper-*
	@cp $(BUILD_DIR)/bin/whisper-* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/bin/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/src/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@cp $(BUILD_DIR)/ggml/src/*.so* $(BIN_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "Libraries staged to $(BIN_DIR)/"

dictate: $(DICTATE_BIN) $(CTL_BIN)

$(DICTATE_BIN): $(DICTATE_SRC) $(SDL_SRC)
	g++ -O2 -std=c++17 \
		-I$(WHISPER_INC) -I$(GGML_INC) -I$(SDL_HDR_DIR) \
		$(SDL_CFLAGS) \
		$(DICTATE_SRC) $(SDL_SRC) \
		-L$(LIB_DIR) -lwhisper -lggml \
		$(SDL_LIBS) -lXtst -lX11 -lpthread \
		-Wl,-rpath,$(LIB_DIR),-rpath,$(LIB_PREFIX) \
		-o $(DICTATE_BIN)
	@echo "Built whisper-dictate"

$(CTL_BIN): $(CTL_SRC)
	gcc -O2 -std=c11 $(CTL_SRC) -o $(CTL_BIN)
	@echo "Built whisper-ctl"

install: all
	@mkdir -p $(BIN_PREFIX) $(LIB_PREFIX) $(CONFIG_DIR) $(SERVICE_DIR)
	@cp $(DICTATE_BIN) $(CTL_BIN) $(BIN_PREFIX)/
	@cp $(BIN_DIR)/lib*.so* $(LIB_PREFIX)/
	@if [ ! -f $(CONFIG_DIR)/dictate.conf ]; then \
		sed 's|^model = .*|model = $(MODELS_DIR)ggml-large-v3-turbo.bin|' \
			$(PROJECT_DIR)dictate.conf > $(CONFIG_DIR)/dictate.conf; \
		echo "Created $(CONFIG_DIR)/dictate.conf"; \
	else \
		echo "Config exists at $(CONFIG_DIR)/dictate.conf (not overwritten)"; \
	fi
	@cp $(PROJECT_DIR)whisper-dictate.service $(SERVICE_DIR)/
	@systemctl --user daemon-reload
	@systemctl --user enable whisper-dictate
	@echo ""
	@echo "Installed:"
	@echo "  Binaries: $(BIN_PREFIX)/whisper-dictate, $(BIN_PREFIX)/whisper-ctl"
	@echo "  Libraries: $(LIB_PREFIX)/lib{whisper,ggml}*.so"
	@echo "  Config: $(CONFIG_DIR)/dictate.conf"
	@echo "  Service: enabled (start with 'systemctl --user start whisper-dictate')"

uninstall:
	-@systemctl --user disable --now whisper-dictate 2>/dev/null
	@rm -f $(SERVICE_DIR)/whisper-dictate.service
	@systemctl --user daemon-reload
	@rm -f $(BIN_PREFIX)/whisper-dictate $(BIN_PREFIX)/whisper-ctl
	@rm -f $(LIB_PREFIX)/libwhisper.so* $(LIB_PREFIX)/libggml.so*
	@echo "Uninstalled whisper-dictate"

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN_DIR)/*.so* $(BIN_DIR)/whisper-*
	rm -f $(DICTATE_BIN) $(CTL_BIN)

rebuild: clean all
