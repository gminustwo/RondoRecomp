/* See achievements.h. */
#include "achievements.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_api_runtime.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"
#include "rc_runtime.h"

#include "http.h"
#include "pce/disc.h"
#include "programs.h"
#include "theme.h"

static rc_client_t *client;
static Pce *pce;
static Settings *set;
static bool settings_changed, reset_wanted;
static bool logging_in, loading;
static char game_cue[1024];       /* the disc running */
static char game_hash[33];        /* its RetroAchievements hash, "" if it has none */
static char loaded_hash[33];      /* the hash whose achievements are loaded (or loading) */
static char note[160];            /* the last login / loading problem, for the menu */
static char agent[160];           /* User-Agent: RondoRecomp/<version> (<system>) rcheevos/<version> */

/* The server's notices come as an achievement of its own at the top of the set (this id, 0
 * points, unlocking about 5 s in), which the library never sends back: a client the server
 * does not recognise (RondoRecomp, until RetroAchievements registers it), or one it allows
 * only casual play, gets "Warning: Unknown Emulator" / "... Hardcore unlocks cannot be
 * earned ...", and its hardcore unlocks are recorded as softcore (leaderboard entries not
 * at all). The library's own RC_CLIENT_ACHIEVEMENT_WARNING_ID, which it keeps private. */
#define SERVER_NOTICE_ID 101000001u
static bool server_softcore;      /* the loaded game's notice says hardcore can't be earned */
static bool hardcore_was;         /* the hardcore setting last applied (ach_configure) */

/* ---- pop-ups and trackers ---------------------------------------------------------- */

typedef struct Popup {
    char head[64], title[128], text[192];
    Uint32 ms;
} Popup;

#define MAX_POPUPS 8
static Popup popups[MAX_POPUPS];
static int n_popups;
static Uint64 popup_since;        /* when the first one in the queue appeared */

static void popup(Uint32 ms, const char *head, const char *title, const char *fmt, ...)
{
    if (n_popups == MAX_POPUPS) {           /* the oldest waiting one gives way */
        memmove(&popups[1], &popups[2], sizeof popups[0] * (MAX_POPUPS - 2));
        n_popups--;
    }
    Popup *p = &popups[n_popups++];
    snprintf(p->head, sizeof p->head, "%s", head);
    snprintf(p->title, sizeof p->title, "%s", title ? title : "");
    p->text[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->text, sizeof p->text, fmt, ap);
        va_end(ap);
    }
    p->ms = ms;
    if (n_popups == 1) popup_since = SDL_GetTicks();
}

typedef struct Tracker { uint32_t id; char text[96]; } Tracker;
static Tracker trackers[4], challenges[4];
static int n_trackers, n_challenges;
static char progress[128];
static Uint64 progress_until;

static void track(Tracker *list, int *n, uint32_t id, const char *text, bool show)
{
    int i = 0;
    while (i < *n && list[i].id != id) i++;
    if (!show) {
        if (i < *n) { memmove(&list[i], &list[i + 1], sizeof list[0] * (size_t)(*n - i - 1)); (*n)--; }
        return;
    }
    if (i == *n) {
        if (*n == 4) return;
        (*n)++;
    }
    list[i].id = id;
    snprintf(list[i].text, sizeof list[i].text, "%s", text);
}

/* ---- the library's hooks -------------------------------------------------------------- */

/* RetroAchievements' PC Engine CD memory: work RAM ($000000), CD RAM ($002000), Super System
 * Card RAM ($012000), backup RAM ($042000) */
static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *c)
{
    (void)c;
    uint32_t i = 0;
    for (; i < num_bytes; i++, address++) {
        if (address < 0x2000) buffer[i] = pce->wram[address];
        else if (address < 0x12000) buffer[i] = pce->cdram[address - 0x2000];
        else if (address < 0x42000) buffer[i] = pce->superram[address - 0x12000];
        else if (address < 0x42800) buffer[i] = pce->bram[address - 0x42000];
        else break;
    }
    return i;
}

typedef struct Call {
    rc_client_server_callback_t callback;
    void *data;
} Call;

static void on_http(int status, const char *body, size_t len, void *userdata)
{
    Call *c = userdata;
    rc_api_server_response_t r;
    r.body = body;
    r.body_length = len;
    r.http_status_code = status ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    c->callback(&r, c->data);
    free(c);
}

