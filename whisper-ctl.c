// whisper-ctl: send commands to whisper-dictate daemon

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char *socket_path(void) {
    static char buf[512];
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime)
        snprintf(buf, sizeof(buf), "%s/whisper-dictate.sock", runtime);
    else
        snprintf(buf, sizeof(buf), "/tmp/whisper-dictate-%d.sock", getuid());
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "start") != 0 && strcmp(argv[1], "stop") != 0)) {
        fprintf(stderr, "usage: whisper-ctl start|stop\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // Daemon not running — silent fail so dwm doesn't stall
        close(fd);
        return 1;
    }

    write(fd, argv[1], strlen(argv[1]));
    close(fd);
    return 0;
}
