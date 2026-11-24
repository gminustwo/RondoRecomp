/* Discord status ("Rich Presence", discord.h).
 *
 * Only the Discord app on this computer is talked to, through its local IPC endpoint:
 * \\.\pipe\discord-ipc-N on Windows; a Unix socket discord-ipc-N in $XDG_RUNTIME_DIR, $TMPDIR
 * or /tmp on Linux and macOS (also where the Flatpak and Snap Discords and Vesktop put theirs).
 * Each frame is an opcode and a length (two little-endian u32s), then JSON: a handshake with
 * the application id, then SET_ACTIVITY commands; the app's PINGs are answered. Without the app
 * running it looks again every 20 s. One thread does all of the connection's I/O, so no read
 * of its own is ever waiting on the pipe when it asks how many bytes are there.
 *
 * Discord takes 5 updates in 20 s: a change goes out at most every 5 s, the latest one
 * winning. The status goes when the game closes (Discord clears it with the connection) or at
 * once when the option is turned off.
 *
 * The application (discord.com/developers/applications): its name is what the profile says
 * is being played, its Rich Presence art asset "icon" the picture. RONDO_DISCORD_APP_ID
 * overrides it (a test application). */
#ifndef _WIN32
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE        /* sockets, nanosleep, clock_gettime under -std=c11 */
#endif
#endif
#include "discord.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "programs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define DISCORD_APP_ID "1574637101792764015"

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };
enum { DS_NOT_STARTED, DS_OFF, DS_LOOKING, DS_CONNECTED, DS_REFUSED };
static SDL_AtomicInt g_state;

/* what the game wants shown (discord_set), and what was last sent and why the app said no
 * (for rondo.log): short copies under a spin lock */
static SDL_SpinLock g_lock;
static bool g_on;
static char g_details[128], g_status[128];
static char g_sent[256], g_refusal[160];

/* (the platform's own sleep and clock: the thread outlives SDL_Quit by a moment at exit) */
static void nap(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}

/* ---- the connection ----------------------------------------------------------------- */

#ifdef _WIN32
typedef HANDLE Conn;
#define NO_CONN INVALID_HANDLE_VALUE

static Conn conn_open(char *name, size_t cap)
{
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(name, cap, "discord-ipc-%d", i);
            return h;
        }
    }
    return NO_CONN;
}

static bool conn_write(Conn c, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(c, b, (DWORD)n, &w, NULL) || !w) return false;
        b += w, n -= w;
    }
    return true;
}

/* the bytes waiting, without waiting for any (-1: the pipe is gone) */
static long conn_avail(Conn c)
{
    DWORD a = 0;
    return PeekNamedPipe(c, NULL, 0, NULL, &a, NULL) ? (long)a : -1;
}

static bool conn_read(Conn c, void *p, size_t n)   /* (n bytes known to be waiting) */
{
    DWORD r = 0;
    return ReadFile(c, p, (DWORD)n, &r, NULL) && r == n;
}

static void conn_close(Conn c) { CloseHandle(c); }
static int self_pid(void) { return (int)GetCurrentProcessId(); }
#else
typedef int Conn;
#define NO_CONN (-1)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0         /* (macOS: SO_NOSIGPIPE on the socket instead) */
#endif

static Conn conn_open(char *name, size_t cap)
{
    const char *dirs[6];
    int nd = 0;
    static const char *const envs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    for (size_t i = 0; i < sizeof envs / sizeof *envs; i++) {
        const char *d = getenv(envs[i]);
        if (d && *d) dirs[nd++] = d;
    }
    dirs[nd++] = "/tmp";
    static const char *const subs[] = {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/",
                                       "snap.discord/", "snap.discord-canary/", ".flatpak/dev.vencord.Vesktop/xdg-run/"};
    for (int i = 0; i < 10; i++)
        for (int d = 0; d < nd; d++)
            for (size_t s = 0; s < sizeof subs / sizeof *subs; s++) {
                struct sockaddr_un a;
                memset(&a, 0, sizeof a);
                a.sun_family = AF_UNIX;
                int k = snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", dirs[d], subs[s], i);
                if (k <= 0 || k >= (int)sizeof a.sun_path || access(a.sun_path, F_OK)) continue;
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) return NO_CONN;
                fcntl(fd, F_SETFD, FD_CLOEXEC);   /* not kept open by Setup, which the game can become */
#ifdef SO_NOSIGPIPE
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
                    snprintf(name, cap, "%sdiscord-ipc-%d", subs[s], i);   /* (not the folder: no home path in logs) */
                    return fd;
                }
                close(fd);
            }
    return NO_CONN;
}

static bool conn_write(Conn c, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        ssize_t w = send(c, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w, n -= (size_t)w;
    }
    return true;
}

static long conn_avail(Conn c)
{
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 0) < 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    if (!(p.revents & (POLLIN | POLLHUP))) return 0;
    int n = 0;
    if (ioctl(c, FIONREAD, &n) < 0) return -1;
    return n > 0 ? n : -1;     /* (readable with nothing in it: closed) */
}