static void RC_CCONV server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                                 void *callback_data, rc_client_t *c)
{
    (void)c;
    Call *call = malloc(sizeof *call);
    if (call) {
        call->callback = callback;
        call->data = callback_data;
        if (http_post(request->url, request->post_data, agent, on_http, call)) return;
        free(call);
    }
    rc_api_server_response_t r;
    r.body = http_error();
    r.body_length = strlen(r.body);
    r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    callback(&r, callback_data);
}

static void RC_CCONV log_message(const char *message, const rc_client_t *c)
{
    (void)c;
    SDL_Log("RetroAchievements: %s", message);
}

static bool game_loaded(void)
{
    const rc_client_game_t *g = client ? rc_client_get_game_info(client) : NULL;
    return g && g->id != 0 && !loading;
}

static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *c)
{
    const rc_client_game_t *g = rc_client_get_game_info(c);
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (e->achievement->id >= SERVER_NOTICE_ID)   /* the server's notice, as it words it */
            popup(8000, "RetroAchievements", e->achievement->title, "%s", e->achievement->description);
        else
            popup(5000, "Achievement unlocked", e->achievement->title, "%s  (%u points)", e->achievement->description,
                  e->achievement->points);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        popup(6000, rc_client_get_hardcore_enabled(c) ? "Game mastered" : "Game completed", g ? g->title : "",
              "Every achievement earned");
        break;
    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
        popup(5000, "Set completed", e->subset ? e->subset->title : "", "Every achievement in it earned");
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        popup(3000, "Leaderboard attempt started", e->leaderboard->title, "%s", e->leaderboard->description);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
        popup(3000, "Leaderboard attempt failed", e->leaderboard->title, NULL);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        popup(4000, "Leaderboard", e->leaderboard->title, "Submitted: %s", e->leaderboard->tracker_value);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
        popup(4000, "Leaderboard", e->leaderboard ? e->leaderboard->title : "", "Rank %u of %u  (best %s)",
              e->leaderboard_scoreboard->new_rank, e->leaderboard_scoreboard->num_entries,
              e->leaderboard_scoreboard->best_score);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
        track(trackers, &n_trackers, e->leaderboard_tracker->id, e->leaderboard_tracker->display,
              e->type != RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE);
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
        track(challenges, &n_challenges, e->achievement->id, e->achievement->title,
              e->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW);
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
        snprintf(progress, sizeof progress, "%s  %s", e->achievement->title, e->achievement->measured_progress);
        progress_until = SDL_GetTicks() + 2500;
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
        progress_until = 0;
        break;
    case RC_CLIENT_EVENT_RESET:
        reset_wanted = true;
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        popup(5000, "RetroAchievements", "The server reported a problem", "%s", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        popup(5000, "RetroAchievements", "Offline", "Unlocks are sent when the connection is back");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        popup(3000, "RetroAchievements", "Back online", "Waiting unlocks were sent");
        break;
    default:
        break;
    }
}

/* ---- the disc's hash (RetroAchievements' PC Engine CD method) through the runtime's disc
 * reader, which takes every image the game does (BIN, IMG, ISO + WAV) -------------------- */

typedef struct Track {
    Disc disc;
    const DiscTrack *t;
} Track;

static void *RC_CCONV cd_open(const char *path, uint32_t track)
{
    Track *h = malloc(sizeof *h);
    char err[256];
    if (!h || !disc_open(&h->disc, path, err, sizeof err)) { free(h); return NULL; }
    h->t = NULL;
    for (int i = 0; i < h->disc.n_tracks; i++) {
        const DiscTrack *t = &h->disc.tracks[i];
        if (track == RC_HASH_CDTRACK_FIRST_DATA ? t->data && !h->t
            : track == RC_HASH_CDTRACK_LAST     ? true
            : track == RC_HASH_CDTRACK_LARGEST  ? !h->t || t->sectors > h->t->sectors
                                                : (uint32_t)t->number == track)
            h->t = t;
    }
    if (!h->t) { free(h); return NULL; }
    return h;
}

static size_t RC_CCONV cd_read(void *handle, uint32_t sector, void *buffer, size_t requested)
{
    Track *h = handle;
    uint8_t data[2048];
    if (!h->t->data || !disc_read_data(&h->disc, (int)sector, 1, data)) return 0;
    if (requested > sizeof data) requested = sizeof data;
    memcpy(buffer, data, requested);
    return requested;
}

