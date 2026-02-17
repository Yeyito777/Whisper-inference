# Whisper-inference Quickstart

## Project Layout

```
Whisper-inference/
  Makefile        -- Build wrapper (configure, build, install, clean, rebuild)
  run.sh          -- TUI model selector + transcription launcher
  src/            -- whisper.cpp source (git submodule)
  whispercpp/     -- Compiled binaries + shared libs
  models/         -- GGML model files (.bin)
  reference/      -- Documentation
```

## Building

From the project root:

```bash
make            # configure + build + install (first time)
make rebuild    # clean + full rebuild
make clean      # remove build artifacts and binaries
```

Binaries land in `whispercpp/`. The build enables **Vulkan** (`-DGGML_VULKAN=1`) by default.

To update whisper.cpp:

```bash
cd src && git pull && cd ..
make rebuild
```

## Downloading Models

Models are GGML `.bin` files. Download from HuggingFace:

```bash
# Using the bundled script
sh src/models/download-ggml-model.sh large-v3
sh src/models/download-ggml-model.sh large-v3-turbo

# Then move to models/
mv src/models/ggml-large-v3.bin models/
mv src/models/ggml-large-v3-turbo.bin models/
```

Or download directly with `hf`:

```bash
hf download ggerganov/whisper.cpp ggml-large-v3.bin --local-dir models/
hf download ggerganov/whisper.cpp ggml-large-v3-turbo.bin --local-dir models/
```

Available models: `tiny.en`, `tiny`, `base.en`, `base`, `small.en`, `small`, `medium.en`, `medium`, `large-v1`, `large-v2`, `large-v3`, `large-v3-turbo`

Quantized variants also available (e.g. `ggml-large-v3-turbo-q5_0.bin`).

## Running Transcription

All commands need `LD_LIBRARY_PATH` set so binaries find their shared libs:

```bash
export LD_LIBRARY_PATH=~/Workspace/Whisper-inference/whispercpp
```

### Using run.sh (TUI)

```bash
./run.sh recording.wav          # select model, transcribe file
./run.sh                        # select model, then enter file path
```

### Direct CLI

```bash
whispercpp/whisper-cli -m models/ggml-large-v3.bin -f audio.wav
```

### Audio Format

Whisper requires **16-bit WAV, 16kHz, mono**. Convert with ffmpeg:

```bash
ffmpeg -i input.mp3 -ar 16000 -ac 1 -c:a pcm_s16le output.wav
```

## Key Flags

| Flag | Purpose |
|------|---------|
| `-m <path>` | Path to model file |
| `-f <path>` | Audio file to transcribe |
| `-t <N>` | Number of CPU threads |
| `-l <lang>` | Language code (e.g. `en`, `es`, `auto`) |
| `--translate` | Translate to English |
| `-otxt` | Output plain text file |
| `-osrt` | Output SRT subtitle file |
| `-ovtt` | Output VTT subtitle file |
| `-ocsv` | Output CSV file |
| `-ojf` | Output JSON (full) |
| `--print-colors` | Colorize output by confidence |
| `--no-timestamps` | Omit timestamps |
| `--prompt <text>` | Initial prompt to guide transcription style |

## GPU & Hardware Notes

**GPU:** AMD Radeon RX 5700 XT (RDNA 1 / Navi 10 / gfx1010)
**Backend:** Vulkan via RADV (Mesa)
**VRAM:** 8 GB

### Model Sizes

| Model | Disk | VRAM (f16) | Parameters | Relative Speed |
|-------|------|------------|------------|----------------|
| tiny | 75 MB | ~400 MB | 39M | Fastest |
| base | 142 MB | ~500 MB | 74M | Fast |
| small | 466 MB | ~1 GB | 244M | Medium |
| medium | 1.5 GB | ~3 GB | 769M | Slower |
| large-v3 | 3.1 GB | ~6 GB | 1.55B | Slowest, best accuracy |
| large-v3-turbo | 1.6 GB | ~3 GB | 809M | ~6x faster than large-v3 |

### Quantized Models

| Quant | Size Reduction | Quality | Notes |
|-------|---------------|---------|-------|
| q5_0 | ~45% smaller | Near-original | Best default for constrained VRAM |
| q8_0 | ~25% smaller | Excellent | Negligible quality loss |

## Useful Binaries

| Binary | Purpose |
|--------|---------|
| `whisper-cli` | Transcribe audio files |
| `whisper-server` | HTTP API for transcription |
| `whisper-bench` | Performance benchmarking |
| `whisper-stream` | Real-time microphone transcription (needs SDL2) |
