# whisper-dictate — Push-to-talk dictation

Hold F1 to record speech, release to transcribe and type into the focused window.

## Architecture

```
keyboard F1 press ──> dwm ──> whisper-ctl start ──> Unix socket
keyboard F1 release ──> dwm ──> whisper-ctl stop  ──> Unix socket
                                                        │
                                              whisper-dictate daemon
                                              ├── model in VRAM (turbo)
                                              ├── SDL2 mic capture
                                              ├── SDL2 sound feedback (connected/disconnected.wav)
                                              ├── whisper.cpp inference
                                              └── XTest keystroke injection
                                                        │
                                                  focused X11 window
```

### Components

- **whisper-dictate** — Daemon (systemd user service). Loads the whisper model into VRAM once at startup. Listens on `$XDG_RUNTIME_DIR/whisper-dictate.sock`. On `start`: plays connected.wav, resumes SDL2 mic capture. On `stop`: plays disconnected.wav, pauses capture, transcribes all buffered audio (up to 30s), injects text via XTest. Sound files live in `assets/`.
- **whisper-ctl** — Tiny C client. Connects to the socket, sends `start` or `stop`, exits. Spawned by dwm as a fire-and-forget process.
- **dwm keybindings** — F1 KeyPress calls `whisper-ctl start`, F1 KeyRelease calls `whisper-ctl stop`. `XkbSetDetectableAutoRepeat` suppresses autorepeat so only real releases fire.

## Configuration

`dictate.conf` in the project root:

```ini
# SDL audio capture device ID. -1 = system default.
# Run `whisper-dictate --list-devices` to see available devices.
capture_id = -1

# Path to GGML model (relative to this file, or absolute)
model = models/ggml-large-v3-turbo.bin

# Spoken language code (en, es, de, fr, ja, auto, etc.)
language = en
```

## Building

```sh
make          # builds whisper.cpp (Vulkan + SDL2) then whisper-dictate + whisper-ctl
make dictate  # rebuilds just the dictate binaries (fast, no cmake)
```

### Dependencies

- whisper.cpp submodule in `src/` (already included)
- Vulkan SDK
- SDL2 (`sdl2` / `sdl2-compat`)
- X11 + XTest (`libxtst`, `libx11`)
- CMake, C/C++ compiler

## systemd user service

Installed at `~/.config/systemd/user/whisper-dictate.service`.

```sh
systemctl --user enable whisper-dictate   # start on login
systemctl --user start whisper-dictate    # start now
systemctl --user restart whisper-dictate  # restart after rebuild
journalctl --user -u whisper-dictate -f   # watch logs
```

The service sets `Environment=DISPLAY=:0` since it needs X11 for XTest injection.

## dwm changes

In `dwm.c`:
- `#include <X11/XKBlib.h>`
- `keyrelease()` handler + `[KeyRelease] = keyrelease` in handler array
- `XkbSetDetectableAutoRepeat(dpy, True, NULL)` in `setup()`
- `keyreleases[]` array in `config.h` (same `Key` struct as `keys[]`)
- `grabkeys()` also grabs keys from `keyreleases[]`

In `config.h`:
```c
static const char *dictate_start[] = { "/path/to/whisper-ctl", "start", NULL };
static const char *dictate_stop[]  = { "/path/to/whisper-ctl", "stop", NULL };

// In keys[]:
{ 0, XK_F1, spawn, {.v = dictate_start } },

// In keyreleases[]:
{ 0, XK_F1, spawn, {.v = dictate_stop } },
```

## How it works

1. On login, systemd starts `whisper-dictate`. The turbo model loads into VRAM (~1.6 GB). The daemon opens the mic (paused), loads sound effects from `assets/`, and waits on the socket.
2. User presses F1. dwm spawns `whisper-ctl start`. The daemon plays `connected.wav`, resumes mic capture, and clears the audio buffer.
3. User speaks while holding F1. SDL2's audio callback fills a 30-second circular buffer in the background.
4. User releases F1. dwm spawns `whisper-ctl stop`. The daemon plays `disconnected.wav`, pauses capture, grabs all buffered audio, and runs `whisper_full()` on it.
5. The transcribed text is injected character-by-character into the focused window via XTest fake key events.

## Troubleshooting

**Daemon crash-looping**: Check `journalctl --user -u whisper-dictate -f`. Common causes:
- `cannot open X display` — service needs `Environment=DISPLAY=:0`
- `failed to load model` — check model path in `dictate.conf`
- `audio init failed` — check `capture_id`, run `whisper-dictate --list-devices`

**F1 does nothing**: Make sure dwm was rebuilt and restarted after adding the keybindings. Check that the daemon is running (`systemctl --user status whisper-dictate`).

**Wrong mic**: Set `capture_id` in `dictate.conf` to the device number from `whisper-dictate --list-devices`.
