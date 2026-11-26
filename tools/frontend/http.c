/* See http.h. */
#include "http.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Request {
    char *url, *post, *agent;
    HttpDone done;
    void *userdata;
    int status;
    char *body;
    size_t len, cap;
    SDL_IOStream *file;            /* http_get_file: the body goes here instead */
    long long got, total;          /* ...bytes so far, of total (0 unknown) */
    HttpProgress progress;
    bool cancelled, write_failed;
    struct Request *next;
} Request;

static SDL_Mutex *lock;
static Request *finished;          /* answered, waiting for http_poll */
static SDL_AtomicInt running;
static char error[160];

const char *http_error(void) { return error; }
int http_pending(void) { return SDL_GetAtomicInt(&running); }

static bool append(Request *r, const void *data, size_t n)
{
    if (r->len + n + 1 > r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4096;
        while (cap < r->len + n + 1) cap *= 2;
        char *b = realloc(r->body, cap);
        if (!b) return false;
        r->body = b;
        r->cap = cap;
    }
    memcpy(r->body + r->len, data, n);
    r->len += n;
    r->body[r->len] = 0;
    return true;
}

static void fail(Request *r, const char *why)
{
    r->status = 0;
    r->len = 0;
    append(r, why, strlen(why));
}

/* the body as it arrives: into memory, or (http_get_file) into the file, with progress */
static bool take(Request *r, const void *data, size_t n)
{
    if (!r->file) return append(r, data, n);
    if (SDL_WriteIO(r->file, data, n) != n) { r->write_failed = true; return false; }
    r->got += (long long)n;
    if (r->progress && !r->progress(r->got, r->total, r->userdata)) { r->cancelled = true; return false; }
    return true;
}

#ifdef _WIN32
/* ---- WinHTTP ------------------------------------------------------------------------- */
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

static bool backend_ready(void) { return true; }

static void perform(Request *r)
{
    wchar_t url[2048], agent[256], host[256], path[2048], extra[2048];
    MultiByteToWideChar(CP_UTF8, 0, r->url, -1, url, 2048);
    MultiByteToWideChar(CP_UTF8, 0, r->agent, -1, agent, 256);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { fail(r, "bad address"); return; }
    wcsncat(path, extra, 2047 - wcslen(path));
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET q = c ? WinHttpOpenRequest(c, r->post ? L"POST" : L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                      : NULL;
    if (s) WinHttpSetTimeouts(s, 15000, 15000, 30000, 30000);
    DWORD len = r->post ? (DWORD)strlen(r->post) : 0;
    const wchar_t *headers = r->post ? L"Content-Type: application/x-www-form-urlencoded\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    bool ok = q && WinHttpSendRequest(q, headers, (DWORD)-1L, r->post ? r->post : WINHTTP_NO_REQUEST_DATA, len, len, 0) &&
              WinHttpReceiveResponse(q, NULL);
    if (ok) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &size, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)status;
        for (;;) {
            DWORD avail = 0, got = 0;
            char buf[8192];
            if (!WinHttpQueryDataAvailable(q, &avail) || avail == 0) break;
            if (avail > sizeof buf) avail = sizeof buf;
            if (!WinHttpReadData(q, buf, avail, &got) || got == 0) break;
            if (!take(r, buf, got)) break;
        }
    } else {
        char why[64];
        snprintf(why, sizeof why, "no connection (error %lu)", (unsigned long)GetLastError());
        fail(r, why);
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
}

#else
/* ---- libcurl, loaded at run time (its ABI: options and info codes are fixed numbers) ---- */
typedef void CURL;
struct curl_slist;
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_USERAGENT 10018
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_TIMEOUT 13
#define CURLOPT_LOW_SPEED_LIMIT 19
#define CURLOPT_LOW_SPEED_TIME 20
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_NOSIGNAL 99
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURL_GLOBAL_DEFAULT 3

static struct {
    int (*global_init)(long);
    CURL *(*easy_init)(void);
    int (*easy_setopt)(CURL *, int, ...);
    int (*easy_perform)(CURL *);
    int (*easy_getinfo)(CURL *, int, ...);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} curl;

static bool backend_ready(void)
{
    static int state;      /* 0 not tried, 1 loaded, -1 missing */
    if (state) return state > 0;
    /* (macOS has its own in /usr/lib) */
    static const char *const names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "libcurl.4.dylib"};
    SDL_SharedObject *so = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !so; i++) so = SDL_LoadObject(names[i]);
    if (so) {
#define LOAD(field, name) *(void **)&curl.field = (void *)SDL_LoadFunction(so, name)
        LOAD(global_init, "curl_global_init");
        LOAD(easy_init, "curl_easy_init");
        LOAD(easy_setopt, "curl_easy_setopt");
        LOAD(easy_perform, "curl_easy_perform");
        LOAD(easy_getinfo, "curl_easy_getinfo");
        LOAD(easy_cleanup, "curl_easy_cleanup");
        LOAD(easy_strerror, "curl_easy_strerror");
        LOAD(slist_append, "curl_slist_append");
        LOAD(slist_free_all, "curl_slist_free_all");
#undef LOAD
    }
    if (!so || !curl.global_init || !curl.easy_init || !curl.easy_setopt || !curl.easy_perform || !curl.easy_getinfo ||
        !curl.easy_cleanup || !curl.easy_strerror || !curl.slist_append || !curl.slist_free_all) {
        snprintf(error, sizeof error, "libcurl not found (install your system's curl package)");
        state = -1;
        return false;
    }
    curl.global_init(CURL_GLOBAL_DEFAULT);   /* once, on the main thread, before any request */
    state = 1;
    return true;
}

