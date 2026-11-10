/* See update.h. */
#include "update.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "http.h"
#include "programs.h"

const char *update_api_url(void)
{
    const char *u = SDL_getenv("RONDO_UPDATE_URL");
    return u && *u ? u : "https://api.github.com/repos/" UPDATE_REPO "/releases/latest";
}

const char *update_current_version(void)
{
    const char *v = SDL_getenv("RONDO_UPDATE_FROM");
    return v && *v ? v : RONDO_VERSION;
}

const char *update_user_agent(void)
{
#ifdef _WIN32
    return "RondoRecomp/" RONDO_VERSION " (Windows)";
#else
    return "RondoRecomp/" RONDO_VERSION " (Linux)";
#endif
}

int update_compare(const char *a, const char *b)
{
    while (*a == 'v' || *a == 'V') a++;
    while (*b == 'v' || *b == 'V') b++;
    for (;;) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        bool more_a = *a == '.', more_b = *b == '.';
        if (!more_a && !more_b) return 0;
        if (!more_a) return -1;           /* 1.1 is older than 1.1.1 */
        if (!more_b) return 1;
        a++;
        b++;
    }
}

/* ---- JSON (only what a release needs) ---- */

typedef struct { const char *p, *e; } Js;

static void js_ws(Js *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static bool js_lit(Js *j, char c)
{
    js_ws(j);
    if (j->p < j->e && *j->p == c) { j->p++; return true; }
    return false;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool js_hex4(Js *j, unsigned *v)
{
    if (j->e - j->p < 4) return false;
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(*j->p++);
        if (h < 0) return false;
        *v = *v << 4 | (unsigned)h;
    }
    return true;
}

/* A string, decoded to UTF-8 into out (cut to n - 1 bytes at a whole character; out NULL:
 * skipped). */
static bool js_string(Js *j, char *out, size_t n)
{
    js_ws(j);
    if (j->p >= j->e || *j->p != '"') return false;
    j->p++;
    size_t o = 0;
    bool cut = false;
    while (j->p < j->e && *j->p != '"') {
        unsigned char bytes[4];
        int nb = 1;
        bytes[0] = (unsigned char)*j->p++;
        if (bytes[0] == '\\') {
            if (j->p >= j->e) return false;
            char esc = *j->p++;
            switch (esc) {
            case 'n': bytes[0] = '\n'; break;
            case 't': bytes[0] = '\t'; break;
            case 'r': bytes[0] = '\r'; break;
            case 'b': bytes[0] = '\b'; break;
            case 'f': bytes[0] = '\f'; break;
            case 'u': {
                unsigned v, lo;
                if (!js_hex4(j, &v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    j->p += 2;
                    if (!js_hex4(j, &lo)) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (v < 0x80) { bytes[0] = (unsigned char)v; }
                else if (v < 0x800) { bytes[0] = (unsigned char)(0xC0 | v >> 6); bytes[1] = (unsigned char)(0x80 | (v & 63)); nb = 2; }
                else if (v < 0x10000) {
                    bytes[0] = (unsigned char)(0xE0 | v >> 12); bytes[1] = (unsigned char)(0x80 | (v >> 6 & 63));
                    bytes[2] = (unsigned char)(0x80 | (v & 63)); nb = 3;
                } else {
                    bytes[0] = (unsigned char)(0xF0 | v >> 18); bytes[1] = (unsigned char)(0x80 | (v >> 12 & 63));
                    bytes[2] = (unsigned char)(0x80 | (v >> 6 & 63)); bytes[3] = (unsigned char)(0x80 | (v & 63)); nb = 4;
                }
                break;
            }
            default: bytes[0] = (unsigned char)esc; break;   /* \" \\ \/ */
            }
        }
        if (!out || cut) continue;
        if (o + (size_t)nb + 1 > n) { cut = true; continue; }
        memcpy(out + o, bytes, (size_t)nb);
        o += (size_t)nb;
    }
    if (j->p >= j->e) return false;
    j->p++;
    if (out && n) {
        if (cut) {   /* no half character at the end (the last whole one may go too) */
            while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
            if (o > 0 && ((unsigned char)out[o - 1] & 0x80)) o--;
        }
        out[o] = 0;
    }
    return true;
}

static bool js_skip(Js *j, int depth)
{
    js_ws(j);
    if (j->p >= j->e || depth > 64) return false;
    char c = *j->p;
    if (c == '"') return js_string(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (js_lit(j, close)) return true;
        for (;;) {
            if (c == '{' && (!js_string(j, NULL, 0) || !js_lit(j, ':'))) return false;
            if (!js_skip(j, depth + 1)) return false;
            if (js_lit(j, ',')) continue;
            return js_lit(j, close);
        }
    }
    const char *s = j->p;   /* a number, true, false, null */
    while (j->p < j->e && (isalnum((unsigned char)*j->p) || *j->p == '-' || *j->p == '+' || *j->p == '.')) j->p++;
    return j->p > s;
}

static bool js_number(Js *j, long long *v)
{
    js_ws(j);
    const char *s = j->p;
    char tmp[32];
    size_t n = 0;
    while (j->p < j->e && (isalnum((unsigned char)*j->p) || *j->p == '-' || *j->p == '+' || *j->p == '.')) {
        if (n < sizeof tmp - 1) tmp[n++] = *j->p;
        j->p++;
    }
    tmp[n] = 0;
    *v = strtoll(tmp, NULL, 10);   /* null: 0 */
    return j->p > s;
}

static bool js_bool(Js *j, bool *v)
{
    js_ws(j);
    *v = j->e - j->p >= 4 && !strncmp(j->p, "true", 4);
    return js_skip(j, 0);
}

/* each key of an object: field reads (or skips) its value */
typedef bool (*JsField)(Js *j, const char *key, void *ctx);

static bool js_object(Js *j, JsField field, void *ctx)
{
    if (!js_lit(j, '{')) return false;
    if (js_lit(j, '}')) return true;
    for (;;) {
        char key[64];
        if (!js_string(j, key, sizeof key) || !js_lit(j, ':')) return false;
        if (!field(j, key, ctx)) return false;
        if (js_lit(j, ',')) continue;
        return js_lit(j, '}');
    }
}

typedef struct Asset {
    char name[160], url[512], digest[80];
    long long size;
} Asset;

typedef struct Release {
    char tag[64], page[256];
    char *body;
    size_t body_cap;
    bool draft, prerelease;
    Asset cur, full, slim, appimage;
    bool have_full, have_slim, have_appimage;
} Release;

static bool asset_field(Js *j, const char *key, void *ctx)
{
    Asset *a = ctx;
    if (!strcmp(key, "name")) return js_string(j, a->name, sizeof a->name);
    if (!strcmp(key, "browser_download_url")) return js_string(j, a->url, sizeof a->url);
    if (!strcmp(key, "digest")) {
        js_ws(j);
        if (j->p < j->e && *j->p == '"') return js_string(j, a->digest, sizeof a->digest);
        return js_skip(j, 0);   /* null */
    }
    if (!strcmp(key, "size")) return js_number(j, &a->size);
    return js_skip(j, 0);
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static bool release_field(Js *j, const char *key, void *ctx)
{
    Release *r = ctx;
    if (!strcmp(key, "tag_name")) return js_string(j, r->tag, sizeof r->tag);
    if (!strcmp(key, "html_url")) return js_string(j, r->page, sizeof r->page);
    if (!strcmp(key, "draft")) return js_bool(j, &r->draft);
    if (!strcmp(key, "prerelease")) return js_bool(j, &r->prerelease);
    if (!strcmp(key, "body")) {
        js_ws(j);
        if (j->p < j->e && *j->p == '"') return js_string(j, r->body, r->body_cap);
        return js_skip(j, 0);
    }
    if (!strcmp(key, "assets")) {
        if (!js_lit(j, '[')) return js_skip(j, 0);
        if (js_lit(j, ']')) return true;
        for (;;) {
            memset(&r->cur, 0, sizeof r->cur);
            if (!js_object(j, asset_field, &r->cur)) return false;
            if (!strncmp(r->cur.name, "RondoRecomp-", 12)) {
                if (ends_with(r->cur.name, "-" UPDATE_SYSTEM "-update-zig" RONDO_ZIG_VERSION UPDATE_EXT)) {
                    r->slim = r->cur;
                    r->have_slim = true;
                } else if (ends_with(r->cur.name, "-" UPDATE_SYSTEM UPDATE_EXT)) {
                    r->full = r->cur;
                    r->have_full = true;
                } else if (ends_with(r->cur.name, UPDATE_APPIMAGE)) {
                    r->appimage = r->cur;
                    r->have_appimage = true;
                }
            }
            if (js_lit(j, ',')) continue;
            return js_lit(j, ']');
        }
    }
    return js_skip(j, 0);
}

/* Markdown to plain text, in place: **bold**, `code`, [text](link) -> text */
static void plain(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) { p += 2; continue; }
        if (*p == '`') { p++; continue; }
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close && close[1] == '(' && strchr(close, ')')) {
                memmove(o, p + 1, (size_t)(close - p - 1));
                o += close - p - 1;
                p = strchr(close, ')') + 1;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
}

/* The release notes' "## New in ..." points (else the first points anywhere): the first
 * sentence of each, at most four. */
static void notes_from(const char *body, char notes[4][160])
{
    const char *section = NULL;
    for (const char *line = body; line && *line;) {
        const char *nl = strchr(line, '\n');
        if (!strncmp(line, "## ", 3) && !SDL_strncasecmp(line + 3, "new", 3)) { section = nl ? nl + 1 : ""; break; }
        line = nl ? nl + 1 : NULL;
    }
    int k = 0;
    for (const char *line = section ? section : body; line && *line && k < 4;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (section && !strncmp(line, "## ", 3)) break;   /* the next section */
        if (len > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            char text[512];
            size_t n = len - 2 < sizeof text - 1 ? len - 2 : sizeof text - 1;
            memcpy(text, line + 2, n);
            text[n] = 0;
            char *cr = strchr(text, '\r');
            if (cr) *cr = 0;
            plain(text);
            char *stop = strstr(text, ". ");
            if (stop) stop[1] = 0;
            if (strlen(text) > 100) {          /* still long: cut at a word */
                text[97] = 0;
                char *sp = strrchr(text, ' ');
                if (sp) *sp = 0;
                strcat(text, "...");
            }
            snprintf(notes[k++], sizeof notes[0], "%s", text);
        }
        line = nl ? nl + 1 : NULL;
    }
}

bool update_parse(const char *json, size_t len, UpdateInfo *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof *info);
    Release r;
    memset(&r, 0, sizeof r);
    r.body_cap = len + 1;
    r.body = malloc(r.body_cap);
    if (!r.body) { snprintf(err, errlen, "out of memory"); return false; }
    r.body[0] = 0;
    Js j = {json, json + len};
    bool ok = js_object(&j, release_field, &r);
    if (!ok || !r.tag[0]) {
        snprintf(err, errlen, "not a release (%.60s)", len ? json : "nothing came");
        free(r.body);
        return false;
    }
    const char *v = r.tag;
    while (*v == 'v' || *v == 'V') v++;
    snprintf(info->version, sizeof info->version, "%s", v);
    snprintf(info->page, sizeof info->page, "%s", r.page);
    notes_from(r.body, info->notes);
    free(r.body);
    if (r.draft || r.prerelease) return true;          /* not one to install */
    /* from the AppImage: the new AppImage, the whole of it (it has the compiler inside) */
    const Asset *a = rondo_appimage() ? (r.have_appimage ? &r.appimage : NULL)
                   : r.have_slim ? &r.slim : r.have_full ? &r.full : NULL;
    /* https only, unless the release itself came over plain http (a local test server) */
    if (a && (!strncmp(a->url, "https://", 8) || !strncmp(update_api_url(), "http://", 7))) {
        snprintf(info->asset, sizeof info->asset, "%s", a->name);
        snprintf(info->url, sizeof info->url, "%s", a->url);
        info->size = a->size;
        info->slim = a == &r.slim;
        if (!strncmp(a->digest, "sha256:", 7) && strlen(a->digest) == 7 + 64) {
            for (int i = 0; i < 64; i++) info->sha256[i] = (char)tolower((unsigned char)a->digest[7 + i]);
            info->sha256[64] = 0;
        }
    }
    return true;
}

bool update_progress_line(const char *seg, char *what, size_t n, int *pct)
{
    size_t len = strlen(seg);
    while (len && seg[len - 1] == ' ') len--;
    if (len < 2 || seg[len - 1] != '%') return false;
    size_t d = len - 1;
    while (d > 0 && isdigit((unsigned char)seg[d - 1])) d--;
    if (d == len - 1 || d == 0 || seg[d - 1] != ' ') return false;
    *pct = atoi(seg + d);
    const char *s = seg;
    while (*s == ' ') s++;
    size_t e = d;
    while (e > (size_t)(s - seg) && seg[e - 1] == ' ') e--;
    size_t m = e > (size_t)(s - seg) ? e - (size_t)(s - seg) : 0;
    if (m >= n) m = n - 1;
    memcpy(what, s, m);
    what[m] = 0;
    return true;
}

/* ---- the check ---- */

static UpdateState check_state;
static UpdateInfo found;
static char check_why[160];

static void on_release(int status, const char *body, size_t len, void *userdata)
{
    (void)userdata;
    UpdateInfo info;
    char err[160];
    if (status != 200) {
        check_state = UPD_FAILED;
        if (status) snprintf(check_why, sizeof check_why, "GitHub answered %d", status);
        else snprintf(check_why, sizeof check_why, "%.150s", body);
    } else if (!update_parse(body, len, &info, err, sizeof err)) {
        check_state = UPD_FAILED;
        snprintf(check_why, sizeof check_why, "%s", err);
    } else {
        found = info;
        check_state = info.url[0] && update_compare(info.version, update_current_version()) > 0 ? UPD_AVAILABLE : UPD_LATEST;
    }
}

void update_check_start(void)
{
    if (check_state == UPD_CHECKING) return;
    if (http_post(update_api_url(), NULL, update_user_agent(), on_release, NULL)) {
        check_state = UPD_CHECKING;
    } else {
        check_state = UPD_FAILED;
        snprintf(check_why, sizeof check_why, "%s", http_error());
    }
}

UpdateState update_state(const UpdateInfo **info, const char **why)
{
    if (info) *info = check_state == UPD_AVAILABLE || check_state == UPD_LATEST ? &found : NULL;
    if (why) *why = check_why;
    return check_state;
}

/* ---- installing ---- */

static Child *run;
static UpdateRun run_state;
static char run_line[160], run_last[160], partial[512];
static int run_pct = -1;

static void run_segment(const char *seg)
{
    char what[120];
    int pct;
    if (update_progress_line(seg, what, sizeof what, &pct)) {
        run_pct = pct;
        snprintf(run_line, sizeof run_line, "%s", what);
        return;
    }
    while (*seg == ' ') seg++;
    if (!*seg) return;
    run_pct = -1;
    snprintf(run_line, sizeof run_line, "%s", seg);
    snprintf(run_last, sizeof run_last, "%s", seg);
}

static void run_feed(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        size_t l = strlen(partial);
        if (buf[i] == '\r' || buf[i] == '\n') {
            run_segment(partial);
            partial[0] = 0;
        } else if (l + 1 < sizeof partial) {
            partial[l] = buf[i];
            partial[l + 1] = 0;
        }
    }
}

bool update_run_start(const char *setup, char *err, size_t errlen)
{
    if (run) { snprintf(err, errlen, "an update is already running"); return false; }
    const char *args[] = {setup, "--update", NULL};
    run = child_start(args, err, errlen);
    if (!run) return false;
    run_state = RUN_RUNNING;
    run_pct = -1;
    partial[0] = run_last[0] = 0;
    snprintf(run_line, sizeof run_line, "Starting Setup...");
    return true;
}

UpdateRun update_run_poll(char *line, size_t n, int *pct)
{
    if (run) {
        char buf[2048];
        int got;
        while ((got = child_read(run, buf, sizeof buf)) > 0) run_feed(buf, got);
        int code;
        if (child_wait(run, false, &code)) {
            while ((got = child_read(run, buf, sizeof buf)) > 0) run_feed(buf, got);
            run_segment(partial);
            partial[0] = 0;
            child_free(run);
            run = NULL;
            run_pct = -1;
            if (code == 0) {
                run_state = RUN_INSTALLED;
            } else if (code == 3) {
                run_state = RUN_LATEST;
                snprintf(run_line, sizeof run_line, "This is the latest version.");
            } else {
                run_state = RUN_FAILED;
                snprintf(run_line, sizeof run_line, "Update failed: %.140s", run_last[0] ? run_last : "Setup stopped");
            }
        }
    }
    if (line) snprintf(line, n, "%s", run_line);
    if (pct) *pct = run_pct;
    return run_state;
}