static bool conn_read(Conn c, void *p, size_t n)
{
    char *b = p;
    while (n) {
        ssize_t r = recv(c, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r, n -= (size_t)r;
    }
    return true;
}

static void conn_close(Conn c) { close(c); }
static int self_pid(void) { return (int)getpid(); }
#endif

/* ---- frames ------------------------------------------------------------------------- */

static Conn g_conn = NO_CONN;
static char g_in[8192];        /* the latest frame's JSON (cut short past this; NUL-terminated) */

static bool send_frame(uint32_t op, const char *json)
{
    size_t n = strlen(json);
    char *f = malloc(8 + n);
    if (!f) return false;
    uint32_t len = (uint32_t)n;   /* (little-endian, as every computer this runs on) */
    memcpy(f, &op, 4);
    memcpy(f + 4, &len, 4);
    memcpy(f + 8, json, n);
    bool ok = conn_write(g_conn, f, 8 + n);
    free(f);
    return ok;
}

/* A frame, if one comes within wait_ms: 1 (its opcode in *op, its JSON in g_in), 0 none, -1
 * the connection is gone. */
static int recv_frame(uint32_t *op, int wait_ms)
{
    long a;
    for (int waited = 0; (a = conn_avail(g_conn)) < 8; waited += 10) {
        if (a < 0) return -1;
        if (waited >= wait_ms) return 0;
        nap(10);
    }
    uint8_t h[8];
    uint32_t len;
    if (!conn_read(g_conn, h, 8)) return -1;
    memcpy(op, h, 4);
    memcpy(&len, h + 4, 4);
    if (len > (1u << 20)) return -1;
    size_t got = 0;
    for (int waited = 0; got < len;) {   /* (the app writes a frame at once: the rest is there or nearly) */
        if ((a = conn_avail(g_conn)) < 0) return -1;
        if (!a) {
            if ((waited += 5) > 2000) return -1;
            nap(5);
            continue;
        }
        char tmp[4096];
        size_t k = len - got < (size_t)a ? len - got : (size_t)a;
        if (k > sizeof tmp) k = sizeof tmp;
        if (!conn_read(g_conn, tmp, k)) return -1;
        if (got < sizeof g_in - 1) memcpy(g_in + got, tmp, got + k < sizeof g_in - 1 ? k : sizeof g_in - 1 - got);
        got += k;
    }
    g_in[len < sizeof g_in - 1 ? len : sizeof g_in - 1] = 0;
    return 1;
}

/* a JSON string's text from g_in ("message":"..."), for the log */
static void json_field(const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(g_in, pat);
    size_t k = 0;
    if (p)
        for (p += strlen(pat); *p && *p != '"' && k + 1 < cap; p++) {
            if (*p == '\\' && p[1]) p++;
            out[k++] = *p;
        }
    out[k] = 0;
}

/* s as the inside of a JSON string */
static void json_escape(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') out[k++] = '\\', out[k++] = (char)ch;
        else if (ch < 0x20) k += (size_t)snprintf(out + k, cap - k, "\\u%04x", ch);
        else out[k++] = (char)ch;
    }
    out[k] = 0;
}

/* ---- the status --------------------------------------------------------------------- */

static bool send_status(const char *details, const char *state, long long start_ms, int nonce)
{
    char d[256], s[256], j[1024];
    json_escape(d, sizeof d, details);
    json_escape(s, sizeof s, state);
    snprintf(j, sizeof j,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"details\":\"%s\",%s%s%s"
             "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"icon\","
             "\"large_text\":\"Rondo Recomp " RONDO_VERSION "\"}}},\"nonce\":\"%d\"}",
             self_pid(), d, *s ? "\"state\":\"" : "", s, *s ? "\"," : "", start_ms, nonce);
    return send_frame(OP_FRAME, j);
}

static void drop(void)
{
    if (g_conn != NO_CONN) conn_close(g_conn);
    g_conn = NO_CONN;
}