static size_t on_data(char *data, size_t size, size_t n, void *userdata)
{
    return take(userdata, data, size * n) ? size * n : 0;
}

static void perform(Request *r)
{
    CURL *h = curl.easy_init();
    if (!h) { fail(r, "libcurl could not start"); return; }
    struct curl_slist *headers = NULL;
    curl.easy_setopt(h, CURLOPT_URL, r->url);
    curl.easy_setopt(h, CURLOPT_USERAGENT, r->agent);
    curl.easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl.easy_setopt(h, CURLOPT_WRITEDATA, (void *)r);
    curl.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    if (r->file) {   /* a download takes as long as it takes, unless it stalls for 30 s */
        curl.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    } else {
        curl.easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    }
    if (r->post) {
        headers = curl.slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
        curl.easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        curl.easy_setopt(h, CURLOPT_POSTFIELDS, r->post);
    }
    int code = curl.easy_perform(h);
    if (code == 0) {
        long status = 0;
        curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        r->status = (int)status;
    } else {
        fail(r, curl.easy_strerror(code));
    }
    if (headers) curl.slist_free_all(headers);
    curl.easy_cleanup(h);
}
#endif

/* ---- requests ------------------------------------------------------------------------ */

static int SDLCALL worker(void *arg)
{
    Request *r = arg;
    perform(r);
    SDL_LockMutex(lock);
    r->next = finished;
    finished = r;
    SDL_UnlockMutex(lock);
    return 0;
}

static char *copy_str(const char *s) { return s ? SDL_strdup(s) : NULL; }

static void release(Request *r)
{
    SDL_free(r->url);
    SDL_free(r->post);
    SDL_free(r->agent);
    free(r->body);
    free(r);
}

bool http_post(const char *url, const char *post_data, const char *user_agent, HttpDone done, void *userdata)
{
    if (!lock) lock = SDL_CreateMutex();
    if (!lock || !backend_ready()) return false;
    Request *r = calloc(1, sizeof *r);
    if (!r) return false;
    r->url = copy_str(url);
    r->post = copy_str(post_data);
    r->agent = copy_str(user_agent ? user_agent : "RondoRecomp");
    r->done = done;
    r->userdata = userdata;
    SDL_AddAtomicInt(&running, 1);
    SDL_Thread *t = SDL_CreateThread(worker, "http", r);
    if (!t) {
        SDL_AddAtomicInt(&running, -1);
        snprintf(error, sizeof error, "cannot start a thread: %s", SDL_GetError());
        release(r);
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

void http_poll(void)
{
    if (!lock) return;
    SDL_LockMutex(lock);
    Request *list = finished;
    finished = NULL;
    SDL_UnlockMutex(lock);
    /* oldest first */
    Request *rev = NULL;
    while (list) { Request *n = list->next; list->next = rev; rev = list; list = n; }
    while (rev) {
        Request *n = rev->next;
        SDL_AddAtomicInt(&running, -1);
        if (rev->done) rev->done(rev->status, rev->body ? rev->body : "", rev->len, rev->userdata);
        release(rev);
        rev = n;
    }
}

int http_get_file(const char *url, const char *user_agent, const char *path, long long expected, HttpProgress progress,
                  void *userdata, char *err, size_t errlen)
{
    err[0] = 0;
    if (!backend_ready()) { snprintf(err, errlen, "%s", error); return 0; }
    Request r;
    memset(&r, 0, sizeof r);
    r.url = (char *)url;
    r.agent = (char *)(user_agent ? user_agent : "RondoRecomp");
    r.file = SDL_IOFromFile(path, "wb");
    if (!r.file) { snprintf(err, errlen, "cannot write %s: %s", path, SDL_GetError()); return 0; }
    r.total = expected;
    r.progress = progress;
    r.userdata = userdata;
    perform(&r);
    if (!SDL_CloseIO(r.file) && !r.write_failed) r.write_failed = true;
    if (r.write_failed) snprintf(err, errlen, "cannot write %s (is the disk full?)", path);
    else if (r.cancelled) snprintf(err, errlen, "cancelled");
    else if (r.status == 0) snprintf(err, errlen, "%s", r.body ? r.body : "no answer");
    else if (r.status != 200) snprintf(err, errlen, "the server answered %d", r.status);
    free(r.body);
    return r.write_failed || r.cancelled ? 0 : r.status;
}

int http_get_text(const char *url, const char *user_agent, char **body, size_t *len, char *err, size_t errlen)
{
    err[0] = 0;
    *body = NULL;
    *len = 0;
    if (!backend_ready()) { snprintf(err, errlen, "%s", error); return 0; }
    Request r;
    memset(&r, 0, sizeof r);
    r.url = (char *)url;
    r.agent = (char *)(user_agent ? user_agent : "RondoRecomp");
    perform(&r);
    if (r.status == 0) {
        snprintf(err, errlen, "%s", r.body ? r.body : "no answer");
        free(r.body);
        return 0;
    }
    if (r.status != 200) snprintf(err, errlen, "the server answered %d", r.status);
    if (!r.body) append(&r, "", 0);
    *body = r.body;
    *len = r.len;
    return r.status;
}
