// whisper-dictate: push-to-talk speech daemon
//
// Loads a whisper model into VRAM on startup, listens on a Unix socket for
// start/stop commands. On "start" captures mic audio, on "stop" transcribes
// and injects text into the focused window via XTest.

#include "common-sdl.h"
#include "whisper.h"
#include "ggml-backend.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>

// ----- config -----

struct Config {
    int capture_id   = -1;
    std::string model = "models/ggml-large-v3-turbo.bin";
    std::string language = "en";
    std::string prompt;
    std::vector<std::pair<std::string, std::string>> replacements;
};

static std::string str_trim(const std::string &s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static Config load_config(const std::string &exe_dir) {
    Config cfg;
    std::string path = exe_dir + "/dictate.conf";
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "dictate: no config at %s, using defaults\n", path.c_str());
        return cfg;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = str_trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = str_trim(line.substr(0, eq));
        std::string val = str_trim(line.substr(eq + 1));
        if (key == "capture_id") cfg.capture_id = std::stoi(val);
        else if (key == "model")    cfg.model = val;
        else if (key == "language") cfg.language = val;
        else if (key == "prompt")   cfg.prompt = val;
        else if (key == "replace") {
            auto arrow = val.find("->");
            if (arrow != std::string::npos)
                cfg.replacements.push_back({
                    str_trim(val.substr(0, arrow)),
                    str_trim(val.substr(arrow + 2))
                });
        }
    }
    return cfg;
}

// ----- XTest text injection -----

static Display *xdpy = nullptr;
static jmp_buf x_error_jmp;
static std::string pending_text;

static int on_x_io_error(Display *) {
    xdpy = nullptr;
    longjmp(x_error_jmp, 1);
    return 0;
}

static bool xtest_connect() {
    xdpy = XOpenDisplay(nullptr);
    if (!xdpy) return false;
    XSetIOErrorHandler(on_x_io_error);
    int ev, err, maj, min;
    if (!XTestQueryExtension(xdpy, &ev, &err, &maj, &min)) {
        XCloseDisplay(xdpy);
        xdpy = nullptr;
        return false;
    }
    return true;
}

static void xtest_type(const std::string &text) {
    if (!xdpy || text.empty()) return;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = text[i];
        uint32_t cp = 0;
        int bytes = 0;
        if (c < 0x80)      { cp = c;           bytes = 1; }
        else if (c < 0xC0) { i++; continue; }
        else if (c < 0xE0) { cp = c & 0x1F;    bytes = 2; }
        else if (c < 0xF0) { cp = c & 0x0F;    bytes = 3; }
        else               { cp = c & 0x07;    bytes = 4; }

        for (int j = 1; j < bytes && (i + j) < text.size(); j++)
            cp = (cp << 6) | (text[i + j] & 0x3F);
        i += bytes;

        KeySym ks;
        if (cp < 0x100) ks = cp;
        else            ks = 0x01000000 | cp;

        KeyCode kc = XKeysymToKeycode(xdpy, ks);
        if (kc == 0) continue;

        bool need_shift = false;
        if (cp >= 'A' && cp <= 'Z') need_shift = true;
        KeySym lower = XKeycodeToKeysym(xdpy, kc, 0);
        KeySym upper = XKeycodeToKeysym(xdpy, kc, 1);
        if (lower != ks && upper == ks) need_shift = true;

        if (need_shift)
            XTestFakeKeyEvent(xdpy, XKeysymToKeycode(xdpy, XK_Shift_L), True, 0);
        XTestFakeKeyEvent(xdpy, kc, True, 0);
        XTestFakeKeyEvent(xdpy, kc, False, 0);
        if (need_shift)
            XTestFakeKeyEvent(xdpy, XKeysymToKeycode(xdpy, XK_Shift_L), False, 0);
    }
    XFlush(xdpy);
}

// ----- sound effects -----

static SDL_AudioDeviceID playback_dev = 0;
static Uint8 *snd_connected_buf = nullptr;
static Uint32 snd_connected_len = 0;
static Uint8 *snd_disconnected_buf = nullptr;
static Uint32 snd_disconnected_len = 0;

