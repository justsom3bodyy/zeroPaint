/* zp_platform.c - linux clipboard and file dialog pipes (wayland + x11) */
#include "zp_platform.h"
#include "zp_codec.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

/* === subprocess pipe helpers === */
/* fork+execvp directly, never pass user strings thru sh -c */
static bool zp_pipe_write_cmd(char *const argv[], const uint8_t *data, size_t len) {
    int pfd[2];
    if (pipe(pfd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[0]);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(pfd[1], data + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(pfd[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static uint8_t *zp_pipe_read_cmd(char *const argv[], size_t *out_len) {
    int pfd[2];
    if (pipe(pfd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t cap = 65536, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        close(pfd[0]);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            if (cap > 32 * 1024 * 1024) break;
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        ssize_t n = read(pfd[0], buf + len, 4096);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!buf || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || len == 0) {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* === clipboard (wayland first, then x11) === */
bool zp_platform_clipboard_copy(const ZPImage *img) {
    size_t len = 0;
    uint8_t *png = zp_encode_png_mem(img, &len);
    if (!png) return false;
    bool ok = false;
    if (getenv("WAYLAND_DISPLAY")) {
        char *argv[] = { "wl-copy", "--type", "image/png", NULL };
        ok = zp_pipe_write_cmd(argv, png, len);
    }
    if (!ok) {
        char *argv[] = { "xclip", "-selection", "clipboard", "-t", "image/png", "-i", NULL };
        ok = zp_pipe_write_cmd(argv, png, len);
    }
    free(png);
    return ok;
}

bool zp_platform_clipboard_paste(ZPImage *out) {
    size_t len = 0;
    uint8_t *raw = NULL;
    if (getenv("WAYLAND_DISPLAY")) {
        char *argv[] = { "wl-paste", "--type", "image/bmp", NULL };
        raw = zp_pipe_read_cmd(argv, &len);
    }
    if (!raw) {
        char *argv[] = { "xclip", "-selection", "clipboard", "-t", "image/bmp", "-o", NULL };
        raw = zp_pipe_read_cmd(argv, &len);
    }
    if (!raw) return false;
    bool ok = zp_decode_bmp_mem(raw, len, out);
    free(raw);
    return ok;
}

/* === native file picker fallback === */
/* tries zenity then kdialog; if neither exists the built-in modal takes over */
bool zp_platform_file_dialog(bool save_mode, char *out_path, size_t out_cap) {
    size_t len = 0;
    uint8_t *res = NULL;
    if (save_mode) {
        char *z_argv[] = { "zenity", "--file-selection", "--save", "--confirm-overwrite", "--filename=untitled.png", NULL };
        res = zp_pipe_read_cmd(z_argv, &len);
        if (!res) {
            char *k_argv[] = { "kdialog", "--getsavefilename", "untitled.png", NULL };
            res = zp_pipe_read_cmd(k_argv, &len);
        }
    } else {
        char *z_argv[] = { "zenity", "--file-selection", "--title=Open Image - 0Paint", NULL };
        res = zp_pipe_read_cmd(z_argv, &len);
        if (!res) {
            char *k_argv[] = { "kdialog", "--getopenfilename", ".", NULL };
            res = zp_pipe_read_cmd(k_argv, &len);
        }
    }
    if (!res) return false;
    while (len > 0 && (res[len - 1] == '\n' || res[len - 1] == '\r')) len--;
    if (len == 0 || len >= out_cap) {
        free(res);
        return false;
    }
    memcpy(out_path, res, len);
    out_path[len] = '\0';
    free(res);
    return true;
}