static void RC_CCONV cd_close(void *handle) { free(handle); }
static uint32_t RC_CCONV cd_first_sector(void *handle) { return (uint32_t)((Track *)handle)->t->lba; }

static bool hash_disc(const char *cue, char hash[33])
{
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, cue, NULL, 0);
    it.callbacks.cdreader.open_track = cd_open;
    it.callbacks.cdreader.read_sector = cd_read;
    it.callbacks.cdreader.close_track = cd_close;
    it.callbacks.cdreader.first_track_sector = cd_first_sector;
    it.callbacks.cdreader.open_track_iterator = NULL;
    bool ok = rc_hash_generate(hash, RC_CONSOLE_PC_ENGINE_CD, &it) != 0;
    rc_hash_destroy_iterator(&it);
    if (!ok) hash[0] = 0;
    return ok;
}

/* ---- logging in and loading the game's achievements -------------------------------------- */

static void maybe_load(void);

/* The library runs hardcore as the setting says, unless the server records this client's
 * hardcore unlocks as softcore: then softcore, so it counts (and re-earns) what the server
 * keeps. Hardcore's rules stay with the setting either way (ach_hardcore). */
static void apply_hardcore(void)
{
    bool want = set->ra_hardcore && !server_softcore;
    if (!!rc_client_get_hardcore_enabled(client) != want) rc_client_set_hardcore_enabled(client, want);
}

static bool hardcore_refused(rc_client_t *c)
{
    const rc_client_achievement_t *n = rc_client_get_achievement_info(c, SERVER_NOTICE_ID);
    return n && n->description && strstr(n->description, "Hardcore unlocks cannot be earned");
}

static void RC_CCONV on_login(int result, const char *error, rc_client_t *c, void *userdata)
{
    bool with_token = userdata != NULL;
    logging_in = false;
    if (result == RC_OK) {
        const rc_client_user_t *u = rc_client_get_user_info(c);
        if (strcmp(set->ra_user, u->username) || strcmp(set->ra_token, u->token)) {
            snprintf(set->ra_user, sizeof set->ra_user, "%s", u->username);
            snprintf(set->ra_token, sizeof set->ra_token, "%s", u->token);
            settings_changed = true;
        }
        note[0] = 0;
        if (!with_token) popup(3000, "RetroAchievements", u->display_name, "Logged in  (%u points)", u->score);
        maybe_load();
        return;
    }
    snprintf(note, sizeof note, "Login failed: %s", error ? error : rc_error_str(result));
    if (with_token && (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)) {
        set->ra_token[0] = 0;          /* no longer good: log in again with the password */
        settings_changed = true;
    }
    popup(5000, "RetroAchievements", "Not logged in", "%s", error ? error : rc_error_str(result));
}

static void RC_CCONV on_load(int result, const char *error, rc_client_t *c, void *userdata)
{
    (void)userdata;
    loading = false;
    const rc_client_game_t *g = rc_client_get_game_info(c);
    if (result == RC_OK && g && g->id) {
        server_softcore = hardcore_refused(c);
        apply_hardcore();
        rc_client_user_game_summary_t sum;
        rc_client_get_user_game_summary(c, &sum);
        note[0] = 0;
        const char *head = rc_client_get_hardcore_enabled(c)      ? "RetroAchievements  -  hardcore"
                           : server_softcore && set->ra_hardcore ? "RetroAchievements  -  softcore only"
                                                                 : "RetroAchievements";
        popup(5000, head, g->title, "%u of %u achievements unlocked  (%u of %u points)", sum.num_unlocked_achievements,
              sum.num_core_achievements, sum.points_unlocked, sum.points_core);
    } else {
        /* keep loaded_hash: an unknown disc is not asked about again until it changes */
        snprintf(note, sizeof note, "No achievements loaded: %s", error ? error : rc_error_str(result));
    }
}

static void maybe_load(void)
{
    if (!client || !set->ra_enabled || !game_hash[0] || loading || logging_in || !rc_client_get_user_info(client))
        return;
    if (!strcmp(loaded_hash, game_hash)) return;
    if (loaded_hash[0]) rc_client_unload_game(client);
    server_softcore = false;               /* asked again with each load */
    apply_hardcore();
    snprintf(loaded_hash, sizeof loaded_hash, "%s", game_hash);
    n_trackers = n_challenges = 0;
    loading = true;
    rc_client_begin_load_game(client, game_hash, on_load, NULL);
}

