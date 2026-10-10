/* term_link for the simulator: runs the user's shell on a pseudo-terminal.
 * Lets the terminal emulator be exercised with real programs (htop, vim,
 * tmux) on the Mac before any SSH exists. */
#include "term_link.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

static int s_fd = -1;
static pid_t s_pid;
static link_state_t s_state = LINK_IDLE;
static char s_status[96];

bool link_has_local(void) { return true; }

static int s_cols, s_rows;

/* SIM_HOSTKEY=new|changed: ask about the host key before "connecting", so
 * the fingerprint dialogs can be exercised. */
static bool spawn(int cols, int rows);

bool link_open(const link_params_t *p, int cols, int rows)
{
    (void)p;
    const char *hk = getenv("SIM_HOSTKEY");
    if (hk && hk[0]) {
        s_cols  = cols;
        s_rows  = rows;
        s_state = LINK_VERIFY_HOST;
        snprintf(s_status, sizeof(s_status), "verify host key");
        return true;
    }
    return spawn(cols, rows);
}

static bool spawn(int cols, int rows)
{
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    s_pid             = forkpty(&s_fd, NULL, NULL, &ws);
    if (s_pid < 0) {
        snprintf(s_status, sizeof(s_status), "forkpty failed: %s", strerror(errno));
        s_state = LINK_CLOSED;
        return false;
    }
    if (s_pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        const char *sh = getenv("SHELL");
        execl(sh ? sh : "/bin/zsh", sh ? sh : "/bin/zsh", "-l", (char *)NULL);
        _exit(127);
    }
    fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL) | O_NONBLOCK);
    s_state = LINK_OPEN;
    snprintf(s_status, sizeof(s_status), "local shell");
    return true;
}

void link_write(const char *d, size_t n)
{
    if (s_fd >= 0 && write(s_fd, d, n) < 0) { /* ignore: the reader notices EOF */ }
}

void link_resize(int cols, int rows)
{
    if (s_fd < 0) return;
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    ioctl(s_fd, TIOCSWINSZ, &ws);
}

void link_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        kill(s_pid, SIGHUP);
        waitpid(s_pid, NULL, WNOHANG);
    }
    s_fd    = -1;
    s_state = LINK_IDLE;
}

size_t link_read(char *buf, size_t max)
{
    if (s_fd < 0) return 0;
    ssize_t n = read(s_fd, buf, max);
    if (n > 0) return (size_t)n;
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(s_fd);
        s_fd    = -1;
        s_state = LINK_CLOSED;
        snprintf(s_status, sizeof(s_status), "shell exited");
    }
    return 0;
}

link_state_t link_state(void) { return s_state; }
const char *link_status(void) { return s_status; }
const char *link_fingerprint(void) { return "SHA256:Xq3v9bK2mJ7tR1pLw8sZ4nYc6hDf0aGe5uIo2kVxNQE"; }
bool link_hostkey_changed(void)
{
    const char *hk = getenv("SIM_HOSTKEY");
    return hk && strcmp(hk, "changed") == 0;
}
void link_hostkey_decide(bool accept)
{
    if (s_state != LINK_VERIFY_HOST) return;
    if (accept) {
        spawn(s_cols, s_rows);
    } else {
        s_state = LINK_CLOSED;
        snprintf(s_status, sizeof(s_status), "host key rejected");
    }
}
const char *link_pubkey(void) { return "ecdsa-sha2-nistp256 AAAA... (simulator)"; }