static void load_sounds(const std::string &exe_dir) {
    SDL_AudioSpec spec;
    std::string path;

    path = exe_dir + "/assets/connected.wav";
    if (!SDL_LoadWAV(path.c_str(), &spec, &snd_connected_buf, &snd_connected_len)) {
        fprintf(stderr, "dictate: failed to load %s: %s\n", path.c_str(), SDL_GetError());
        return;
    }

    SDL_AudioSpec obtained;
    playback_dev = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained, 0);
    if (playback_dev == 0) {
        fprintf(stderr, "dictate: failed to open playback device: %s\n", SDL_GetError());
        SDL_FreeWAV(snd_connected_buf);
        snd_connected_buf = nullptr;
        return;
    }

    path = exe_dir + "/assets/disconnected.wav";
    if (!SDL_LoadWAV(path.c_str(), &spec, &snd_disconnected_buf, &snd_disconnected_len)) {
        fprintf(stderr, "dictate: failed to load %s: %s\n", path.c_str(), SDL_GetError());
    }
}

static void play_sound(Uint8 *buf, Uint32 len) {
    if (!playback_dev || !buf) return;
    SDL_ClearQueuedAudio(playback_dev);
    SDL_QueueAudio(playback_dev, buf, len);
    SDL_PauseAudioDevice(playback_dev, 0);
}

// ----- socket -----

static std::string socket_path() {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime) return std::string(runtime) + "/whisper-dictate.sock";
    return std::string("/tmp/whisper-dictate-") + std::to_string(getuid()) + ".sock";
}

static int create_socket() {
    std::string path = socket_path();
    unlink(path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); exit(1);
    }
    chmod(path.c_str(), 0700);
    if (listen(fd, 4) < 0) { perror("listen"); close(fd); exit(1); }

    fprintf(stderr, "dictate: listening on %s\n", path.c_str());
    return fd;
}

// ----- main -----

static volatile sig_atomic_t running = 1;
static void sighandler(int) { running = 0; }