static int SDLCALL discord_thread(void *arg)
{
    const char *id = arg;
    char name[96] = "", sent_d[128] = "", sent_s[128] = "";
    bool sent_any = false, said_missing = false, said_refused = false;
    uint64_t next_try = 0, last_send = 0;
    int nonce = 0, errors = 0;
    long long start_ms = (long long)time(NULL) * 1000;   /* the profile's "elapsed": since the game started */
    for (;;) {
        char details[128], state[128];
        SDL_LockSpinlock(&g_lock);
        bool on = g_on;
        memcpy(details, g_details, sizeof details);
        memcpy(state, g_status, sizeof state);
        SDL_UnlockSpinlock(&g_lock);
        uint64_t now = now_ms();

        if (!on) {
            if (g_conn != NO_CONN) {   /* turned off: cleared at once */
                char j[128];
                snprintf(j, sizeof j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"%d\"}", self_pid(),
                         ++nonce);
                send_frame(OP_FRAME, j);
                drop();
                fprintf(stderr, "[discord] off: the status is cleared\n");
            }
            SDL_SetAtomicInt(&g_state, DS_OFF);
            next_try = 0;
            sent_any = false;
            nap(500);
            continue;
        }

        if (g_conn == NO_CONN) {
            if (SDL_GetAtomicInt(&g_state) != DS_REFUSED) SDL_SetAtomicInt(&g_state, DS_LOOKING);
            if (now < next_try) {
                nap(500);
                continue;
            }
            next_try = now + 20000;
            if ((g_conn = conn_open(name, sizeof name)) == NO_CONN) {
                if (!said_missing) fprintf(stderr, "[discord] the Discord app isn't running: looking for it every 20 s\n");
                said_missing = true;
                continue;
            }
            char j[160];
            uint32_t op = 0;
            snprintf(j, sizeof j, "{\"v\":1,\"client_id\":\"%s\"}", id);
            int r = send_frame(OP_HANDSHAKE, j) ? recv_frame(&op, 5000) : -1;
            if (r > 0 && op == OP_FRAME && strstr(g_in, "\"READY\"")) {   /* (its user is not logged: privacy) */
                fprintf(stderr, "[discord] connected to the Discord app (%s)\n", name);
                SDL_SetAtomicInt(&g_state, DS_CONNECTED);
                said_missing = said_refused = sent_any = false;
                errors = 0;
            } else {
                char msg[160];
                json_field("message", msg, sizeof msg);
                if (r > 0 && op == OP_CLOSE) {
                    if (!said_refused) fprintf(stderr, "[discord] the Discord app said no: %s\n", msg[0] ? msg : "(no reason)");
                    said_refused = true;
                    SDL_LockSpinlock(&g_lock);
                    snprintf(g_refusal, sizeof g_refusal, "%s", msg[0] ? msg : "no reason given");
                    SDL_UnlockSpinlock(&g_lock);
                    SDL_SetAtomicInt(&g_state, DS_REFUSED);
                } else fprintf(stderr, "[discord] no answer from the Discord app (%s)\n", name);
                drop();
                continue;
            }
        }

        /* what came in: pings to answer, errors, the app closing */
        uint32_t op = 0;
        int r;
        while ((r = recv_frame(&op, 0)) > 0) {
            if (op == OP_PING) send_frame(OP_PONG, g_in);
            else if (op == OP_CLOSE) {
                r = -1;
                break;
            } else if (op == OP_FRAME && strstr(g_in, "\"evt\":\"ERROR\"") && errors < 20) {
                char msg[160];
                json_field("message", msg, sizeof msg);
                fprintf(stderr, "[discord] the Discord app: %s%s\n", msg, ++errors == 20 ? " (no more of these)" : "");
            }
        }
        if (r < 0) {
            fprintf(stderr, "[discord] the Discord app closed the connection: looking for it every 20 s\n");
            drop();
            said_missing = true;
            next_try = now + 20000;
            continue;
        }

        if (details[0] && (!sent_any || strcmp(details, sent_d) || strcmp(state, sent_s)) &&
            (!sent_any || now - last_send >= 5000)) {
            if (!send_status(details, state, start_ms, ++nonce)) {
                fprintf(stderr, "[discord] the Discord app closed the connection: looking for it every 20 s\n");
                drop();
                continue;
            }
            last_send = now, sent_any = true;
            snprintf(sent_d, sizeof sent_d, "%s", details);
            snprintf(sent_s, sizeof sent_s, "%s", state);
            SDL_LockSpinlock(&g_lock);
            snprintf(g_sent, sizeof g_sent, "%s%s%s", details, state[0] ? " | " : "", state);
            SDL_UnlockSpinlock(&g_lock);
            fprintf(stderr, "[discord] status: %s%s%s\n", details, state[0] ? " | " : "", state);
        }
        nap(250);
    }
    return 0;
}

void discord_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    const char *e = SDL_getenv("RONDO_DISCORD_APP_ID");
    static char id[32];
    snprintf(id, sizeof id, "%s", e && *e ? e : DISCORD_APP_ID);
    SDL_SetAtomicInt(&g_state, DS_OFF);
    SDL_Thread *t = SDL_CreateThread(discord_thread, "discord", id);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&g_state, DS_NOT_STARTED);
}

void discord_set(bool on, const char *details, const char *state)
{
    SDL_LockSpinlock(&g_lock);
    g_on = on;
    if (strcmp(details, g_details)) snprintf(g_details, sizeof g_details, "%s", details);
    if (strcmp(state, g_status)) snprintf(g_status, sizeof g_status, "%s", state);
    SDL_UnlockSpinlock(&g_lock);
}

const char *discord_status_text(void)
{
    static char text[320];
    SDL_LockSpinlock(&g_lock);
    switch (SDL_GetAtomicInt(&g_state)) {
    case DS_CONNECTED:
        snprintf(text, sizeof text, "shown on Discord (last: %s)", g_sent[0] ? g_sent : "nothing yet");
        break;
    case DS_LOOKING: snprintf(text, sizeof text, "the Discord app isn't running on this computer"); break;
    case DS_REFUSED: snprintf(text, sizeof text, "the Discord app turned it away: %s", g_refusal); break;
    case DS_OFF: snprintf(text, sizeof text, "off (Gameplay: Discord status)"); break;
    default: snprintf(text, sizeof text, "not started (test run)"); break;
    }
    SDL_UnlockSpinlock(&g_lock);
    return text;
}
