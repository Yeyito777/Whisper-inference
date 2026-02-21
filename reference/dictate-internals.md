# Dictate system — Technical internals

Deep-dive into the implementation of the push-to-talk dictation system: the daemon, the IPC client, the dwm integration, and the design decisions behind each.

## Files

| File | Language | Purpose |
|------|----------|---------|
| `whisper-dictate.cpp` | C++17 | Daemon — model loading, audio capture, transcription, keystroke injection |
| `whisper-ctl.c` | C11 | IPC client — sends start/stop over Unix socket |
| `dictate.conf` | INI-style | Runtime config — mic device, model path, language |
| `Makefile` | Make | Build targets for whisper.cpp (cmake) and dictate binaries (direct g++/gcc) |
| `~/.config/systemd/user/whisper-dictate.service` | systemd unit | User service for auto-start on login |
| `~/Config/dwm/dwm.c` | C99 | Window manager — keyrelease handler, detectable autorepeat |
| `~/Config/dwm/config.h` | C99 | Keybindings — F1 press/release → whisper-ctl |

## whisper-dictate.cpp

### Initialization sequence (main:172–233)

1. **Exe directory resolution** (main:179–187) — `readlink("/proc/self/exe")` + `dirname()` to find the binary's directory. All relative paths in `dictate.conf` resolve from here. This lets the daemon work regardless of `$CWD`.

2. **Config parsing** (`load_config`:46–67) — Reads `dictate.conf` from the exe directory. Simple `key = value` parser that handles `#` comments and whitespace. Populates a `Config` struct with `capture_id`, `model`, `language`.

3. **GGML backend init** (main:203) — `ggml_backend_load_all()` discovers and loads all available compute backends (Vulkan, CPU). This is what enables GPU inference. Must be called before `whisper_init_from_file_with_params`.

4. **Whisper context creation** (main:205–215) — `whisper_context_default_params()` with `use_gpu=true` and `flash_attn=true`. `whisper_init_from_file_with_params()` loads the GGML model file, allocates GPU buffers, and returns the context. This is the expensive step (~1–2 seconds, ~1.6 GB VRAM for turbo).

5. **SDL2 audio init** (main:218–225) — `audio_async` is whisper.cpp's SDL2 audio wrapper (from `src/examples/common-sdl.cpp`). Constructor takes buffer length in ms (30000 = 30s circular buffer). `audio.init(capture_id, 16000)` opens the capture device at 16kHz mono float32. The device starts **paused** — no audio is captured until `audio.resume()`.

6. **XTest connect** (`xtest_connect`:81–92) — Opens a separate X11 display connection (the daemon's own, not dwm's). Registers `XSetIOErrorHandler` with `on_x_io_error` which uses `longjmp` to return control to the main reconnect loop instead of crashing. Queries for the XTest extension. Returns bool so it can be used both at startup and during reconnection. A `setjmp` point before the main loop catches X disconnects: pauses recording if active, polls `XOpenDisplay` every 2s, then retries any `pending_text` that was interrupted mid-injection.

7. **Unix socket creation** (`create_socket`:134–153) — `$XDG_RUNTIME_DIR/whisper-dictate.sock` (typically `/run/user/1000/whisper-dictate.sock`). `AF_UNIX` + `SOCK_STREAM`. `unlink()` first to clean up stale sockets from a previous crash. `chmod(0700)` for user-only access. `listen(fd, 4)` — backlog of 4 is more than enough since commands are sequential.

### Main loop (main:235–313)

The loop uses `poll()` with a 200ms timeout on the listening socket. When idle (not recording), it blocks here. When a client connects:

1. `accept()` → `read()` → `close()` — the client connection is transient (whisper-ctl connects, sends a few bytes, disconnects).
2. Command is trimmed and matched against `"start"` or `"stop"`.

**On "start"** (main:253–258):
- `audio.resume()` — Calls `SDL_PauseAudioDevice(dev, 0)` which starts the SDL audio callback. The callback (`audio_async::callback` in common-sdl.cpp:139) writes incoming samples into the circular buffer under a mutex.
- `audio.clear()` — Resets the circular buffer position and length to 0 so we don't transcribe stale audio from a previous recording.
- `recording = true` — Prevents duplicate starts, enables stop handling.

**On "stop"** (main:259–312):
- `SDL_Delay(100)` — 100ms grace period. Without this, the last ~50ms of speech can be cut off because `audio.pause()` stops the callback immediately.
- `audio.get(30000, pcm)` — Copies all available samples (up to 30s) from the circular buffer into `pcm`. The circular buffer uses a read position that trails the write position, returning samples in chronological order. This is a snapshot — the callback may still be writing, but the mutex in `audio_async::get()` ensures a consistent read.
- `audio.pause()` — Stops the SDL audio callback.

