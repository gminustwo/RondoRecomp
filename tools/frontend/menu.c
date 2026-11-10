/* See menu.h. */
#include "menu.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "programs.h"
#include "theme.h"
#include "video.h"

enum { PAGE_MAIN, PAGE_VIDEO, PAGE_GAMEPLAY, PAGE_CHEATS, PAGE_CONTROLS, PAGE_START, PAGE_ACH, PAGE_UPDATE, PAGE_COUNT };

/* IT_EDIT: a text field (arg 0: the Settings string at field, n_choices its size; arg 1: the
 * password, kept in the Menu); IT_INFO: a status line, Menu.ach[arg] (0-1), Menu.upd[arg - 2] */
typedef enum { IT_ACTION, IT_PAGE, IT_CHOICE, IT_TOGGLE, IT_TEXT, IT_BACK, IT_BIND, IT_EDIT, IT_INFO } ItemKind;

typedef struct Item {
    ItemKind kind;
    const char *label;
    int arg;                   /* IT_ACTION: MenuResult; IT_PAGE: page; IT_BIND: action (binds.h) */
    size_t field;              /* IT_CHOICE / IT_TOGGLE: offsetof(Settings, ...) */
    const char *const *choices;
    int n_choices;
    int base;                  /* value of choice 0 (slot numbers start at 1) */
    MenuResult on_change;
} Item;