static void login_with_token(void)
{
    if (!client || logging_in || !set->ra_enabled || !set->ra_user[0] || !set->ra_token[0] || rc_client_get_user_info(client))
        return;
    logging_in = true;
    rc_client_begin_login_with_token(client, set->ra_user, set->ra_token, on_login, (void *)1);
}

/* ---- the frontend's side ---------------------------------------------------------------- */

void ach_init(Pce *p, Settings *s)
{
    pce = p;
    set = s;
    client = rc_client_create(read_memory, server_call);
    if (!client) return;
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(client, on_event);
    char clause[64] = "";
    rc_client_get_user_agent_clause(client, clause, sizeof clause);
#ifdef _WIN32
    snprintf(agent, sizeof agent, "RondoRecomp/" RONDO_VERSION " (Windows) %s", clause);
#else
    snprintf(agent, sizeof agent, "RondoRecomp/" RONDO_VERSION " (Linux) %s", clause);
#endif
    hardcore_was = set->ra_hardcore;
    apply_hardcore();
    login_with_token();
}

void ach_shutdown(void)
{
    if (client) rc_client_destroy(client);
    client = NULL;
}

void ach_configure(void)
{
    if (!client) return;
    if (!set->ra_enabled) {
        if (loaded_hash[0]) rc_client_unload_game(client);
        loaded_hash[0] = 0;
        loading = server_softcore = false;
        n_trackers = n_challenges = 0;
        progress_until = 0;
    }
    /* switching hardcore on with a game loaded restarts it: the library asks (RC_CLIENT_EVENT_RESET),
     * or, while the server refuses hardcore (the library stays softcore), this does, so hardcore's
     * rules always start from power-on */
    if (set->ra_hardcore && !hardcore_was && server_softcore && game_loaded()) reset_wanted = true;
    hardcore_was = set->ra_hardcore;
    apply_hardcore();
    login_with_token();
    maybe_load();
}

void ach_login(const char *user, const char *password)
{
    if (!client) return;
    if (rc_client_get_user_info(client)) ach_logout();
    note[0] = 0;
    logging_in = true;
    rc_client_begin_login_with_password(client, user, password, on_login, NULL);
}

void ach_logout(void)
{
    if (!client) return;
    if (loaded_hash[0]) rc_client_unload_game(client);
    rc_client_logout(client);
    loaded_hash[0] = 0;
    loading = logging_in = server_softcore = false;
    n_trackers = n_challenges = 0;
    set->ra_token[0] = 0;
    settings_changed = true;
    snprintf(note, sizeof note, "Logged out.");
}

bool ach_logged_in(void) { return client && rc_client_get_user_info(client) != NULL; }

void ach_game_started(const char *cue)
{
    if (!client) return;
    if (strcmp(cue, game_cue)) {
        snprintf(game_cue, sizeof game_cue, "%s", cue);
        if (!hash_disc(cue, game_hash)) snprintf(note, sizeof note, "This disc has no RetroAchievements hash.");
    }
    if (game_loaded() && !strcmp(loaded_hash, game_hash)) rc_client_reset(client);
    n_trackers = n_challenges = 0;
    progress_until = 0;
    maybe_load();
}

void ach_frame(void)
{
    if (client && set->ra_enabled && game_loaded()) rc_client_do_frame(client);
}

bool ach_update(void)
{
    http_poll();
    if (client) {
        rc_client_idle(client);
        maybe_load();
    }
    bool changed = settings_changed;
    settings_changed = false;
    return changed;
}

bool ach_hardcore(void)
{
    /* the setting's: kept while the server counts the unlocks as softcore (apply_hardcore) */
    return client && set->ra_enabled && set->ra_hardcore && game_loaded();
}

bool ach_take_reset(void)
{
    bool r = reset_wanted;
    reset_wanted = false;
    return r;
}

static void progress_path(char *out, size_t n, const char *state_path) { snprintf(out, n, "%s.ra", state_path); }

void ach_save_progress(const char *state_path)
{
    char path[1300];
    progress_path(path, sizeof path, state_path);
    if (!game_loaded()) { SDL_RemovePath(path); return; }
    size_t size = rc_client_progress_size(client);
    uint8_t *buf = size ? malloc(size) : NULL;
    if (buf && rc_client_serialize_progress_sized(client, buf, size) == RC_OK) SDL_SaveFile(path, buf, size);
    free(buf);
}