**Transcription** (main:276–301):
- `whisper_full_default_params(WHISPER_SAMPLING_GREEDY)` — Greedy decoding (no beam search). Faster than beam search, slightly less accurate.
- Key params: `no_context=true` (don't use previous transcriptions as prompt), `no_timestamps=true` (we only want text, not `[00:00 --> 00:05]` prefixes), `n_threads=4` (CPU threads for non-GPU parts of the pipeline).
- `whisper_full(ctx, wparams, pcm.data(), pcm.size())` — The main inference call. Internally: PCM → log mel spectrogram → encoder (GPU) → decoder (GPU) → tokens → text. For 10s of audio with turbo on Vulkan, this takes ~500–1500ms.
- Segments are concatenated. Whisper may produce multiple segments for longer audio. Each `whisper_full_get_segment_text(ctx, i)` returns a `const char*` owned by the context (valid until next `whisper_full` call).

### XTest keystroke injection (xtest_type:86–124)

**UTF-8 decoding** (lines 90–101): Manual UTF-8 → Unicode codepoint decoder. Handles 1–4 byte sequences. Continuation bytes (0x80–0xBF) are skipped.

**Keysym mapping** (lines 103–105): ASCII codepoints map 1:1 to X11 keysyms. Unicode codepoints >= 0x100 use the X11 Unicode keysym range: `0x01000000 | codepoint`.

**Shift detection** (lines 110–114): Compares the unshifted keysym (index 0) and shifted keysym (index 1) for the keycode. If the target keysym matches only the shifted variant, a synthetic Shift_L press is injected around the key event.

**Event injection** (lines 116–121): `XTestFakeKeyEvent` injects synthetic key events that bypass X11 grabs. This is how the daemon can type into any focused window — XTest events are delivered directly to the focus window, not through the grab chain.

**XFlush** (line 123): Flushes all queued events to the X server in one batch. Without this, events would sit in the client buffer.

### Socket path convention

Both `whisper-dictate.cpp:socket_path()` and `whisper-ctl.c:socket_path()` use identical logic:
- `$XDG_RUNTIME_DIR/whisper-dictate.sock` if `XDG_RUNTIME_DIR` is set
- `/tmp/whisper-dictate-{uid}.sock` as fallback

This ensures the daemon and client always agree on the path without configuration.

## whisper-ctl.c

42 lines of C. Intentionally minimal — it's spawned by dwm on every F1 press/release, so startup time matters.

- `socket()` → `connect()` → `write()` → `close()`. No response expected.
- **Silent failure on connect error** (line 35): If the daemon isn't running, `connect()` fails with `ENOENT`. The client returns 1 silently — no error dialog, no stderr. dwm spawns it fire-and-forget so nobody checks the exit code.
- No dynamic allocation, no libraries beyond libc.

## dwm integration

### KeyRelease handling

Stock dwm only handles `KeyPress` events. Three changes were needed:

1. **Handler array** (`dwm.c:280`): Added `[KeyRelease] = keyrelease` to the static event handler table.

2. **keyrelease function** (`dwm.c:1158–1172`): Mirrors `keypress()` exactly but iterates over `keyreleases[]` instead of `keys[]`. Same `XKeycodeToKeysym` + `CLEANMASK` matching.

3. **grabkeys** (`dwm.c:1107–1120`): Added a second loop over `keyreleases[]` to `XGrabKey` those keysyms too. `XGrabKey` delivers both KeyPress and KeyRelease for a grabbed key, but the key must be grabbed first. Since F1 appears in both `keys[]` (press) and `keyreleases[]` (release), it gets grabbed by both loops — harmless duplicate, but ensures release-only bindings would also work.

### Detectable autorepeat

`XkbSetDetectableAutoRepeat(dpy, True, NULL)` in `setup()` (dwm.c:1988). Without this, holding F1 generates rapid `KeyRelease`→`KeyPress` pairs from X11's autorepeat system (at the configured rate of 45/sec). With detectable autorepeat enabled, X11 only sends the real `KeyRelease` when the physical key is actually lifted. This is critical — without it, holding F1 would spam start/stop commands.

### config.h bindings

```c
// keys[] — fires on KeyPress
{ 0, XK_F1, spawn, {.v = dictate_start } },

// keyreleases[] — fires on KeyRelease
{ 0, XK_F1, spawn, {.v = dictate_stop } },
```

Modifier is `0` (no modifier required). `spawn()` forks+execs the command array. dwm's `SA_NOCLDWAIT` flag in `setup()` ensures child processes don't become zombies.

## Audio pipeline details

### SDL2 circular buffer (common-sdl.cpp)

`audio_async` manages a circular float32 buffer of size `(sample_rate * len_ms) / 1000` samples. At 16kHz and 30000ms, that's 480,000 floats (1.83 MB).

- **Callback** (`common-sdl.cpp:139`): SDL calls this from its audio thread with raw PCM chunks (1024 samples per frame). The callback writes into the circular buffer under mutex, wrapping at the end.
- **get()** (`common-sdl.cpp:170`): Copies the last N ms of audio from the buffer. Handles wraparound. The caller gets a contiguous vector.
- **clear()** (`common-sdl.cpp:117`): Resets `m_audio_pos` and `m_audio_len` to 0. Does not zero the buffer memory — subsequent `get()` calls just return empty until new audio arrives.
- **pause/resume** (`common-sdl.cpp:81,99`): `SDL_PauseAudioDevice(dev, 0/1)`. Pause stops the callback from firing. The buffer retains its data.

### Whisper inference parameters

| Parameter | Value | Why |
|-----------|-------|-----|
| `strategy` | `WHISPER_SAMPLING_GREEDY` | Fastest decoding. Beam search (`beam_size=5`) would be ~2x slower for marginal accuracy gain. |
| `no_context` | `true` | Each recording is independent. Using previous context as prompt can cause the model to repeat itself. |
| `no_timestamps` | `true` | We want raw text, not `[00:00.000 --> 00:05.000]` annotations. |
| `single_segment` | `false` | Allow multiple segments for longer recordings. Whisper naturally segments at ~30s boundaries. |
| `n_threads` | `4` | CPU threads for mel spectrogram computation and other non-GPU steps. |
| `language` | from config | Hardcoded language skips the 30-token language detection pass. |

### Model details

`ggml-large-v3-turbo.bin`: 809M parameters, ~1.6 GB on disk, float16. Turbo is a distilled version of large-v3 with only 4 decoder layers (vs 32 in full large-v3). This makes it ~6x faster at decoding with minimal accuracy loss.

## systemd service

```ini
[Service]
Type=simple
Environment=DISPLAY=:0
ExecStartPre=/bin/sh -c 'until xset q >/dev/null 2>&1; do sleep 2; done'
ExecStart=%h/Config/whisper/whisper-dictate
Restart=on-failure
RestartSec=2
```

- `Type=simple` — systemd considers the service started as soon as the process is running. No readiness notification needed.
- `Environment=DISPLAY=:0` — Required because user services don't inherit the X11 display variable. Without this, `XOpenDisplay(NULL)` fails.
- `ExecStartPre` — Blocks until X server is accepting connections (`xset q` succeeds). Prevents the daemon from starting without X, which would waste time loading the model into VRAM only to fail on `XOpenDisplay`. Previously this caused crash-looping (model load → XOpenDisplay fail → exit → restart → repeat, visible as VRAM flickering).
- `Restart=on-failure` + `RestartSec=2` — The daemon survives X disconnects internally (longjmp + reconnect loop), so it only exits on actual crashes or SIGTERM. `on-failure` restarts on non-zero exits. On restart, `ExecStartPre` blocks until X is back.

## Design decisions and rejected approaches

### VAD (voice activity detection) — rejected

We initially implemented a VAD mode where the daemon would continuously transcribe during F1 hold, injecting text on each speech pause. This used `vad_simple()` from whisper.cpp's `common.cpp` — a simple energy-ratio check comparing the last 1s of audio against the full window.

Problems encountered:
1. **Hallucinations on silence**: Whisper generates plausible text ("Hello", "Thank you") when fed silent audio. The energy-based VAD couldn't reliably distinguish ambient noise from speech, so it kept triggering transcription on silence.
2. **Buffer management**: Each VAD-triggered transcription called `audio.clear()`, losing audio context. If VAD triggered too early (on noise), the actual speech that followed got split or lost.
3. **Timing sensitivity**: The VAD polls every 100ms checking a 2s window. Speech pauses shorter than the detection window were missed; pauses longer than expected caused double-triggers.

The simple hold-to-record, release-to-transcribe approach avoids all of these issues. The user is the VAD.

### XTest vs xdotool

We chose XTest (`XTestFakeKeyEvent`) directly rather than spawning `xdotool type`. Reasons:
- No process spawn overhead per injection (the daemon keeps an open X connection)
- Direct control over shift/modifier state
- No dependency on xdotool at runtime

The tradeoff is more complex code for UTF-8 handling and shift detection, but it's a one-time implementation cost.

### Separate X display connection

The daemon opens its own X11 connection (`xdpy`) independent of dwm's. This is necessary because:
- The daemon runs as a separate process (systemd service), not as part of dwm
- XTest events injected on any valid display connection are delivered to the focus window regardless of which client injected them
- This avoids any thread-safety issues with sharing dwm's display connection