static void list_devices() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return;
    }
    int n = SDL_GetNumAudioDevices(SDL_TRUE);
    printf("Capture devices:\n");
    for (int i = 0; i < n; i++)
        printf("  %d: %s\n", i, SDL_GetAudioDeviceName(i, SDL_TRUE));
    SDL_Quit();
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list-devices") == 0) {
        list_devices();
        return 0;
    }

    // Resolve exe directory for config + model paths
    char exe_buf[4096];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    std::string exe_dir;
    if (len > 0) {
        exe_buf[len] = '\0';
        exe_dir = dirname(exe_buf);
    } else {
        exe_dir = ".";
    }

    Config cfg = load_config(exe_dir);

    std::string model_path = cfg.model;
    if (!model_path.empty() && model_path[0] != '/')
        model_path = exe_dir + "/" + model_path;

    fprintf(stderr, "dictate: model=%s capture=%d lang=%s\n",
            model_path.c_str(), cfg.capture_id, cfg.language.c_str());

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    // Init backends and load model
    ggml_backend_load_all();

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = true;
    cparams.flash_attn = true;

    fprintf(stderr, "dictate: loading model...\n");
    struct whisper_context *ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        fprintf(stderr, "dictate: failed to load model: %s\n", model_path.c_str());
        return 1;
    }
    fprintf(stderr, "dictate: model loaded\n");

    // Init SDL audio — 30 second buffer, starts paused
    const int audio_buf_ms = 30000;
    audio_async audio(audio_buf_ms);
    if (!audio.init(cfg.capture_id, WHISPER_SAMPLE_RATE)) {
        fprintf(stderr, "dictate: audio init failed\n");
        whisper_free(ctx);
        return 1;
    }
    fprintf(stderr, "dictate: audio ready (paused)\n");

    load_sounds(exe_dir);

    if (!xtest_connect()) {
        fprintf(stderr, "dictate: cannot open X display\n");
        whisper_free(ctx);
        return 1;
    }

    int sock_fd = create_socket();

    volatile bool recording = false;
    struct pollfd pfd = { .fd = sock_fd, .events = POLLIN, .revents = 0 };

    fprintf(stderr, "dictate: ready\n");

    if (setjmp(x_error_jmp) != 0) {
        fprintf(stderr, "dictate: X server connection lost\n");
        if (recording) {
            audio.pause();
            recording = false;
        }
        fprintf(stderr, "dictate: waiting for X server...\n");
        while (running) {
            sleep(2);
            if (xtest_connect()) {
                fprintf(stderr, "dictate: X server reconnected\n");
                break;
            }
        }
        if (!pending_text.empty()) {
            xtest_type(pending_text);
            pending_text.clear();
        }
    }

    while (running) {
        int ret = poll(&pfd, 1, 200);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        int client = accept(sock_fd, nullptr, nullptr);
        if (client < 0) continue;

        char buf[32] = {};
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        close(client);
        if (n <= 0) continue;

        std::string cmd = str_trim(std::string(buf, n));

        if (cmd == "start" && !recording) {
            fprintf(stderr, "dictate: recording started\n");
            play_sound(snd_connected_buf, snd_connected_len);
            audio.resume();
            audio.clear();
            recording = true;
        }
        else if (cmd == "stop" && recording) {
            recording = false;
            fprintf(stderr, "dictate: recording stopped, transcribing...\n");
            play_sound(snd_disconnected_buf, snd_disconnected_len);

            SDL_Delay(100);

            std::vector<float> pcm;
            audio.get(audio_buf_ms, pcm);
            audio.pause();

            if (pcm.empty()) {
                fprintf(stderr, "dictate: no audio captured\n");
                continue;
            }

            auto t0 = std::chrono::high_resolution_clock::now();

            whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            wparams.print_progress   = false;
            wparams.print_special    = false;
            wparams.print_realtime   = false;
            wparams.print_timestamps = false;
            wparams.translate        = false;
            wparams.single_segment   = false;
            wparams.no_context       = true;
            wparams.no_timestamps    = true;
            wparams.language         = cfg.language.c_str();
            wparams.initial_prompt   = cfg.prompt.empty() ? nullptr : cfg.prompt.c_str();
            wparams.n_threads        = 4;

            if (whisper_full(ctx, wparams, pcm.data(), pcm.size()) != 0) {
                fprintf(stderr, "dictate: transcription failed\n");
                continue;
            }

            auto t1 = std::chrono::high_resolution_clock::now();
            float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

            std::string text;
            int nseg = whisper_full_n_segments(ctx);
            for (int i = 0; i < nseg; i++) {
                const char *seg = whisper_full_get_segment_text(ctx, i);
                if (seg) text += seg;
            }

            text = str_trim(text);

            if (text.empty()) {
                fprintf(stderr, "dictate: (silence) [%.0f ms]\n", ms);
                continue;
            }

            for (auto &[from, to] : cfg.replacements) {
                size_t pos = 0;
                while ((pos = [&]{
                    for (size_t i = pos; i + from.size() <= text.size(); i++) {
                        bool match = true;
                        for (size_t j = 0; j < from.size(); j++) {
                            if (tolower(text[i+j]) != tolower(from[j])) { match = false; break; }
                        }
                        if (match) return i;
                    }
                    return std::string::npos;
                }()) != std::string::npos) {
                    text.replace(pos, from.size(), to);
                    pos += to.size();
                }
            }

            fprintf(stderr, "dictate: \"%s\" [%.0f ms]\n", text.c_str(), ms);
            pending_text = text;
            xtest_type(text);
            pending_text.clear();
        }
    }

    if (playback_dev) SDL_CloseAudioDevice(playback_dev);
    if (snd_connected_buf) SDL_FreeWAV(snd_connected_buf);
    if (snd_disconnected_buf) SDL_FreeWAV(snd_disconnected_buf);

    std::string path = socket_path();
    unlink(path.c_str());
    close(sock_fd);
    if (xdpy) XCloseDisplay(xdpy);
    whisper_free(ctx);
    fprintf(stderr, "dictate: shutdown\n");
    return 0;
}