void ach_load_progress(const char *state_path)
{
    if (!game_loaded()) return;
    char path[1300];
    progress_path(path, sizeof path, state_path);
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    /* no file: the achievements start over from here (none half-way) */
    rc_client_deserialize_progress_sized(client, data, data ? size : 0);
    SDL_free(data);
    n_trackers = n_challenges = 0;
}

void ach_status(char *line0, size_t n0, char *line1, size_t n1)
{
    const rc_client_user_t *u = client ? rc_client_get_user_info(client) : NULL;
    line1[0] = 0;
    if (!client) { snprintf(line0, n0, "RetroAchievements could not start."); return; }
    if (logging_in) snprintf(line0, n0, "Logging in...");
    else if (u) snprintf(line0, n0, "Logged in as %s  (%u points, %u softcore)", u->display_name, u->score, u->score_softcore);
    else snprintf(line0, n0, "%s", note[0] ? note : "Not logged in: enter your user name and password, then Log in.");
    if (!set->ra_enabled) { snprintf(line1, n1, "RetroAchievements is off."); return; }
    if (!u) return;
    if (loading) { snprintf(line1, n1, "Loading the game's achievements..."); return; }
    const rc_client_game_t *g = rc_client_get_game_info(client);
    if (g && g->id) {
        rc_client_user_game_summary_t sum;
        rc_client_get_user_game_summary(client, &sum);
        snprintf(line1, n1, "%u of %u achievements, %u of %u points%s", sum.num_unlocked_achievements,
                 sum.num_core_achievements, sum.points_unlocked, sum.points_core,
                 rc_client_get_hardcore_enabled(client) ? " (hardcore)"
                 : server_softcore && set->ra_hardcore ? " (softcore only: the server allows no hardcore unlocks here)" : "");
    } else if (note[0]) {
        snprintf(line1, n1, "%s", note);
    }
}

void ach_draw(SDL_Renderer *ren)
{
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float u = th_unit(ww, wh), m = 14 * u;
    Uint64 now = SDL_GetTicks();
    /* trackers, challenges and progress: small plaques down the right-hand side */
    float y = m;
    char lines[10][128];
    int nl = 0;
    for (int i = 0; i < n_trackers && nl < 10; i++) snprintf(lines[nl++], sizeof lines[0], "%s", trackers[i].text);
    for (int i = 0; i < n_challenges && nl < 10; i++) snprintf(lines[nl++], sizeof lines[0], "Challenge: %s", challenges[i].text);
    if (now < progress_until && nl < 10) snprintf(lines[nl++], sizeof lines[0], "%s", progress);
    for (int i = 0; i < nl; i++) {
        float px = 12 * u, h = 24 * u;
        float w = th_measure(ren, TH_FONT_UI, px, lines[i], 0) + h + 20 * u;
        th_plaque(ren, (float)ww - w - m, y, (float)ww - m, y + h, u, 0.92f);
        th_text(ren, TH_FONT_UI, px, (float)ww - m - w * 0.5f, y + h * 0.5f, TH_TEXT, lines[i], 0.5f, 0.5f, 0, TH_NONE);
        y += h + 6 * u;
    }
    /* the pop-up in front, low in the middle, fading in and out */
    if (!n_popups) return;
    Popup *p = &popups[0];
    float t = (float)(now - popup_since);
    if (t >= (float)p->ms) {
        memmove(&popups[0], &popups[1], sizeof popups[0] * (size_t)(n_popups - 1));
        n_popups--;
        popup_since = now;
        return;
    }
    float a = t < 250 ? t / 250 : (float)p->ms - t < 400 ? ((float)p->ms - t) / 400 : 1;
    float hp = 10.5f * u, tp = 17 * u, xp = 12.5f * u;
    float w = th_measure(ren, TH_FONT_CAPS, hp, p->head, 2 * u);
    float w1 = th_measure(ren, TH_FONT_UI, tp, p->title, 0), w2 = th_measure(ren, TH_FONT_UI, xp, p->text, 0);
    if (w1 > w) w = w1;
    if (w2 > w) w = w2;
    w += 90 * u;
    if (w > (float)ww - 2 * m) w = (float)ww - 2 * m;
    float h = (p->text[0] ? 74 : 56) * u, x0 = ((float)ww - w) * 0.5f, y0 = (float)wh - h - 46 * u;
    th_plaque(ren, x0, y0, x0 + w, y0 + h, u, a);
    float cx = x0 + w * 0.5f, inner = w - 70 * u;
    th_text(ren, TH_FONT_CAPS, th_fit(ren, TH_FONT_CAPS, hp, p->head, inner, 2 * u), cx, y0 + 15 * u,
            th_fade(TH_GOLD, a), p->head, 0.5f, 0.5f, 2 * u, TH_NONE);
    th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, tp, p->title, inner, 0), cx, y0 + 35 * u, th_fade(TH_TEXT, a),
            p->title, 0.5f, 0.5f, 0, th_fade(th_alpha(TH_EMBER, 150), a));
    if (p->text[0])
        th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, xp, p->text, inner, 0), cx, y0 + 56 * u,
                th_fade(TH_TEXT_DIM, a), p->text, 0.5f, 0.5f, 0, TH_NONE);
}