static const char *const OFF_ON[] = {"Off", "On"};
static const char *const ASPECTS[] = {"4:3 (TV)", "Square pixels", "Stretch to fill"};
static const char *const FILTERS[] = {"Sharp", "Nearest", "Smooth"};
static const char *const SIDES[] = {"Blurred picture", "Black"};
static const char *const EFFECTS[] = {"Off", "Scanlines", "CRT", "Curved CRT", "Composite"};
static const char *const EFFECT_STRENGTHS[] = {"Light", "Medium", "Strong"};
static const char *const SCALINGS[] = {"Fit screen", "Whole multiples"};
static const char *const SKIPS[] = {"Press START", "Automatically", "Never"};
static const char *const LANGS[] = {"English patch", "Japanese original"};
static const char *const SLOTS[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
static const char *const VOLUMES[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};

/* a setting's offset, checked against its type at compile time (a bool read as an int
 * picks up the bytes after it) */
#define F_INT(x) _Generic(((Settings *)0)->x, int: offsetof(Settings, x))
#define F_BOOL(x) _Generic(((Settings *)0)->x, bool: offsetof(Settings, x))
#define CHOICE(lbl, fld, arr, base, chg) {IT_CHOICE, lbl, 0, F_INT(fld), arr, (int)(sizeof arr / sizeof arr[0]), base, chg}
#define TOGGLE(lbl, fld, chg) {IT_TOGGLE, lbl, 0, F_BOOL(fld), OFF_ON, 2, 0, chg}
#define BIND(lbl, act) {IT_BIND, lbl, act, 0, NULL, 0, 0, 0}

static const Item MAIN_ITEMS[] = {
    {IT_ACTION, "Resume", MENU_CLOSE, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Save state", MENU_SAVE_STATE, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Load state", MENU_LOAD_STATE, 0, NULL, 0, 0, 0},
    CHOICE("State slot", slot, SLOTS, 1, MENU_SETTINGS_CHANGED),
    {IT_PAGE, "Video", PAGE_VIDEO, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Gameplay", PAGE_GAMEPLAY, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Cheats", PAGE_CHEATS, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Controls", PAGE_CONTROLS, 0, NULL, 0, 0, 0},
    {IT_PAGE, "RetroAchievements", PAGE_ACH, 0, NULL, 0, 0, 0},
    CHOICE("Language", language, LANGS, 0, MENU_LANGUAGE),
    {IT_ACTION, "Reset game", MENU_RESET, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Start screen", MENU_START_SCREEN, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Quit", MENU_QUIT, 0, NULL, 0, 0, 0},
};
static const Item START_ITEMS[] = {
    {IT_ACTION, "Play", MENU_PLAY, 0, NULL, 0, 0, 0},
    CHOICE("Language", language, LANGS, 0, MENU_LANGUAGE),
    {IT_ACTION, "Load state", MENU_LOAD_STATE, 0, NULL, 0, 0, 0},
    CHOICE("State slot", slot, SLOTS, 1, MENU_SETTINGS_CHANGED),
    {IT_PAGE, "Video", PAGE_VIDEO, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Gameplay", PAGE_GAMEPLAY, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Cheats", PAGE_CHEATS, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Controls", PAGE_CONTROLS, 0, NULL, 0, 0, 0},
    {IT_PAGE, "RetroAchievements", PAGE_ACH, 0, NULL, 0, 0, 0},
    {IT_PAGE, "Updates", PAGE_UPDATE, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Setup (add a disc, rebuild)", MENU_SETUP, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Quit", MENU_QUIT, 0, NULL, 0, 0, 0},
};
static const Item VIDEO_ITEMS[] = {
    TOGGLE("Fullscreen", fullscreen, MENU_VIDEO_CHANGED),
    TOGGLE("VSync", vsync, MENU_VIDEO_CHANGED),
    CHOICE("Aspect ratio", aspect, ASPECTS, 0, MENU_SETTINGS_CHANGED),
    CHOICE("Size", scaling, SCALINGS, 0, MENU_SETTINGS_CHANGED),
    CHOICE("Filter", filter, FILTERS, 0, MENU_SETTINGS_CHANGED),
    CHOICE("Screen effect", effect, EFFECTS, 0, MENU_SETTINGS_CHANGED),
    CHOICE("Effect strength", effect_strength, EFFECT_STRENGTHS, 0, MENU_SETTINGS_CHANGED),
    CHOICE("Beside the picture", sides, SIDES, 0, MENU_SETTINGS_CHANGED),
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};
static const Item GAMEPLAY_ITEMS[] = {
    CHOICE("Skip cinematics", skip, SKIPS, 0, MENU_SETTINGS_CHANGED),
    TOGGLE("Rewind", rewind, MENU_SETTINGS_CHANGED),
    CHOICE("Volume", volume, VOLUMES, 0, MENU_SETTINGS_CHANGED),
    TOGGLE("Start screen at launch", start_screen, MENU_SETTINGS_CHANGED),
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};
static const Item CHEAT_ITEMS[] = {
    TOGGLE("Infinite health", cheat_health, MENU_SETTINGS_CHANGED),
    TOGGLE("Infinite hearts", cheat_hearts, MENU_SETTINGS_CHANGED),
    TOGGLE("Infinite lives", cheat_lives, MENU_SETTINGS_CHANGED),
    TOGGLE("Maria unlocked", maria_unlocked, MENU_SETTINGS_CHANGED),
    TOGGLE("Play as Maria", play_maria, MENU_SETTINGS_CHANGED),
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};
static const Item CONTROL_ITEMS[] = {
    {IT_TEXT, "\tController\tKeyboard", 0, 0, NULL, 0, 0, 0},
    {IT_TEXT, "Move\tD-pad / stick\tArrows", 0, 0, NULL, 0, 0, 0},
    BIND("Jump", ACT_JUMP),
    BIND("Attack", ACT_ATTACK),
    BIND("Sub-weapon", ACT_SUB),
    BIND("Item Crash", ACT_CRASH),
    BIND("Backflip", ACT_FLIP),
    BIND("Run (hold)", ACT_RUN),
    BIND("Pause", ACT_PAUSE),
    BIND("Menu", ACT_MENU),
    BIND("Rewind (hold)", ACT_REWIND),
    BIND("Save state", ACT_SAVE),
    BIND("Load state", ACT_LOAD),
    {IT_TEXT, "Slot / fullscreen\t\tF6 F7 / F11", 0, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Reset controls", MENU_RESET_CONTROLS, 0, NULL, 0, 0, 0},
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};

static const Item ACH_ITEMS[] = {
    TOGGLE("RetroAchievements", ra_enabled, MENU_ACH_CHANGED),
    TOGGLE("Hardcore mode", ra_hardcore, MENU_ACH_CHANGED),
    {IT_EDIT, "User name", 0, offsetof(Settings, ra_user), NULL, (int)sizeof(((Settings *)0)->ra_user), 0, 0},
    {IT_EDIT, "Password", 1, 0, NULL, (int)sizeof(((Menu *)0)->password), 0, 0},
    {IT_ACTION, "Log in", MENU_ACH_LOGIN, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Log out", MENU_ACH_LOGOUT, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 0, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 1, 0, NULL, 0, 0, 0},
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};

static const Item UPDATE_ITEMS[] = {
    {IT_INFO, "", 2, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 3, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 4, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 5, 0, NULL, 0, 0, 0},
    {IT_INFO, "", 6, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Download and install", MENU_UPDATE_INSTALL, 0, NULL, 0, 0, 0},
    {IT_ACTION, "Check now", MENU_UPDATE_CHECK, 0, NULL, 0, 0, 0},
    TOGGLE("Check at launch", check_updates, MENU_SETTINGS_CHANGED),
    {IT_ACTION, "Release page", MENU_UPDATE_PAGE, 0, NULL, 0, 0, 0},
    {IT_BACK, "Back", 0, 0, NULL, 0, 0, 0},
};

/* parent -1: a root (the in-game menu, the start screen); the option pages return to
 * whichever root opened them */
static const struct { const char *title; const Item *items; int n; int parent; } PAGES[PAGE_COUNT] = {
    {"Paused", MAIN_ITEMS, (int)(sizeof MAIN_ITEMS / sizeof MAIN_ITEMS[0]), -1},
    {"Video", VIDEO_ITEMS, (int)(sizeof VIDEO_ITEMS / sizeof VIDEO_ITEMS[0]), PAGE_MAIN},
    {"Gameplay", GAMEPLAY_ITEMS, (int)(sizeof GAMEPLAY_ITEMS / sizeof GAMEPLAY_ITEMS[0]), PAGE_MAIN},
    {"Cheats", CHEAT_ITEMS, (int)(sizeof CHEAT_ITEMS / sizeof CHEAT_ITEMS[0]), PAGE_MAIN},
    {"Controls", CONTROL_ITEMS, (int)(sizeof CONTROL_ITEMS / sizeof CONTROL_ITEMS[0]), PAGE_MAIN},
    {"Main Menu", START_ITEMS, (int)(sizeof START_ITEMS / sizeof START_ITEMS[0]), -1},
    {"Achievements", ACH_ITEMS, (int)(sizeof ACH_ITEMS / sizeof ACH_ITEMS[0]), PAGE_MAIN},
    {"Updates", UPDATE_ITEMS, (int)(sizeof UPDATE_ITEMS / sizeof UPDATE_ITEMS[0]), PAGE_MAIN},
};

/* What the selected item does: the footer's help line. */
static const struct { const char *label, *help; } HELP[] = {
    {"Play", "Start the game: on from power-on to its title screen."},
    {"Language", "The English patch (fan translation) or the Japanese original: each is its own disc."},
    {"Load state", "Continue from the save state in the slot below."},
    {"State slot", "Which of the nine save-state slots to use (F6 / F7 in the game)."},
    {"Video", "Fullscreen, the picture's shape, size, filter and screen effect."},
    {"Gameplay", "Story scenes, rewind, volume and the start screen."},
    {"Cheats", "Infinite health, hearts and lives; Maria."},
    {"Controls", "The buttons and keys: change any of them."},
    {"Setup (add a disc, rebuild)", "Open Setup to add the other language's disc or rebuild; this closes."},
    {"Quit", "Close Rondo Recomp."},
    {"Resume", "Back to the game."},
    {"Save state", "Keep this moment in the slot below."},
    {"Reset game", "Restart the game from power-on."},
    {"Start screen", "Leave the game for the start screen."},
    {"Fullscreen", "Use the whole screen at its own resolution (F11)."},
    {"VSync", "Show each frame when the screen refreshes: no tearing. Off: at once, maybe torn."},
    {"Aspect ratio", "4:3 as on a TV, square pixels, or stretched to fill."},
    {"Size", "As big as the screen allows, or whole multiples only."},
    {"Filter", "Sharp: crisp, even pixels. Nearest: raw. Smooth: softened."},
    {"Screen effect", "Scanlines, a CRT's glow and phosphors (flat or curved glass), or a soft composite TV."},
    {"Effect strength", "How strong the screen effect is."},
    {"Beside the picture", "What fills the screen beside a narrower picture."},
    {"Skip cinematics", "Logo and intro: START, A or X goes to the title screen. Story scenes: START runs them fast."},
    {"Rewind", "Hold the Rewind control (LT / R unless changed) to play the game backwards."},
    {"Reset controls", "Every button and key back to its default."},
    {"Volume", "Sound volume."},
    {"Start screen at launch", "Off: " GAME_PROGRAM " goes straight into the game."},
    {"Infinite health", "Health never goes down."},
    {"Infinite hearts", "Hearts stay at 99."},
    {"Infinite lives", "Lives never run out."},
    {"Maria unlocked", "The game offers Maria, as after rescuing her in Stage 2."},
    {"Play as Maria", "Every stage, the Prologue too, is played as Maria: from the next stage."},
    {"Back", "Back to the previous page."},
    {"RetroAchievements", "Earn achievements at retroachievements.org: on / off, your account."},
    {"Hardcore mode", "No save-state loading, rewind or cheats; unlocks count as hardcore. On restarts the game."},
    {"User name", "Your retroachievements.org user name: A / Enter, type it, then Enter."},
    {"Password", "Used once to log in; only the login token is kept. A / Enter, type it, then Enter."},
    {"Log in", "Log in to RetroAchievements with the name and password above."},
    {"Log out", "Forget the login on this PC."},
    {"Updates", "A newer version of Rondo Recomp: what is new, download and install it."},
    {"Download and install", "Setup downloads the new version, puts it in place and rebuilds the game; it restarts."},
    {"Check now", "Ask GitHub for the latest version again."},
    {"Check at launch", "Ask GitHub (github.com/" "gminustwo/RondoRecomp) for a newer version at each launch."},
    {"Release page", "Open the new version's page on GitHub in your web browser."},
};

static int parent_of(const Menu *m, int page)
{
    int p = PAGES[page].parent;
    return p == PAGE_MAIN ? m->root : p;
}

static int get_value(const Settings *s, const Item *it)
{
    const char *p = (const char *)s + it->field;
    if (it->kind == IT_TOGGLE) return *(const bool *)p ? 1 : 0;
    int v = *(const int *)p - it->base;
    return v < 0 ? 0 : v >= it->n_choices ? it->n_choices - 1 : v;
}

static void set_value(Settings *s, const Item *it, int v)
{
    char *p = (char *)s + it->field;
    v = (v % it->n_choices + it->n_choices) % it->n_choices;
    if (it->kind == IT_TOGGLE) *(bool *)p = v != 0;
    else *(int *)p = v + it->base;
}

static bool selectable(const Item *it) { return it->kind != IT_TEXT && it->kind != IT_INFO; }

/* a text field's characters and their room */
static char *edit_buffer(Menu *m, Settings *s, const Item *it, size_t *cap)
{
    *cap = (size_t)it->n_choices;
    return it->arg == 1 ? m->password : (char *)s + it->field;
}

static int first_selectable(int page)
{
    for (int i = 0; i < PAGES[page].n; i++)
        if (selectable(&PAGES[page].items[i])) return i;
    return 0;
}

void menu_open(Menu *m)
{
    m->open = true;
    m->page = m->root = PAGE_MAIN;
    m->sel[PAGE_MAIN] = 0;
    m->status[0] = 0;
    m->opened_ms = SDL_GetTicks();
}

void menu_open_start_screen(Menu *m)
{
    m->open = true;
    m->page = m->root = PAGE_START;
    m->sel[PAGE_START] = 0;
    m->status[0] = 0;
    m->opened_ms = SDL_GetTicks();
}

bool menu_on_start_screen(const Menu *m) { return m->open && m->root == PAGE_START; }

MenuResult menu_nav(Menu *m, Settings *s, Nav nav)
{
    if (m->updating) return MENU_NOTHING;   /* Setup is replacing the game's files */
    const Item *items = PAGES[m->page].items;
    int n = PAGES[m->page].n;
    int *sel = &m->sel[m->page];
    if (!selectable(&items[*sel])) {
        while (*sel < n - 1 && !selectable(&items[*sel])) (*sel)++;
    }
    const Item *it = &items[*sel];
    switch (nav) {
    case NAV_UP:
    case NAV_DOWN: {
        int step = nav == NAV_UP ? -1 : 1, i = *sel;
        do { i = (i + step + n) % n; } while (!selectable(&items[i]) && i != *sel);
        *sel = i;
        return MENU_NOTHING;
    }
    case NAV_LEFT:
    case NAV_RIGHT:
        if (it->kind == IT_CHOICE || it->kind == IT_TOGGLE) {
            set_value(s, it, get_value(s, it) + (nav == NAV_LEFT ? -1 : 1));
            return it->on_change;
        }
        return MENU_NOTHING;
    case NAV_OK:
        switch (it->kind) {
        case IT_ACTION:
            if (it->arg == MENU_RESET_CONTROLS) {
                bind_defaults(s->bind_pad, s->bind_key);
                snprintf(m->status, sizeof m->status, "Every control is back to its default.");
                return MENU_SETTINGS_CHANGED;
            }
            return (MenuResult)it->arg;
        case IT_BIND:
            m->capturing = it->arg + 1;
            m->capture_until = SDL_GetTicks() + 5000;
            m->n_held = m->n_last = m->held_key = m->last_key = 0;
            m->key_after = false;
            snprintf(m->status, sizeof m->status, "Hold its button, two buttons (Back, then RB) or key until the count ends.");
            return MENU_NOTHING;
        case IT_EDIT:
            m->editing = *sel + 1;
            snprintf(m->status, sizeof m->status, "Type, then Enter.  Steam Deck: Steam + X if the keyboard does not open.");
            return MENU_TEXT_START;
        case IT_PAGE: m->page = it->arg; m->sel[m->page] = first_selectable(m->page); return MENU_NOTHING;
        case IT_BACK: m->page = parent_of(m, m->page); return MENU_NOTHING;
        case IT_CHOICE:
        case IT_TOGGLE: set_value(s, it, get_value(s, it) + 1); return it->on_change;
        default: return MENU_NOTHING;
        }
    case NAV_BACK:
        if (PAGES[m->page].parent >= 0) m->page = parent_of(m, m->page);
        else if (m->root != PAGE_START) return MENU_CLOSE;    /* the start screen stays */
        return MENU_NOTHING;
    case NAV_CLEAR_PAD:       /* X / Delete: a control's controller button / key */
    case NAV_CLEAR_KEY:
        if (it->kind == IT_BIND) {
            const char *msg = bind_clear(s->bind_pad, s->bind_key, it->arg, nav == NAV_CLEAR_PAD);
            snprintf(m->status, sizeof m->status, "%s", msg);
            return MENU_SETTINGS_CHANGED;
        }
        return MENU_NOTHING;
    case NAV_MENU:            /* START / F1: on the start screen, play */
        if (m->root != PAGE_START) return MENU_CLOSE;
        if (m->page == PAGE_START) return MENU_PLAY;
        m->page = PAGE_START;
        return MENU_NOTHING;
    default:
        return MENU_NOTHING;
    }
}

bool menu_capturing(const Menu *m) { return m->open && m->capturing > 0; }

bool menu_editing(const Menu *m) { return m->open && m->editing > 0; }

MenuResult menu_text(Menu *m, Settings *s, const SDL_Event *ev)
{
    if (!m->editing) return MENU_NOTHING;
    const Item *it = &PAGES[m->page].items[m->editing - 1];
    size_t cap;
    char *buf = edit_buffer(m, s, it, &cap);
    bool end = false;
    switch (ev->type) {
    case SDL_EVENT_TEXT_INPUT: {
        size_t l = strlen(buf), add = strlen(ev->text.text);
        if (l + add < cap) memcpy(buf + l, ev->text.text, add + 1);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.scancode == SDL_SCANCODE_BACKSPACE) {
            size_t l = strlen(buf);
            while (l > 0 && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) l--;   /* a whole UTF-8 character */
            if (l > 0) l--;
            buf[l] = 0;
        } else if (ev->key.scancode == SDL_SCANCODE_RETURN || ev->key.scancode == SDL_SCANCODE_KP_ENTER ||
                   ev->key.scancode == SDL_SCANCODE_ESCAPE || ev->key.scancode == SDL_SCANCODE_TAB) {
            end = true;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        end = ev->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || ev->gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
              ev->gbutton.button == SDL_GAMEPAD_BUTTON_START;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        end = true;
        break;
    default:
        break;
    }
    if (!end) return MENU_NOTHING;
    m->editing = 0;
    m->status[0] = 0;
    return MENU_TEXT_END;
}

/* What the count would give now: CAPTURED and the control (*is_pad: a controller binding,
 * else a key), CAPTURED_NOTHING, or CAPTURED_TOO_MANY (three buttons or more). Buttons held
 * win, then a key held, then whatever was pressed last. */
enum { CAPTURED_NOTHING, CAPTURED, CAPTURED_TOO_MANY };

static int buttons_binding(const int *b, int n, int *value)
{
    if (n > 2) return CAPTURED_TOO_MANY;
    *value = n == 2 ? BIND_COMBO(b[0], b[1]) : b[0];   /* the first pressed is the one held */
    return CAPTURED;
}

static int captured(const Menu *m, bool *is_pad, int *value)
{
    *is_pad = true;
    if (m->n_held) return buttons_binding(m->held, m->n_held, value);
    *is_pad = false;
    *value = m->held_key ? m->held_key : m->last_key;
    if (m->held_key || (m->key_after && m->last_key)) return CAPTURED;
    *is_pad = true;
    if (m->n_last) return buttons_binding(m->last, m->n_last, value);
    return CAPTURED_NOTHING;
}

MenuResult menu_update(Menu *m, Settings *s)
{
    if (!m->capturing || SDL_GetTicks() < m->capture_until) return MENU_NOTHING;
    int act = m->capturing - 1;
    m->capturing = 0;
    bool is_pad;
    int v, got = captured(m, &is_pad, &v);
    if (got != CAPTURED) {
        snprintf(m->status, sizeof m->status, got == CAPTURED_TOO_MANY ? "Two buttons at most: %s left as it was."
                                                                       : "Nothing held: %s left as it was.",
                 bind_action(act)->name);
        return MENU_NOTHING;
    }
    int before_pad[ACT_COUNT], before_key[ACT_COUNT];
    memcpy(before_pad, s->bind_pad, sizeof before_pad);
    memcpy(before_key, s->bind_key, sizeof before_key);
    const char *msg = bind_assign(s->bind_pad, s->bind_key, act, is_pad, v);
    snprintf(m->status, sizeof m->status, "%s", msg ? msg : "");
    return memcmp(before_pad, s->bind_pad, sizeof before_pad) || memcmp(before_key, s->bind_key, sizeof before_key)
               ? MENU_SETTINGS_CHANGED
               : MENU_NOTHING;
}

MenuResult menu_capture(Menu *m, Settings *s, const Input *in, const SDL_Event *ev)
{
    (void)s;
    if (!m->capturing) return MENU_NOTHING;
    int act = m->capturing - 1;
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        (ev->type == SDL_EVENT_KEY_DOWN && ev->key.scancode == SDL_SCANCODE_ESCAPE)) {
        m->capturing = 0;
        snprintf(m->status, sizeof m->status, "%s: left as it was.", bind_action(act)->name);
        return MENU_NOTHING;
    }
    /* follow what is held (from the start of the count: the A that chose the row is not) */
    bool down;
    int v;
    if (input_pad_event(in, ev, &v, &down)) {
        int i = 0;
        while (i < m->n_held && m->held[i] != v) i++;
        if (!down) {                                   /* let go: out of the held ones */
            if (i < m->n_held) {
                memmove(&m->held[i], &m->held[i + 1], (size_t)(m->n_held - i - 1) * sizeof m->held[0]);
                m->n_held--;
            }
        } else if (bind_pad_reserved(v)) {
            snprintf(m->status, sizeof m->status, "%s is kept for %s.", bind_pad_name(v),
                     v == SDL_GAMEPAD_BUTTON_GUIDE ? "the system" : "moving");
        } else if (i == m->n_held && m->n_held < (int)(sizeof m->held / sizeof m->held[0])) {
            m->held[m->n_held++] = v;
            memcpy(m->last, m->held, sizeof m->last);
            m->n_last = m->n_held;
            m->key_after = false;
        }
    } else if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        int k = ev->key.scancode;
        if (bind_key_reserved(k)) {
            snprintf(m->status, sizeof m->status, "%s is kept for %s.", bind_key_name(k),
                     k == SDL_SCANCODE_F6 || k == SDL_SCANCODE_F7 ? "the state slot"
                     : k == SDL_SCANCODE_F11 ? "fullscreen" : "moving");
        } else {
            m->held_key = m->last_key = k;
            m->key_after = true;
        }
    } else if (ev->type == SDL_EVENT_KEY_UP && (int)ev->key.scancode == m->held_key) {
        m->held_key = 0;
    }
    return MENU_NOTHING;
}

static const char *item_help(int page, const Item *it)
{
    if (it->kind == IT_BIND)
        return bind_action(it->arg)->clearable
                   ? "A / Enter, then hold its button (or two) or key until the count ends. X / Delete clears it."
                   : "A / Enter, then hold its button (or two) or key until the count ends. One in use swaps over.";
    if (page == PAGE_CONTROLS && it->kind == IT_BACK)
        return "Sub-weapon is also Up + Attack, Backflip is Jump twice: the original inputs work too.";
    for (size_t i = 0; i < sizeof HELP / sizeof HELP[0]; i++)
        if (!strcmp(HELP[i].label, it->label)) return HELP[i].help;
    return "";
}

/* Where everything goes for a page (layout units u: 1 at 960 x 540), shared by drawing and
 * the mouse. The start screen's own page has its panel on the right, the rose window and the
 * title on the left; the others are one panel in the middle. */
typedef struct Frame {
    int w, h;
    float u, cx, cy;
    float x0, x1, y0, y1;        /* the panel: y0 is its arch's apex */
    float rise;                  /* the arch's height */
    float title_y, sub_y, div_y, foot_y;
    float rows_top, row_h, text_px;
    bool start;
} Frame;

static Frame frame_for(const Menu *m, SDL_Renderer *ren)
{
    Frame F;
    SDL_GetRenderOutputSize(ren, &F.w, &F.h);
    float u = F.u = th_unit(F.w, F.h);
    F.cx = (float)F.w * 0.5f;
    F.cy = (float)F.h * 0.5f;
    F.start = m->page == PAGE_START;
    /* as tall as the page needs (arch and title, rows of 30, footer), at most 500 */
    int rows = PAGES[m->page].n;
    float head = F.start ? 78 : 92;
    float height = (head + 30 + (float)(rows - 1) * 30 + 18 + 54 + 12) * u;
    if (height > 500 * u) height = 500 * u;
    F.x0 = F.start ? F.cx + 20 * u : F.cx - 300 * u;
    F.x1 = F.start ? F.cx + 440 * u : F.cx + 300 * u;
    F.y0 = F.cy - height * 0.5f + 6 * u;
    F.y1 = F.cy + height * 0.5f + 6 * u;
    F.rise = 26 * u;
    F.title_y = F.y0 + 50 * u;
    F.sub_y = F.y0 + 75 * u;
    F.div_y = F.y0 + head * u;
    F.foot_y = F.y1 - 54 * u;
    F.rows_top = F.div_y + 30 * u;
    float span = (F.foot_y - 18 * u) - F.rows_top;
    F.row_h = rows > 1 ? span / (float)(rows - 1) : 30 * u;
    if (F.row_h > 30 * u) F.row_h = 30 * u;
    F.text_px = F.row_h * 0.62f;
    if (F.text_px > 18 * u) F.text_px = 18 * u;
    return F;
}

static float row_y(const Frame *F, int i) { return F->rows_top + (float)i * F->row_h; }

int menu_item_at(const Menu *m, SDL_Renderer *ren, float x, float y)
{
    Frame F = frame_for(m, ren);
    if (x < F.x0 + 16 * F.u || x > F.x1 - 16 * F.u) return -1;
    for (int i = 0; i < PAGES[m->page].n; i++) {
        float yc = row_y(&F, i);
        if (y >= yc - F.row_h * 0.5f && y < yc + F.row_h * 0.5f)
            return selectable(&PAGES[m->page].items[i]) ? i : -1;
    }
    return -1;
}

int menu_find(const Menu *m, const char *label)
{
    for (int i = 0; i < PAGES[m->page].n; i++)
        if (!strcmp(PAGES[m->page].items[i].label, label)) return i;
    return -1;
}

bool menu_item_center(const Menu *m, SDL_Renderer *ren, int item, float *x, float *y)
{
    if (item < 0 || item >= PAGES[m->page].n) return false;
    Frame F = frame_for(m, ren);
    *x = (F.x0 + F.x1) * 0.5f;
    *y = row_y(&F, item);
    return true;
}

void menu_hover(Menu *m, int item)
{
    if (item >= 0 && item < PAGES[m->page].n && selectable(&PAGES[m->page].items[item])) m->sel[m->page] = item;
}

MenuResult menu_click(Menu *m, Settings *s, int item)
{
    if (item < 0 || item >= PAGES[m->page].n || !selectable(&PAGES[m->page].items[item])) return MENU_NOTHING;
    m->sel[m->page] = item;
    return menu_nav(m, s, NAV_OK);
}

/* The title (blackletter) under the arch's apex, a line naming the game on the in-game
 * pages, the ornamented divider. */
static void draw_header(SDL_Renderer *ren, const Frame *F, const char *title, bool game_line, float a)
{
    float u = F->u, mx = (F->x0 + F->x1) * 0.5f;
    th_text(ren, TH_FONT_TITLE, 34 * u, mx, F->title_y, th_fade(TH_TEXT, a), title, 0.5f, 0.5f, 1 * u,
            th_fade(th_alpha(TH_EMBER, 150), a));
    if (game_line)
        th_text(ren, TH_FONT_UI, 10.5f * u, mx, F->sub_y, th_fade(TH_GOLD, a), "CASTLEVANIA  -  RONDO OF BLOOD", 0.5f,
                0.5f, 3 * u, TH_NONE);
    th_divider(ren, F->x0 + 22 * u, F->x1 - 22 * u, F->div_y, u, a);
}

/* A row of the controls table: columns separated by tabs; a first column left empty makes
 * it the table's heading. */
static void draw_table_row(SDL_Renderer *ren, const Frame *F, float y, const char *text, float px, float a)
{
    float u = F->u, cols[3] = {F->x0 + 54 * u, F->x0 + 280 * u, F->x0 + 440 * u};
    bool heading = text[0] == '\t';
    const char *p = text;
    for (int c = 0; c < 3; c++) {
        const char *e = strchr(p, '\t');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char part[96];
        if (n >= sizeof part) n = sizeof part - 1;
        memcpy(part, p, n);
        part[n] = 0;
        SDL_Color col = heading ? TH_GOLD : c == 0 ? TH_TEXT : TH_ACTION;
        th_text(ren, heading ? TH_FONT_CAPS : TH_FONT_UI, heading ? px * 0.85f : px, cols[c], y, th_fade(col, a), part,
                0, 0.5f, heading ? 2.0f * u : 0, TH_NONE);
        if (!e) break;
        p = e + 1;
    }
}

static void draw_rows(SDL_Renderer *ren, const Frame *F, const Menu *m, const Settings *s, float t, float a)
{
    float u = F->u, half = F->row_h * 0.46f;
    const Item *items = PAGES[m->page].items;
    for (int i = 0; i < PAGES[m->page].n; i++) {
        const Item *it = &items[i];
        float y = row_y(F, i);
        bool sel = i == m->sel[m->page] && selectable(it);
        bool action = it->kind == IT_ACTION || it->kind == IT_PAGE || it->kind == IT_BACK;
        if (sel) {
            /* a bar of candlelight fading to the right between gold hairlines, a cross for a cursor */
            th_rect_h(ren, F->x0 + 16 * u, y - half, F->x1 - 16 * u, y + half, th_fade(th_alpha(TH_CANDLE, 110), a),
                      th_fade(th_alpha(TH_CANDLE, 6), a));
            th_rect_h(ren, F->x0 + 16 * u, y - half, F->x1 - 16 * u, y - half + 1.0f, th_fade(th_alpha(TH_GOLD, 150), a),
                      th_fade(th_alpha(TH_GOLD, 0), a));
            th_rect_h(ren, F->x0 + 16 * u, y + half - 1.0f, F->x1 - 16 * u, y + half, th_fade(th_alpha(TH_GOLD, 150), a),
                      th_fade(th_alpha(TH_GOLD, 0), a));
            float bob = sinf(t * 6.0f) * 2.0f * u;
            th_cross(ren, F->x0 + 34 * u + bob, y, F->row_h * 0.42f, th_fade((SDL_Color){255, 230, 156, 255}, a),
                     th_fade((SDL_Color){190, 128, 44, 255}, a), th_fade((SDL_Color){60, 20, 10, 255}, a), 1.0f * u);
        } else if (it->kind != IT_TEXT) {
            th_rect_h(ren, F->x0 + 30 * u, y + F->row_h * 0.48f, (F->x0 + F->x1) * 0.5f, y + F->row_h * 0.48f + 1.0f,
                      th_alpha(TH_GOLD, 0), th_fade(th_alpha(TH_GOLD, 26), a));
            th_rect_h(ren, (F->x0 + F->x1) * 0.5f, y + F->row_h * 0.48f, F->x1 - 30 * u, y + F->row_h * 0.48f + 1.0f,
                      th_fade(th_alpha(TH_GOLD, 26), a), th_alpha(TH_GOLD, 0));
        }
        if (it->kind == IT_TEXT) {
            draw_table_row(ren, F, y, it->label, F->text_px * 0.95f, a);
            continue;
        }
        if (it->kind == IT_INFO) {
            const char *line = it->arg < 2 ? m->ach[it->arg] : m->upd[it->arg - 2];
            float w = (F->x1 - F->x0) - 80 * u;
            if (it->arg == 4 && m->updating && m->upd_pct >= 0) {   /* the step's progress */
                float bx0 = F->x0 + 60 * u, bx1 = F->x1 - 60 * u, h = F->row_h * 0.16f;
                float fill = bx0 + (bx1 - bx0) * (float)(m->upd_pct > 100 ? 100 : m->upd_pct) / 100.0f;
                th_rect(ren, bx0, y - h, bx1, y + h, th_fade((SDL_Color){20, 16, 14, 220}, a));
                th_rect_h(ren, bx0, y - h, fill, y + h, th_fade(th_alpha(TH_EMBER, 230), a), th_fade(TH_CANDLE, a));
                th_rect_h(ren, bx0, y - h - 1.0f, bx1, y - h, th_fade(th_alpha(TH_GOLD, 120), a), th_fade(th_alpha(TH_GOLD, 40), a));
                continue;
            }
            bool news = it->arg == 3 && (m->notice[0] || m->updating);   /* the line that matters */
            th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, F->text_px * (news ? 0.95f : 0.85f), line, w, 0),
                    (F->x0 + F->x1) * 0.5f, y,
                    th_fade(news ? (SDL_Color){255, 222, 150, 255} : it->arg >= 4 ? TH_TEXT : th_alpha(TH_GOLD, 220), a), line,
                    0.5f, 0.5f, 0, news ? th_fade(th_alpha(TH_EMBER, 150), a) : TH_NONE);
            continue;
        }
        if (it->kind == IT_BIND) {
            char row[160];
            if (m->capturing == it->arg + 1) {
                Uint64 now = SDL_GetTicks();
                int left = m->capture_until > now ? (int)((m->capture_until - now + 999) / 1000) : 0;
                bool is_pad;
                int v, got = captured(m, &is_pad, &v);   /* what it would take now */
                snprintf(row, sizeof row, "%s\t%s   %d", it->label,
                         got == CAPTURED_TOO_MANY ? "Two buttons at most"
                         : got == CAPTURED        ? (is_pad ? bind_pad_name(v) : bind_key_name(v))
                                                  : "Hold it now...",
                         left);
            } else {
                snprintf(row, sizeof row, "%s\t%s\t%s", it->label, bind_pad_name(s->bind_pad[it->arg]),
                         bind_key_name(s->bind_key[it->arg]));
            }
            draw_table_row(ren, F, y, row, F->text_px * 0.95f, a);
            continue;
        }
        char label[96];
        snprintf(label, sizeof label, "%s", it->label);
        if (it->kind == IT_ACTION && (it->arg == MENU_SAVE_STATE || it->arg == MENU_LOAD_STATE))
            snprintf(label, sizeof label, "%s  (slot %d)", it->label, s->slot);
        bool value = it->kind == IT_CHOICE || it->kind == IT_TOGGLE || it->kind == IT_EDIT;
        float label_w = (F->x1 - F->x0) - (value ? 300 : 110) * u;
        SDL_Color lc = sel ? (SDL_Color){255, 248, 232, 255} : action ? TH_ACTION : TH_TEXT;
        th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, F->text_px, label, label_w, 0), F->x0 + 56 * u, y,
                th_fade(lc, a), label, 0, 0.5f, 0, sel ? th_fade(th_alpha(TH_EMBER, 170), a) : TH_NONE);
        if (it->kind == IT_EDIT) {
            /* the field: what is typed (the password as stars), a caret while typing */
            size_t cap;
            const char *text = edit_buffer((Menu *)m, (Settings *)s, it, &cap);
            char shown[160];
            size_t n = strlen(text);
            if (it->arg == 1) {
                if (n > 40) n = 40;
                memset(shown, '*', n);
                shown[n] = 0;
            } else {
                snprintf(shown, sizeof shown, "%s", text);
            }
            bool typing = m->editing == i + 1;
            if (typing && (SDL_GetTicks() / 500) % 2 == 0) strncat(shown, "_", sizeof shown - strlen(shown) - 1);
            if (!shown[0] && !typing) snprintf(shown, sizeof shown, "-");
            float box_x0 = F->x1 - 230 * u, box_x1 = F->x1 - 22 * u;
            th_rect(ren, box_x0, y - F->row_h * 0.36f, box_x1, y + F->row_h * 0.36f,
                    th_fade(typing ? (SDL_Color){60, 44, 26, 230} : (SDL_Color){20, 16, 14, 200}, a));
            th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, F->text_px, shown, box_x1 - box_x0 - 16 * u, 0), box_x0 + 8 * u, y,
                    th_fade(typing ? (SDL_Color){255, 230, 170, 255} : th_alpha(TH_GOLD, 210), a), shown, 0, 0.5f, 0, TH_NONE);
        } else if (value) {
            const char *v = it->choices[get_value(s, it)];
            th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, F->text_px, v, 168 * u, 0), F->x1 - 120 * u, y,
                    th_fade(sel ? (SDL_Color){255, 214, 120, 255} : th_alpha(TH_GOLD, 210), a), v, 0.5f, 0.5f, 0,
                    TH_NONE);
            SDL_Color arrow = th_fade(sel ? TH_GOLD : th_alpha(TH_GOLD, 80), a);
            th_arrow(ren, F->x1 - 214 * u, y, F->row_h * 0.34f, false, arrow);
            th_arrow(ren, F->x1 - 26 * u, y, F->row_h * 0.34f, true, arrow);
        } else if (it->kind == IT_PAGE) {
            th_arrow(ren, F->x1 - 26 * u, y, F->row_h * 0.34f, true, th_fade(sel ? TH_GOLD : th_alpha(TH_GOLD, 120), a));
            if (it->arg == PAGE_UPDATE && m->upd_badge[0]) {   /* a newer version: says so, softly glowing */
                float glow = 0.75f + 0.25f * sinf(t * 3.0f);
                th_text(ren, TH_FONT_UI, F->text_px * 0.85f, F->x1 - 44 * u, y,
                        th_fade((SDL_Color){255, 214, 120, (Uint8)(255 * glow)}, a), m->upd_badge, 1.0f, 0.5f, 0,
                        th_fade(th_alpha(TH_EMBER, 160), a));
            }
        }
    }
}

