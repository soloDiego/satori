// The status socket: a one-way feed of the window list for bars.
//
//     satori status
//
// connects and prints one JSON line per change, oldest state first:
//
//     {"focused":0,"windows":[{"app_id":"foot","title":"~"},{"app_id":"zen","title":"..."}]}
//
// windows is creation order, newest first -- the Mod+J direction. focused is an
// index into it, -1 when nothing has focus.
//
// Clients only read. A new client gets the current line at once, so a bar that
// starts or restarts after satori is never blank. A client that stops reading
// is dropped rather than buffered for: the next line supersedes the last, so
// there is nothing worth keeping.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "satori.h"

// $XDG_RUNTIME_DIR/satori-<display>.sock. Keyed on the display so a nested
// test session never takes over the live session's socket.
static bool status_path(char *buf, size_t len) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!dir || !display) return false;
    const char *slash = strrchr(display, '/');   // WAYLAND_DISPLAY may be absolute
    if (slash) display = slash + 1;
    int n = snprintf(buf, len, "%s/satori-%s.sock", dir, display);
    return n > 0 && (size_t) n < len;
}

void status_init(struct satori *satori) {
    satori->status_fd = -1;
    for (size_t i = 0; i < SATORI_STATUS_CLIENTS; i++) satori->status_clients[i] = -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!status_path(addr.sun_path, sizeof addr.sun_path)) {
        satori_log("status: no XDG_RUNTIME_DIR or WAYLAND_DISPLAY, no socket\n");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        satori_log("status: socket: %s\n", strerror(errno));
        return;
    }
    // A socket someone answers on belongs to a live satori -- a second one
    // started on the same display, which is about to get `unavailable`. Leave
    // it alone. One nobody answers on is left over from a crash.
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe != -1 && connect(probe, (struct sockaddr *) &addr, sizeof addr) == 0) {
        satori_log("status: %s in use, no socket\n", addr.sun_path);
        close(probe);
        close(fd);
        return;
    }
    if (probe != -1) close(probe);
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) == -1 || listen(fd, 4) == -1) {
        satori_log("status: %s: %s\n", addr.sun_path, strerror(errno));
        close(fd);
        return;
    }
    satori->status_fd = fd;
    satori->status_dirty = true;
    satori_log("status: %s\n", addr.sun_path);
}

// Appends src as a JSON string body. Bytes >= 0x80 pass through: titles are
// UTF-8 and JSON is too.
static void json_string(FILE *out, const char *src) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) (src ? src : ""); *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(out, "\\%c", *p);
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

// The whole state as one line. Owned; NULL on allocation failure.
static char *status_line(const struct satori *satori, size_t *len) {
    char *buf = NULL;
    FILE *out = open_memstream(&buf, len);
    if (!out) return NULL;

    int focused = -1, i = 0;
    for (const struct window *win = satori->windows; win; win = win->next, i++) {
        if (win == satori->focused) focused = i;
    }
    fprintf(out, "{\"focused\":%d,\"windows\":[", focused);
    for (const struct window *win = satori->windows; win; win = win->next) {
        fputs("{\"app_id\":", out);
        json_string(out, win->app_id);
        fputs(",\"title\":", out);
        json_string(out, win->title);
        fputs(win->next ? "}," : "}", out);
    }
    fputs("]}\n", out);

    if (fclose(out) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

// False when the client has gone or stopped reading; the caller drops it.
static bool status_send(int fd, const char *line, size_t len) {
    ssize_t n = send(fd, line, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    return n == (ssize_t) len;
}

static void status_drop(struct satori *satori, size_t i) {
    close(satori->status_clients[i]);
    satori->status_clients[i] = -1;
    satori_log("status: client gone\n");
}

void status_accept(struct satori *satori) {
    // accept + fcntl rather than accept4, which is GNU and not in POSIX 2008.
    int fd = accept(satori->status_fd, NULL, NULL);
    if (fd == -1) return;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1 || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        close(fd);
        return;
    }

    // Nothing polls the clients, so one that hung up holds its slot until the
    // next send fails. A bar restarted a few times with the window list still
    // would fill the table; reap here, where a slot is actually needed.
    for (size_t i = 0; i < SATORI_STATUS_CLIENTS; i++) {
        char c;
        if (satori->status_clients[i] != -1
                && recv(satori->status_clients[i], &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
            status_drop(satori, i);
        }
    }

    for (size_t i = 0; i < SATORI_STATUS_CLIENTS; i++) {
        if (satori->status_clients[i] != -1) continue;
        satori->status_clients[i] = fd;
        satori_log("status: client\n");
        size_t len;
        char *line = status_line(satori, &len);
        if (line && !status_send(fd, line, len)) status_drop(satori, i);
        free(line);
        return;
    }
    satori_log("status: too many clients\n");
    close(fd);
}

void status_flush(struct satori *satori) {
    if (!satori->status_dirty) return;
    satori->status_dirty = false;

    size_t len;
    char *line = NULL;
    for (size_t i = 0; i < SATORI_STATUS_CLIENTS; i++) {
        if (satori->status_clients[i] == -1) continue;
        if (!line && !(line = status_line(satori, &len))) return;
        if (!status_send(satori->status_clients[i], line, len)) status_drop(satori, i);
    }
    free(line);
}

void status_destroy(struct satori *satori) {
    // No listener means no clients either -- and the client slots may not even
    // be initialised yet, so they must not be read.
    if (satori->status_fd == -1) return;
    for (size_t i = 0; i < SATORI_STATUS_CLIENTS; i++) {
        if (satori->status_clients[i] != -1) close(satori->status_clients[i]);
    }
    close(satori->status_fd);
    struct sockaddr_un addr;
    if (status_path(addr.sun_path, sizeof addr.sun_path)) unlink(addr.sun_path);
}

// `satori status`: copy the feed to stdout until satori goes away.
int status_client(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!status_path(addr.sun_path, sizeof addr.sun_path)) {
        fprintf(stderr, "satori status: XDG_RUNTIME_DIR and WAYLAND_DISPLAY must be set\n");
        return 1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1 || connect(fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
        fprintf(stderr, "satori status: %s: %s\n", addr.sun_path, strerror(errno));
        if (fd != -1) close(fd);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, (size_t) n, stdout) != (size_t) n) break;
        fflush(stdout);     // a bar reads line by line; never sit on one
    }
    close(fd);
    return 0;
}