/* ---- test ------------------------------------------------------------------------------ */

typedef struct Answer {
    bool done;
    int status;
    char *body;
    size_t len;
} Answer;

static void on_answer(int status, const char *body, size_t len, void *userdata)
{
    Answer *a = userdata;
    a->status = status;
    a->body = malloc(len + 1);
    if (a->body) { memcpy(a->body, body, len); a->body[len] = 0; a->len = len; }
    a->done = true;
}

static rc_runtime_t test_runtime;
static bool test_active, test_hit;

static uint32_t RC_CCONV test_peek(uint32_t address, uint32_t num_bytes, void *ud)
{
    (void)ud;
    uint8_t b[4] = {0};
    if (num_bytes > 4 || read_memory(address, b, num_bytes, NULL) < num_bytes) return 0;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void RC_CCONV test_event(const rc_runtime_event_t *e)
{
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) test_hit = true;
}

bool ach_test_trigger(const char *memaddr)
{
    rc_runtime_init(&test_runtime);
    test_hit = false;
    test_active = rc_runtime_activate_achievement(&test_runtime, 1, memaddr, NULL, 0) == RC_OK;
    return test_active;
}

bool ach_test_triggered(void)
{
    if (!test_active) return false;
    if (!test_hit) rc_runtime_do_frame(&test_runtime, test_event, test_peek, NULL, NULL);
    return test_hit;
}

void ach_test_popup(void)
{
    n_popups = 0;
    popup(5000, "Achievement unlocked", "A test of the pop-up", "Shown when RetroAchievements awards one  (10 points)");
    popup_since = SDL_GetTicks() - 1000;    /* fully faded in */
}

int ach_test_identify(const char *cue, char hash[33], char *err, size_t errlen)
{
    if (!hash_disc(cue, hash)) { snprintf(err, errlen, "the disc could not be hashed"); return -1; }
    rc_api_resolve_hash_request_t params;
    memset(&params, 0, sizeof params);
    params.game_hash = hash;
    rc_api_request_t req;
    if (rc_api_init_resolve_hash_request(&req, &params) != RC_OK) { snprintf(err, errlen, "no request"); return -1; }
    Answer a = {0};
    if (!http_post(req.url, req.post_data, agent[0] ? agent : "RondoRecomp/" RONDO_VERSION, on_answer, &a)) {
        snprintf(err, errlen, "%s", http_error());
        rc_api_destroy_request(&req);
        return -1;
    }
    Uint64 until = SDL_GetTicks() + 30000;
    while (!a.done && SDL_GetTicks() < until) {
        http_poll();
        SDL_Delay(10);
    }
    rc_api_destroy_request(&req);
    if (!a.done) { snprintf(err, errlen, "no answer in 30 s"); return -1; }
    rc_api_server_response_t sr;
    sr.body = a.body;
    sr.body_length = a.len;
    sr.http_status_code = a.status ? a.status : RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    rc_api_resolve_hash_response_t resp;
    int r = rc_api_process_resolve_hash_server_response(&resp, &sr);
    int id = -1;
    if (r == RC_OK && resp.response.succeeded) id = (int)resp.game_id;
    else snprintf(err, errlen, "HTTP %d: %.80s", a.status, a.body ? a.body : "");
    rc_api_destroy_resolve_hash_response(&resp);
    free(a.body);
    return id;
}