/* What the selected item does (or the last message), then the buttons. */
static void draw_footer(SDL_Renderer *ren, const Frame *F, const Menu *m, float a)
{
    float u = F->u, w = F->x1 - F->x0 - 56 * u, mx = (F->x0 + F->x1) * 0.5f;
    th_rect_h(ren, F->x0 + 24 * u, F->foot_y, mx, F->foot_y + 1.0f, th_alpha(TH_GOLD, 0), th_fade(th_alpha(TH_GOLD, 110), a));
    th_rect_h(ren, mx, F->foot_y, F->x1 - 24 * u, F->foot_y + 1.0f, th_fade(th_alpha(TH_GOLD, 110), a), th_alpha(TH_GOLD, 0));
    const Item *it = &PAGES[m->page].items[m->sel[m->page]];
    const char *help = m->status[0] ? m->status : selectable(it) && !m->updating ? item_help(m->page, it) : "";
    th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, 13.5f * u, help, w, 0), mx, F->foot_y + 18 * u,
            th_fade(m->status[0] ? TH_ACTION : TH_TEXT, a), help, 0.5f, 0.5f, 0, TH_NONE);
    const char *hint = m->updating
                           ? "UPDATING  -  THE GAME RESTARTS WHEN IT IS DONE"
                       : m->editing
                           ? "TYPE     BACKSPACE  DELETE     ENTER / A  DONE"
                       : m->capturing
                           ? "HOLD THE BUTTONS OR KEY  UNTIL THE COUNT ENDS     ESC  LEAVE IT"
                       : m->page == PAGE_START
                           ? "UP / DOWN  CHOOSE     LEFT / RIGHT  CHANGE     A / ENTER  SELECT     START  PLAY"
                       : m->page == PAGE_MAIN
                           ? "UP / DOWN  CHOOSE     LEFT / RIGHT  CHANGE     A / ENTER  SELECT     B / ESC  RESUME"
                           : "UP / DOWN  CHOOSE     LEFT / RIGHT  CHANGE     A / ENTER  SELECT     B / ESC  BACK";
    th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, 9.5f * u, hint, w, 1.2f * u), mx, F->foot_y + 38 * u,
            th_fade(TH_TEXT_DIM, a), hint, 0.5f, 0.5f, 1.2f * u, TH_NONE);
}

