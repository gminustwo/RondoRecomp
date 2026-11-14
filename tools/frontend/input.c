/* See input.h. */
#include "input.h"

#include <stdio.h>
#include <string.h>

#include "pce/pce.h"

#define STICK 16000

static const bool *key_state;   /* the showcase's keys in place of the keyboard's */

void input_use_keys(const bool *keys) { key_state = keys; }

static const bool *keys_held(void) { return key_state ? key_state : SDL_GetKeyboardState(NULL); }

void input_init(Input *in)
{
    memset(in, 0, sizeof *in);
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);   /* controllers already connected */
    for (int i = 0; i < n && i < 8; i++) in->pads[i] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    in->pad_last = n > 0;
}

bool input_pad_last(const Input *in) { return in->pad_last; }

void input_use_binds(Input *in, const int *pad, const int *key)
{
    in->bind_pad = pad;
    in->bind_key = key;
}

/* a controller binding held on any controller (triggers past a light press) */
static bool pad_held(const Input *in, int b)
{
    if (b == BIND_NONE) return false;
    for (int i = 0; i < 8; i++) {
        SDL_Gamepad *g = in->pads[i];
        if (!g) continue;
        if (b == BIND_LT || b == BIND_RT) {
            if (SDL_GetGamepadAxis(g, b == BIND_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000)
                return true;
        } else if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) {
            return true;
        }
    }
    return false;
}

static bool swallowed(const Input *in, int b) { return b >= 0 && b < BIND_CONTROLS && in->swallowed[b]; }

/* some action's combination starts with button b / has b second and its first held */
static bool starts_combo(const Input *in, int b)
{
    for (int a = 0; a < ACT_COUNT; a++)
        if (BIND_HOLD(in->bind_pad[a]) == b) return true;
    return false;
}

static bool combo_held_for(const Input *in, int b)
{
    for (int a = 0; a < ACT_COUNT; a++) {
        int c = in->bind_pad[a];
        if (BIND_HOLD(c) != BIND_NONE && BIND_PRESS(c) == b && pad_held(in, BIND_HOLD(c))) return true;
    }
    return false;
}

bool input_held(const Input *in, int act)
{
    if (!in->bind_pad || act < 0 || act >= ACT_COUNT) return false;
    int k = in->bind_key[act];
    if (k > 0 && k < SDL_SCANCODE_COUNT && keys_held()[k]) return true;
    int b = in->bind_pad[act];
    if (BIND_HOLD(b) != BIND_NONE)                 /* the second pressed while the first was held */
        return pad_held(in, BIND_HOLD(b)) && pad_held(in, BIND_PRESS(b)) && swallowed(in, BIND_PRESS(b));
    return pad_held(in, b) && !swallowed(in, b);
}

bool input_control_of(const Input *in, const SDL_Event *ev, bool *is_pad, int *value)
{
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        *is_pad = false;
        *value = ev->key.scancode;
        return true;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        *is_pad = true;
        *value = ev->gbutton.button;
        return true;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        int t = ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (!in->trig_pressed[t]) return false;
        *is_pad = true;
        *value = t ? BIND_RT : BIND_LT;
        return true;
    }
    return false;
}

bool input_pad_event(const Input *in, const SDL_Event *ev, int *value, bool *down)
{
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        *value = ev->gbutton.button;
        *down = ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        return true;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        int t = ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (!in->trig_pressed[t] && !in->trig_released[t]) return false;
        *value = t ? BIND_RT : BIND_LT;
        *down = in->trig_pressed[t];
        return true;
    }
    return false;
}

bool input_pressed(const Input *in, const SDL_Event *ev, int act)
{
    if (!in->bind_pad || act < 0 || act >= ACT_COUNT) return false;
    if (ev->type == SDL_EVENT_KEY_DOWN)
        return !ev->key.repeat && in->bind_key[act] != 0 && (int)ev->key.scancode == in->bind_key[act];
    int b = in->bind_pad[act], v;
    bool down;
    if (b == BIND_NONE || !input_pad_event(in, ev, &v, &down)) return false;
    if (BIND_HOLD(b) != BIND_NONE)                 /* a combination: its second, the first held */
        return down && v == BIND_PRESS(b) && pad_held(in, BIND_HOLD(b));
    if (v != b) return false;
    if (starts_combo(in, b)) return !down && in->released_armed;   /* let go without one */
    return down && !swallowed(in, b);
}

