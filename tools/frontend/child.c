/* See child.h. */
#include "child.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

struct Child {
    HANDLE proc, out;
};

/* one argument, quoted as the C runtime splits a command line again */
static bool add_arg(char **cmd, size_t *len, size_t *cap, const char *arg)
{
    size_t need = *len + strlen(arg) * 2 + 4;
    if (need > *cap) {
        size_t c = *cap ? *cap : 1024;
        while (c < need) c *= 2;
        char *b = SDL_realloc(*cmd, c);
        if (!b) return false;
        *cmd = b;
        *cap = c;
    }
    char *o = *cmd + *len;
    if (*len) *o++ = ' ';
    bool plain = *arg && !strpbrk(arg, " \t\"");
    if (plain) {
        strcpy(o, arg);
        o += strlen(arg);
    } else {
        *o++ = '"';
        for (const char *p = arg;; p++) {
            size_t slashes = 0;
            while (*p == '\\') { slashes++; p++; }
            if (!*p) { for (size_t i = 0; i < slashes * 2; i++) *o++ = '\\'; break; }
            if (*p == '"') { for (size_t i = 0; i < slashes * 2 + 1; i++) *o++ = '\\'; }
            else { for (size_t i = 0; i < slashes; i++) *o++ = '\\'; }
            *o++ = *p;
        }
        *o++ = '"';
    }
    *o = 0;
    *len = (size_t)(o - *cmd);
    return true;
}

/* NAME=value, by name as Windows orders an environment (case aside; the name's end first) */
static int env_order(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    for (;; x++, y++) {
        int cx = *x == '=' ? 0 : SDL_toupper((unsigned char)*x), cy = *y == '=' ? 0 : SDL_toupper((unsigned char)*y);
        if (cx != cy || !cx) return cx - cy;
    }
}

/* The environment as SDL keeps it, which SDL_SetEnvironmentVariable changes (Setup: zig's
 * cache and lib folders) but which CreateProcessW would not pass on by itself, as an
 * environment block; NULL: the process's own. (SDL_CreateProcess, elsewhere, passes it.) */
static wchar_t *env_block(void)
{
    char **vars = SDL_GetEnvironmentVariables(SDL_GetEnvironment());
    if (!vars) return NULL;
    size_t n = 0, total = 1;
    while (vars[n]) total += (size_t)MultiByteToWideChar(CP_UTF8, 0, vars[n++], -1, NULL, 0);
    wchar_t *block = n ? SDL_malloc(total * sizeof *block) : NULL, *p = block;
    SDL_qsort(vars, n, sizeof *vars, env_order);
    for (size_t i = 0; block && i < n; i++)
        p += MultiByteToWideChar(CP_UTF8, 0, vars[i], -1, p, (int)(total - (size_t)(p - block)));
    if (block) *p = 0;
    SDL_free(vars);
    return block;
}

Child *child_start(const char *const *args, char *err, size_t errlen)
{
    char *cmd = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; args[i]; i++)
        if (!add_arg(&cmd, &len, &cap, args[i])) { SDL_free(cmd); snprintf(err, errlen, "out of memory"); return NULL; }
    int wn = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
    wchar_t *wcmd = SDL_malloc((size_t)wn * sizeof(wchar_t));
    if (!wcmd) { SDL_free(cmd); snprintf(err, errlen, "out of memory"); return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, wn);
    SDL_free(cmd);

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    Child *c = SDL_calloc(1, sizeof *c);
    if (!c || !CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        snprintf(err, errlen, "cannot make a pipe (error %lu)", (unsigned long)GetLastError());
        SDL_free(c);
        SDL_free(wcmd);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   /* only the child's end is inherited */
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    wchar_t *env = env_block();
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env, NULL,
                             &si, &pi);
    DWORD e = GetLastError();
    SDL_free(env);
    SDL_free(wcmd);
    CloseHandle(wr);   /* the child has its own: the pipe ends when it (and anything it started) does */
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        snprintf(err, errlen, "cannot start %s (error %lu)", args[0], (unsigned long)e);
        CloseHandle(rd);
        SDL_free(c);
        return NULL;
    }
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess;
    c->out = rd;
    return c;
}

int child_read(Child *c, char *buf, int n)
{
    if (!c->out) return -1;
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(c->out, NULL, 0, NULL, &avail, NULL)) return -1;   /* closed: no more output */
    if (!avail) return 0;
    if (avail > (DWORD)n) avail = (DWORD)n;
    if (!ReadFile(c->out, buf, avail, &got, NULL)) return -1;
    return (int)got;
}

bool child_wait(Child *c, bool block, int *code)
{
    if (WaitForSingleObject(c->proc, block ? INFINITE : 0) != WAIT_OBJECT_0) return false;
    DWORD rc = 1;
    GetExitCodeProcess(c->proc, &rc);
    if (code) *code = (int)rc;
    return true;
}

void child_free(Child *c)
{
    if (!c) return;
    if (c->out) CloseHandle(c->out);
    if (c->proc) CloseHandle(c->proc);
    SDL_free(c);
}

#else
/* ---- elsewhere: SDL's processes (no console windows to avoid) ---- */
struct Child {
    SDL_Process *proc;
    SDL_IOStream *out;
};

Child *child_start(const char *const *args, char *err, size_t errlen)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) { snprintf(err, errlen, "cannot start %s: %s", args[0], SDL_GetError()); return NULL; }
    Child *c = SDL_calloc(1, sizeof *c);
    if (!c) { SDL_DestroyProcess(p); snprintf(err, errlen, "out of memory"); return NULL; }
    c->proc = p;
    c->out = SDL_GetPointerProperty(SDL_GetProcessProperties(p), SDL_PROP_PROCESS_STDOUT_POINTER, NULL);
    return c;
}

int child_read(Child *c, char *buf, int n)
{
    if (!c->out) return -1;
    size_t got = SDL_ReadIO(c->out, buf, (size_t)n);
    if (got > 0) return (int)got;
    return SDL_GetIOStatus(c->out) == SDL_IO_STATUS_NOT_READY ? 0 : -1;
}

bool child_wait(Child *c, bool block, int *code)
{
    int rc = 1;
    if (!SDL_WaitProcess(c->proc, block, &rc)) return false;
    if (code) *code = rc;
    return true;
}

void child_free(Child *c)
{
    if (!c) return;
    SDL_DestroyProcess(c->proc);   /* does not stop it */
    SDL_free(c);
}
#endif