/* The start screen's left side: the rose window, the title, the disc. */
static void draw_start_title(SDL_Renderer *ren, const Frame *F, const Menu *m, float t, float a)
{
    float u = F->u, x = F->cx - 230 * u, y = F->cy;
    th_emblem(ren, x, y - 96 * u, 58 * u, t, a);
    th_text(ren, TH_FONT_CAPS, th_fit(ren, TH_FONT_CAPS, 24 * u, "CASTLEVANIA", 380 * u, 5 * u), x, y + 10 * u,
            th_fade(TH_GOLD, a), "CASTLEVANIA", 0.5f, 0.5f, 5 * u, th_fade(th_alpha((SDL_Color){0, 0, 0, 255}, 160), a));
    float tp = th_fit(ren, TH_FONT_TITLE, 66 * u, "Rondo of Blood", 420 * u, 1 * u);
    th_text(ren, TH_FONT_TITLE, tp, x, y + 58 * u, th_fade(TH_TEXT, a), "Rondo of Blood", 0.5f, 0.5f, 1 * u,
            th_fade(th_alpha(TH_EMBER, 190), a));
    th_divider(ren, x - 170 * u, x + 170 * u, y + 102 * u, u, a);
    th_text(ren, TH_FONT_UI, 11 * u, x, y + 122 * u, th_fade(TH_TEXT_DIM, a), "RONDO RECOMP", 0.5f, 0.5f, 5 * u, TH_NONE);
    if (m->info[0][0])
        th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, 12.5f * u, m->info[0], 420 * u, 0), x, y + 146 * u,
                th_fade(TH_GOLD, a), m->info[0], 0.5f, 0.5f, 0, th_fade(th_alpha((SDL_Color){0, 0, 0, 255}, 150), a));
    if (m->info[1][0])
        th_text(ren, TH_FONT_UI, 10 * u, x, y + 164 * u, th_fade(TH_TEXT_DIM, a), m->info[1], 0.5f, 0.5f, 0.5f * u,
                th_fade(th_alpha((SDL_Color){0, 0, 0, 255}, 150), a));
    if (m->notice[0]) {   /* a newer version is out */
        float glow = 0.8f + 0.2f * sinf(t * 2.5f);
        th_text(ren, TH_FONT_UI, th_fit(ren, TH_FONT_UI, 13 * u, m->notice, 420 * u, 0), x, y + 190 * u,
                th_fade((SDL_Color){255, 218, 140, (Uint8)(255 * glow)}, a), m->notice, 0.5f, 0.5f, 0,
                th_fade(th_alpha(TH_EMBER, 170), a));
    }
}