void input_close(Input *in)
{
    for (int i = 0; i < 8; i++)
        if (in->pads[i]) SDL_CloseGamepad(in->pads[i]);
    memset(in, 0, sizeof *in);
}

/* a button or trigger pressed or let go: what it means for combinations */
static void combo_event(Input *in, int b, bool down)
{
    if (b < 0 || b >= BIND_CONTROLS) return;
    if (!down) {
        in->released_armed = in->armed[b];
        in->armed[b] = in->swallowed[b] = false;
        return;
    }
    in->swallowed[b] = in->bind_pad && combo_held_for(in, b);
    in->armed[b] = !in->swallowed[b];
    if (in->swallowed[b])                          /* the first buttons were used: no press of their own */
        for (int a = 0; a < ACT_COUNT; a++) {
            int c = in->bind_pad[a], h = BIND_HOLD(c);
            if (h >= 0 && h < BIND_CONTROLS && BIND_PRESS(c) == b && pad_held(in, h)) in->armed[h] = false;
        }
}

void input_device_event(Input *in, const SDL_Event *ev)
{
    /* only for the event that pressed or let go of it */
    in->trig_pressed[0] = in->trig_pressed[1] = in->trig_released[0] = in->trig_released[1] = false;
    in->released_armed = false;
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED) {
        in->pad_last = true;
        if (SDL_GetGamepadFromID(ev->gdevice.which)) return;
        for (int i = 0; i < 8; i++)
            if (!in->pads[i]) { in->pads[i] = SDL_OpenGamepad(ev->gdevice.which); break; }
    } else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED) {
        bool any = false;
        for (int i = 0; i < 8; i++) {
            if (in->pads[i] && SDL_GetGamepadID(in->pads[i]) == ev->gdevice.which) {
                SDL_CloseGamepad(in->pads[i]);
                in->pads[i] = NULL;
            }
            any |= in->pads[i] != NULL;
        }
        if (!any) in->pad_last = false;
    } else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        bool down = ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down) in->pad_last = true;
        combo_event(in, ev->gbutton.button, down);
    } else if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (ev->gaxis.value > STICK || ev->gaxis.value < -STICK) in->pad_last = true;
        if (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            /* a trigger as a button: pressed past half way, let go below a quarter */
            int t = ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            bool down = in->trig_held[t] ? ev->gaxis.value > 8000 : ev->gaxis.value > STICK;
            in->trig_pressed[t] = down && !in->trig_held[t];
            in->trig_released[t] = !down && in->trig_held[t];
            in->trig_held[t] = down;
            if (in->trig_pressed[t] || in->trig_released[t]) combo_event(in, t ? BIND_RT : BIND_LT, down);
        }
    } else if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        in->pad_last = false;
    }
}

void input_cancel_macros(Input *in)
{
    in->q_len = in->q_pos = in->q_left = 0;
    memset(in->armed, 0, sizeof in->armed);
}

static void push(Input *in, uint8_t set, uint8_t clear, int frames)
{
    if (in->q_len < 6) in->q[in->q_len++] = (MacroStep){set, clear, frames};
}

uint8_t input_game(Input *in)
{
    const bool *k = keys_held();
    uint8_t m = 0;
    bool sub = false, flip = false;
    if (k[SDL_SCANCODE_UP]) m |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) m |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) m |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) m |= PAD_RIGHT;
    if (input_held(in, ACT_UP)) m |= PAD_UP;          /* the player's own keys to move (WASD) */
    if (input_held(in, ACT_DOWN)) m |= PAD_DOWN;
    if (input_held(in, ACT_LEFT)) m |= PAD_LEFT;
    if (input_held(in, ACT_RIGHT)) m |= PAD_RIGHT;
    /* the game's buttons and the two macros, from the controls (keys and controller alike) */
    if (input_held(in, ACT_JUMP)) m |= PAD_I;
    if (input_held(in, ACT_ATTACK)) m |= PAD_II;
    if (input_held(in, ACT_CRASH)) m |= PAD_SELECT;
    if (input_held(in, ACT_PAUSE)) m |= PAD_RUN;
    sub = input_held(in, ACT_SUB);
    flip = input_held(in, ACT_FLIP);
    for (int i = 0; i < 8; i++) {   /* moving: the d-pad and the left stick */
        SDL_Gamepad *g = in->pads[i];
        if (!g) continue;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= PAD_UP;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= PAD_DOWN;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= PAD_LEFT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= PAD_RIGHT;
        int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        int y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -STICK) m |= PAD_LEFT;
        if (x > STICK) m |= PAD_RIGHT;
        if (y < -STICK) m |= PAD_UP;
        if (y > STICK) m |= PAD_DOWN;
    }

    /* start a macro on a new press (not while another one runs) */
    if (!in->q_len) {
        if (sub && !in->prev_sub) {
            /* Up + II; II has to be seen as a new press, so let go of it first if held */
            if (in->last_out & PAD_II) push(in, 0, PAD_II, 3);
            push(in, PAD_UP | PAD_II, PAD_DOWN, 4);
        } else if (flip && !in->prev_flip) {
            /* I, let go, I again while still rising */
            if (in->last_out & PAD_I) push(in, 0, PAD_I, 3);
            push(in, PAD_I, 0, 4);
            push(in, 0, PAD_I, 3);
            push(in, PAD_I, 0, 4);
        }
        if (in->q_len) { in->q_pos = 0; in->q_left = in->q[0].frames; }
    }
    in->prev_sub = sub;
    in->prev_flip = flip;
    if (in->q_len) {
        const MacroStep *st = &in->q[in->q_pos];
        m = (uint8_t)((m & ~st->clear) | st->set);
        if (--in->q_left <= 0) {
            if (++in->q_pos < in->q_len) in->q_left = in->q[in->q_pos].frames;
            else in->q_len = in->q_pos = in->q_left = 0;   /* done */
        }
    }

    /* opposite directions cannot be pressed together on a real pad */
    if ((m & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT)) m &= (uint8_t)~(PAD_LEFT | PAD_RIGHT);
    if ((m & (PAD_UP | PAD_DOWN)) == (PAD_UP | PAD_DOWN)) m &= (uint8_t)~(PAD_UP | PAD_DOWN);
    in->last_out = m;
    return m;
}

bool input_run_held(const Input *in) { return input_held(in, ACT_RUN); }

bool input_attack_held(const Input *in) { return input_held(in, ACT_ATTACK); }

bool input_rewind_held(const Input *in) { return input_held(in, ACT_REWIND); }

Nav input_nav_event(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        switch (ev->key.scancode) {
        case SDL_SCANCODE_UP: return NAV_UP;
        case SDL_SCANCODE_DOWN: return NAV_DOWN;
        case SDL_SCANCODE_LEFT: return NAV_LEFT;
        case SDL_SCANCODE_RIGHT: return NAV_RIGHT;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_X: case SDL_SCANCODE_SPACE: return ev->key.repeat ? NAV_NONE : NAV_OK;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_Z: case SDL_SCANCODE_BACKSPACE: return ev->key.repeat ? NAV_NONE : NAV_BACK;
        case SDL_SCANCODE_F1: return ev->key.repeat ? NAV_NONE : NAV_MENU;
        case SDL_SCANCODE_DELETE: return ev->key.repeat ? NAV_NONE : NAV_CLEAR_KEY;
        default: return NAV_NONE;
        }
    }
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return NAV_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return NAV_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return NAV_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return NAV_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH: return NAV_OK;
        case SDL_GAMEPAD_BUTTON_EAST: return NAV_BACK;
        case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_START: return NAV_MENU;
        case SDL_GAMEPAD_BUTTON_WEST: return NAV_CLEAR_PAD;
        default: return NAV_NONE;
        }
    }
    return NAV_NONE;
}

Nav input_nav_stick(Input *in)
{
    int dir = 0;
    for (int i = 0; i < 8; i++) {
        SDL_Gamepad *g = in->pads[i];
        if (!g) continue;
        int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (y < -24000) dir = NAV_UP;
        else if (y > 24000) dir = NAV_DOWN;
        else if (x < -24000) dir = NAV_LEFT;
        else if (x > 24000) dir = NAV_RIGHT;
    }
    Uint64 now = SDL_GetTicks();
    if (dir == 0) { in->stick_dir = 0; return NAV_NONE; }
    if (dir != in->stick_dir) { in->stick_dir = dir; in->stick_next = now + 350; return (Nav)dir; }
    if (now >= in->stick_next) { in->stick_next = now + 110; return (Nav)dir; }
    return NAV_NONE;
}