void menu_draw(const Menu *m, const Settings *s, SDL_Renderer *ren)
{
    Frame F = frame_for(m, ren);
    Uint64 now = SDL_GetTicks();
    float t = (float)now / 1000.0f, since = (float)(now - m->opened_ms) / 1000.0f;
    float pop = since * 6.0f;
    if (pop > 1) pop = 1;
    if (pop < 0) pop = 0;
    float a = pop * pop;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (m->root == PAGE_START) {
        th_backdrop(ren, F.w, F.h, t);              /* the game is not running behind it */
    } else {
        float fw = (float)F.w, fh = (float)F.h;
        th_rect(ren, 0, 0, fw, fh, (SDL_Color){5, 6, 14, 178});
        th_rect_h(ren, 0, 0, fw * 0.25f, fh, (SDL_Color){0, 0, 0, 120}, (SDL_Color){0, 0, 0, 0});
        th_rect_h(ren, fw * 0.75f, 0, fw, fh, (SDL_Color){0, 0, 0, 0}, (SDL_Color){0, 0, 0, 120});
    }
    if (F.start) {
        float b = since * 2.0f;
        draw_start_title(ren, &F, m, t, b > 1 ? 1 : b);
    }
    /* the frame opens out from its middle */
    float mid = (F.y0 + F.y1) * 0.5f;
    th_frame(ren, F.x0, mid - (mid - F.y0) * pop, F.x1, mid + (F.y1 - mid) * pop, F.rise * pop, F.u, pop);
    draw_header(ren, &F, PAGES[m->page].title, !F.start, a);
    draw_rows(ren, &F, m, s, t, a);
    draw_footer(ren, &F, m, a);
}

int menu_check(Settings *s, SDL_Renderer *ren, char *err, int errlen)
{
    Settings keep = *s;
    int bad = 0;
    err[0] = 0;
    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        Menu m = {.open = true, .page = pg};
        for (int i = 0; i < PAGES[pg].n; i++) {
            const Item *it = &PAGES[pg].items[i];
            if (it->kind != IT_CHOICE && it->kind != IT_TOGGLE) continue;
            size_t at = it->field, size = it->kind == IT_TOGGLE ? sizeof(bool) : sizeof(int);
            m.sel[pg] = i;
            for (int v = 0; v < it->n_choices; v++) {
                Settings before;
                memcpy(&before, s, sizeof before);
                set_value(s, it, v);
                if (get_value(s, it) != v || memcmp(s, &before, at) ||
                    memcmp((const char *)s + at + size, (const char *)&before + at + size, sizeof *s - at - size)) {
                    if (!bad) snprintf(err, (size_t)errlen, "%s / %s, value %d", PAGES[pg].title, it->label, v);
                    bad++;
                }
                menu_draw(&m, s, ren);
            }
        }
    }
    *s = keep;
    return bad;
}