/* input_check: a virtual controller (SDL's), its buttons set one by one and the events they
 * make read as play reads them */
typedef struct Check {
    Input in;
    SDL_Joystick *joy;
    SDL_JoystickID id;
} Check;

/* press or let go of a button / trigger; the actions among Save state, Load state and Menu
 * its events pressed (bits by ACT_) */
static unsigned check_step(Check *c, int ctl, bool down)
{
    if (ctl == BIND_LT || ctl == BIND_RT)
        SDL_SetJoystickVirtualAxis(c->joy, ctl == BIND_RT ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                                   down ? 32767 : -32768);
    else if (ctl != BIND_NONE)
        SDL_SetJoystickVirtualButton(c->joy, ctl, down);
    SDL_UpdateJoysticks();
    unsigned pressed = 0;
    SDL_Event evs[32];
    int n;
    while ((n = SDL_PeepEvents(evs, 32, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_JOYSTICK_AXIS_MOTION + 0xff)) > 0)
        for (int i = 0; i < n; i++) {
            const SDL_Event *ev = &evs[i];
            bool ours = (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP)
                            ? ev->gbutton.which == c->id
                            : ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && ev->gaxis.which == c->id;
            if (!ours) continue;
            input_device_event(&c->in, ev);
            static const int acts[] = {ACT_SAVE, ACT_LOAD, ACT_MENU};
            for (int a = 0; a < 3; a++)
                if (input_pressed(&c->in, ev, acts[a])) pressed |= 1u << acts[a];
        }
    return pressed;
}

int input_check(char *err, int errlen)
{
    int bad = 0;
    err[0] = 0;
#define FAIL(...) do { if (!bad++) snprintf(err, (size_t)errlen, __VA_ARGS__); } while (0)
    const char *was = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    char *hint = was ? SDL_strdup(was) : NULL;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* the test window may be behind */
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "RondoRecomp test controller";
    Check c;
    memset(&c, 0, sizeof c);
    c.id = SDL_AttachVirtualJoystick(&d);
    if (c.id) c.in.pads[0] = SDL_OpenGamepad(c.id);
    if (c.in.pads[0]) c.joy = SDL_GetGamepadJoystick(c.in.pads[0]);
    if (!c.joy) {
        FAIL("no virtual controller: %s", SDL_GetError());
    } else {
        const int BACK = SDL_GAMEPAD_BUTTON_BACK, RB = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                  LB = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
        const unsigned SAVE = 1u << ACT_SAVE, LOAD = 1u << ACT_LOAD, MENU = 1u << ACT_MENU;
        int pad[ACT_COUNT], key[ACT_COUNT];
        bind_defaults(pad, key);
        pad[ACT_SAVE] = BIND_COMBO(BACK, RB);
        pad[ACT_LOAD] = BIND_COMBO(BACK, LB);
        pad[ACT_REWIND] = BIND_COMBO(BACK, BIND_LT);
        input_use_binds(&c.in, pad, key);
        check_step(&c, BIND_LT, false);            /* triggers at rest */
        check_step(&c, BIND_RT, false);
        unsigned p;
        /* Back + RB: save state; RB is not a backflip then, and Back opens no menu */
        if ((p = check_step(&c, BACK, true))) FAIL("Back pressed: %#x, not nothing yet", p);
        if ((p = check_step(&c, RB, true)) != SAVE) FAIL("Back + RB: %#x, not save state", p);
        if (input_held(&c.in, ACT_FLIP)) FAIL("Back + RB is a backflip too");
        if ((p = check_step(&c, RB, false))) FAIL("RB let go: %#x", p);
        if ((p = check_step(&c, BACK, false))) FAIL("Back let go after Back + RB: %#x, not nothing", p);
        /* Back alone: the menu, when let go */
        check_step(&c, BACK, true);
        if ((p = check_step(&c, BACK, false)) != MENU) FAIL("Back alone: %#x, not the menu", p);
        /* RB alone: a backflip */
        if ((p = check_step(&c, RB, true))) FAIL("RB alone: %#x", p);
        if (!input_held(&c.in, ACT_FLIP)) FAIL("RB alone is no backflip");
        check_step(&c, RB, false);
        /* Back + LB: load state, without running */
        check_step(&c, BACK, true);
        if ((p = check_step(&c, LB, true)) != LOAD) FAIL("Back + LB: %#x, not load state", p);
        if (input_run_held(&c.in)) FAIL("Back + LB runs");
        check_step(&c, LB, false);
        check_step(&c, BACK, false);
        /* LB first, then Back: running, and no load state (the order counts) */
        check_step(&c, LB, true);
        if ((p = check_step(&c, BACK, true))) FAIL("LB then Back: %#x, not nothing", p);
        if (!input_run_held(&c.in)) FAIL("LB then Back does not run");
        if ((p = check_step(&c, BACK, false)) != MENU) FAIL("LB held, Back alone: %#x, not the menu", p);
        check_step(&c, LB, false);
        /* a held combination with a trigger: Back + LT rewinds while both are held */
        check_step(&c, BIND_LT, true);
        if (SDL_GetGamepadAxis(c.in.pads[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER) < 16000)
            FAIL("the virtual LT reads %d", SDL_GetGamepadAxis(c.in.pads[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        if (input_rewind_held(&c.in)) FAIL("LT alone rewinds (Rewind is Back + LT)");
        check_step(&c, BIND_LT, false);
        check_step(&c, BACK, true);
        check_step(&c, BIND_LT, true);
        if (!input_rewind_held(&c.in)) FAIL("Back + LT does not rewind");
        check_step(&c, BIND_LT, false);
        if (input_rewind_held(&c.in)) FAIL("still rewinding after LT let go");
        if ((p = check_step(&c, BACK, false))) FAIL("Back let go after Back + LT: %#x", p);
        /* the menu opening or closing forgets a Back that was down */
        check_step(&c, BACK, true);
        input_cancel_macros(&c.in);
        if ((p = check_step(&c, BACK, false))) FAIL("Back let go after the menu changed: %#x", p);
        /* with no combinations, Back is the menu as soon as it is pressed */
        bind_defaults(pad, key);
        if ((p = check_step(&c, BACK, true)) != MENU) FAIL("Back with no combinations: %#x, not the menu at once", p);
        check_step(&c, BACK, false);
    }
    /* keys of the player's own to move (WASD, with A and S taken from backflip and run), beside
     * the arrows; opposite ones cancel out */
    {
        static bool keys[SDL_SCANCODE_COUNT];
        const bool *was_keys = key_state;
        int pad[ACT_COUNT], key[ACT_COUNT];
        bind_defaults(pad, key);
        key[ACT_FLIP] = key[ACT_RUN] = 0;
        key[ACT_UP] = SDL_SCANCODE_W;
        key[ACT_LEFT] = SDL_SCANCODE_A;
        key[ACT_DOWN] = SDL_SCANCODE_S;
        key[ACT_RIGHT] = SDL_SCANCODE_D;
        Input k;
        memset(&k, 0, sizeof k);
        input_use_binds(&k, pad, key);
        key_state = keys;
        memset(keys, 0, sizeof keys);
        keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_D] = true;
        unsigned m;
        if ((m = input_game(&k)) != (PAD_UP | PAD_RIGHT)) FAIL("W + D: %#x, not up and right", m);
        keys[SDL_SCANCODE_W] = false;
        keys[SDL_SCANCODE_A] = true;
        if ((m = input_game(&k)) != 0) FAIL("A + D: %#x, not nothing", m);
        memset(keys, 0, sizeof keys);
        keys[SDL_SCANCODE_LEFT] = keys[SDL_SCANCODE_S] = true;
        if ((m = input_game(&k)) != (PAD_LEFT | PAD_DOWN)) FAIL("Left arrow + S: %#x, not left and down", m);
        if (input_run_held(&k)) FAIL("S still runs");
        key_state = was_keys;
    }
    if (c.in.pads[0]) SDL_CloseGamepad(c.in.pads[0]);
    if (c.id) SDL_DetachVirtualJoystick(c.id);
    c.joy = NULL;
    check_step(&c, BIND_NONE, false);              /* its own events (removed) */
    if (hint) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, hint);
    else SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    SDL_free(hint);
#undef FAIL
    return bad;
}
