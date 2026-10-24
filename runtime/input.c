/* The window's input (M2; docs/PORT.md "Input"; rebindable: docs/LAUNCHER.md "Input bindings"): the
 * keyboard and every SDL gamepad, ORed into pad 1 (a digital pad: SELECT, START, the D-pad, L1/L2/R1/R2 and the four
 * face buttons; psyq_pad_set, psyq.h's bit order), once per vsync from pump.c; and the hotkey actions (pause,
 * fullscreen, the mods' bindings). With `input.player2` ("first" or "second": that gamepad by connection order, SDL's
 * instance ids growing with each connection) one gamepad is player 2 instead: it drives pad 2 (port 1, connected
 * while that gamepad is there, with the same gamepad map) and never a hotkey; player 1 keeps the keyboard and every
 * other gamepad. "off" (the default): port 1 stays empty, as on a console with one pad. Only in a build configured
 * with -DPSXSTACK_SDL=ON (PSXSTACK_SDL); the headless build has no input but the script's (its actions never fire; the
 * settings' names are checked for their shape only).
 *
 * Inputs: every keyboard key (an SDL scancode: the key's place, whatever the layout) and the gamepad inputs of
 * input_pad_names (SDL's positional buttons, then the triggers and the stick directions past half way, all gamepads
 * ORed). The pad map is a list of (input, PS1 button) entries; by default:
 *   arrows             D-pad              X  cross      C  circle     Z  square     S  triangle
 *   Enter, keypad Enter START             Backspace, right Shift  SELECT
 *   Q  L1      E  R1      1  L2      3  R2
 *   gamepads: south cross, east circle, west square, north triangle, back SELECT, start START, the shoulders L1/R1,
 *   the triggers L2/R2, the D-pad and the left stick the D-pad
 * The settings (`input.keyboard`, `input.gamepad`: a PS1 button -> a name or a list of names) replace a button's
 * keyboard or gamepad entries. Hotkeys (`input.hotkeys`, and the mods' binding options through port_input_action):
 * a binding is a list of triggers, a trigger one input or a chord of up to INPUT_CHORD inputs, all held
 * (docs/LAUNCHER.md "Input bindings"). Defaults: pause P, fullscreen F11, save_state F5, load_state F9, menu Escape or
 * Back+Start on a gamepad (the in-game menu, menu.h: the menu in a pause).
 *
 * Hotkeys never reach the pad: a trigger that completes (all its inputs held) latches until every one of its inputs is
 * released, and a latched trigger's inputs are masked out of the pad (so a single-key hotkey is never seen by the game,
 * and a chord's buttons stop reaching it once it is complete; its first button reaches it until then).
 *
 * With `--script` the script owns the pad: the keyboard and the gamepads are read but never reach psyq_pad_set, so a
 * replay in a window runs exactly as it does headless (the hotkeys still act). Without one, every change of the pad
 * goes to the frame log's `I` lines and the record's `inputs` (port_framelog_input), as the script's do. Closing the
 * window ends the run: port_exit(0, "window closed").
 *
 * `--input-test` (a self-test of the mapping, for CI and after a change here): from frame 2 on, it injects the key
 * events of every keyboard input of the pad map with SDL_PushEvent (a press in one frame, the release in the next),
 * then attaches a virtual SDL gamepad (player 1's: with `input.player2` "first", a second one is attached before it)
 * and does the same with each gamepad input of the map, then one chord of two keys and a gamepad input, then every
 * hotkey trigger; after each step's poll the buttons sent to psyq_pad_set must be the map's bits for the inputs held
 * (none for a hotkey's: masked; with a script: nothing sent at all) and a hotkey step must have fired its action (the
 * actions do nothing during the test). Without --config it tests the defaults; with one, the settings' map and
 * hotkeys. Then the players: a second virtual gamepad, a gamepad input pressed on each pad and on both, every hotkey
 * trigger made of gamepad inputs on the second (player 2: it reaches pad 2 and fires nothing), then the second
 * detached; after each poll both ports must be what `input.player2` says (with "off" everything is pad 1's and port 1
 * stays empty). Then (without a script) the pause: its key pressed, released and pressed again
 * while paused; the pause must poll without a vsync and give back the game; the quick slot's round trip; then the
 * menu's (a build with PSXSTACK_MENU, menu.h): its hotkey pressed at a vsync, the menu must be shown in the pause's
 * polls (the second present with it up is the <PREFIX>_PORT_MENU_SHOT picture), the hotkey again gives back the game.
 * It ends the run with status 0 when every check passed, 6 when one failed; with
 * a script it only logs the result and the script runs on, so its log can be compared with a run without the test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "port_harness.h"
#include "menu.h"
#include "port_runtime.h"
#include "psyq.h"
#include "savestate.h"
#include "settings.h"

#ifdef PSXSTACK_SDL
#include <SDL3/SDL.h>
#endif

/* The PS1 pad's bits (psyq.h psyq_pad_set; active high), by the settings' names. */
static const struct {
    const char *name;
    u16 bits;
} input_buttons_named[] = {
    { "select", 1u << 0 },  { "start", 1u << 3 },     { "up", 1u << 4 },     { "right", 1u << 5 },
    { "down", 1u << 6 },    { "left", 1u << 7 },      { "l2", 1u << 8 },     { "r2", 1u << 9 },
    { "l1", 1u << 10 },     { "r1", 1u << 11 },       { "triangle", 1u << 12 }, { "circle", 1u << 13 },
    { "cross", 1u << 14 },  { "square", 1u << 15 },
};
#define INPUT_BUTTONS ((int)(sizeof(input_buttons_named) / sizeof(input_buttons_named[0])))
#define PAD_SELECT (1u << 0)
#define PAD_START (1u << 3)
#define PAD_UP (1u << 4)
#define PAD_RIGHT (1u << 5)
#define PAD_DOWN (1u << 6)
#define PAD_LEFT (1u << 7)
#define PAD_L2 (1u << 8)
#define PAD_R2 (1u << 9)
#define PAD_L1 (1u << 10)
#define PAD_R1 (1u << 11)
#define PAD_TRIANGLE (1u << 12)
#define PAD_CIRCLE (1u << 13)
#define PAD_CROSS (1u << 14)
#define PAD_SQUARE (1u << 15)

/* The gamepad inputs: the buttons in SDL_GamepadButton order (index = the SDL button), then the axes past half way. */
static const char *const input_pad_names[] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
    "touchpad",
    "lefttrigger", "righttrigger", "leftx-", "leftx+", "lefty-", "lefty+", "rightx-", "rightx+", "righty-", "righty+",
};
#define INPUT_PAD_BUTTONS 21
#define INPUT_PAD_INPUTS ((int)(sizeof(input_pad_names) / sizeof(input_pad_names[0])))
#define INPUT_KEYS 512 /* SDL_SCANCODE_COUNT */
#define INPUT_PAD(n) (INPUT_KEYS + (n)) /* the input id of gamepad input n */
#define INPUT_IDS (INPUT_KEYS + INPUT_PAD_INPUTS)
enum { PAD_LT = INPUT_PAD_BUTTONS, PAD_RT, PAD_LX_NEG, PAD_LX_POS, PAD_LY_NEG, PAD_LY_POS };

#define INPUT_MAX_MAP 128
#define INPUT_MAX_ACTIONS 32
#define INPUT_MAX_TRIGGERS 8
#define INPUT_CHORD 4

static struct {
    int id;
    u16 bits;
} input_map[INPUT_MAX_MAP];
static int input_map_count;

typedef struct InputTrigger {
    int n;
    int ids[INPUT_CHORD];
    int complete; /* every input held at the last poll */
    int latched;  /* completed, and not every input released since: its inputs are masked out of the pad */
} InputTrigger;

static struct {
    const char *name;
    InputTrigger triggers[INPUT_MAX_TRIGGERS];
    int count;
    int held, pressed; /* at the last poll: a trigger complete; a trigger completed */
} input_actions[INPUT_MAX_ACTIONS];
static int input_action_count;
int port_action_pause = -1, port_action_fullscreen = -1, port_action_save_state = -1, port_action_load_state = -1;
int port_action_menu = -1;
static int input_test;      /* --input-test running */
static int input_test_mute; /* the actions do nothing for their consumers (the test's steps) */
/* input.player<n> (n >= 2; a multitap would add players): the gamepad that player has, by connection order (1 the
 * first, 2 the second; 0 none: that player's port stays empty). Player 1 has the keyboard and every other gamepad. */
static int input_player_gamepad[PORT_PADS];
static const char *const input_player_names[] = { "off", "first", "second" };
#define INPUT_PLAYER_NAMES ((int)(sizeof(input_player_names) / sizeof(input_player_names[0])))

/* ---- The names (the settings' grammar; docs/LAUNCHER.md "Input bindings") */

static int input_button_bits(const char *name) {
    int i;
    for (i = 0; i < INPUT_BUTTONS; i++) {
        if (strcmp(input_buttons_named[i].name, name) == 0) {
            return input_buttons_named[i].bits;
        }
    }
    return -1;
}

static int input_pad_index(const char *name) {
    int i;
    for (i = 0; i < INPUT_PAD_INPUTS; i++) {
        if (strcmp(input_pad_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* A key name (SDL3's scancode names) -> its input id; -1 when unknown. Without SDL: 0 for any non-empty name (the
 * headless build checks the shape only: it has no keyboard). */
static int input_key_id(const char *name) {
#ifdef PSXSTACK_SDL
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc != SDL_SCANCODE_UNKNOWN && (int)sc < INPUT_KEYS ? (int)sc : -1;
#else
    return name[0] != '\0' ? 0 : -1;
#endif
}

/* An input of a binding: a key name or "pad:" + a gamepad input name. */
static int input_binding_input(const char *name, const char *where) {
    int id;
    if (strncmp(name, "pad:", 4) == 0) {
        id = input_pad_index(name + 4);
        if (id < 0) {
            port_settings_fail(where, "\"%s\": not a gamepad input (docs/LAUNCHER.md \"Input bindings\")", name);
        }
        return INPUT_PAD(id);
    }
    id = input_key_id(name);
    if (id < 0) {
        port_settings_fail(where, "\"%s\": not a key name (SDL3's scancode names, e.g. \"Tab\", \"F2\")", name);
    }
    return id;
}

static void input_trigger_add(int action, const PortJson *t, const char *where) {
    InputTrigger *tr;
    size_t i;
    if (t->type == PORT_JSON_STRING && t->string[0] == '\0') {
        return; /* unbound */
    }
    if (input_actions[action].count == INPUT_MAX_TRIGGERS) {
        port_settings_fail(where, "at most %d triggers", INPUT_MAX_TRIGGERS);
    }
    tr = &input_actions[action].triggers[input_actions[action].count];
    memset(tr, 0, sizeof(*tr));
    if (t->type == PORT_JSON_STRING) {
        tr->ids[tr->n++] = input_binding_input(t->string, where);
    } else if (t->type == PORT_JSON_ARRAY && t->count >= 1 && t->count <= INPUT_CHORD) {
        for (i = 0; i < t->count; i++) {
            if (t->items[i].type != PORT_JSON_STRING) {
                port_settings_fail(where, "a chord is a list of input names");
            }
            tr->ids[tr->n++] = input_binding_input(t->items[i].string, where);
        }
    } else {
        port_settings_fail(where, "a trigger is an input name or a chord (a list of 1 to %d names)", INPUT_CHORD);
    }
    input_actions[action].count++;
}

int port_input_action(const char *name, const PortJson *binding, const char *where, const char *default_json) {
    PortJson *dflt = NULL;
    int a;
    size_t i;
    if (input_action_count == INPUT_MAX_ACTIONS) {
        port_fatal("input: more than %d actions", INPUT_MAX_ACTIONS);
    }
    a = input_action_count++;
    memset(&input_actions[a], 0, sizeof(input_actions[a]));
    input_actions[a].name = name;
    if (binding == NULL) {
        char err[128];
        dflt = port_json_parse(default_json, strlen(default_json), err, sizeof(err));
        if (dflt == NULL) {
            port_fatal("input: %s: the default binding %s: %s", name, default_json, err);
        }
        binding = dflt;
    }
    if (binding->type == PORT_JSON_STRING) {
        input_trigger_add(a, binding, where);
    } else if (binding->type == PORT_JSON_ARRAY) {
        for (i = 0; i < binding->count; i++) {
            input_trigger_add(a, &binding->items[i], where);
        }
    } else {
        port_settings_fail(where, "a binding is a string or a list, not %s", port_json_type_name(binding->type));
    }
    if (dflt != NULL) {
        port_json_free(dflt);
    }
    return a;
}

void port_input_check_binding(const PortJson *binding, const char *where) {
    int saved = input_action_count;
    port_input_action(where, binding, where, "\"\"");
    input_action_count = saved;
}

int port_input_pressed(int action) {
    return action >= 0 && action < input_action_count && !input_test_mute && input_actions[action].pressed;
}

int port_input_held(int action) {
    return action >= 0 && action < input_action_count && !input_test_mute && input_actions[action].held;
}

static void input_map_add(int id, u16 bits) {
    if (input_map_count == INPUT_MAX_MAP) {
        port_fatal("input: more than %d pad map entries", INPUT_MAX_MAP);
    }
    input_map[input_map_count].id = id;
    input_map[input_map_count].bits = bits;
    input_map_count++;
}

static void input_map_defaults(void) {
    static const struct {
        const char *name;
        u16 bits;
    } keys[] = {
        { "Up", PAD_UP },         { "Right", PAD_RIGHT },     { "Down", PAD_DOWN },      { "Left", PAD_LEFT },
        { "X", PAD_CROSS },       { "C", PAD_CIRCLE },        { "Z", PAD_SQUARE },       { "S", PAD_TRIANGLE },
        { "Return", PAD_START },  { "Keypad Enter", PAD_START }, { "Backspace", PAD_SELECT },
        { "Right Shift", PAD_SELECT }, { "Q", PAD_L1 },       { "E", PAD_R1 },           { "1", PAD_L2 },
        { "3", PAD_R2 },
    };
    static const struct {
        int pad;
        u16 bits;
    } pads[] = {
        { 0, PAD_CROSS },  { 1, PAD_CIRCLE }, { 2, PAD_SQUARE }, { 3, PAD_TRIANGLE }, { 4, PAD_SELECT },
        { 6, PAD_START },  { 9, PAD_L1 },     { 10, PAD_R1 },    { 11, PAD_UP },      { 12, PAD_DOWN },
        { 13, PAD_LEFT },  { 14, PAD_RIGHT }, { PAD_LT, PAD_L2 }, { PAD_RT, PAD_R2 },  { PAD_LX_NEG, PAD_LEFT },
        { PAD_LX_POS, PAD_RIGHT }, { PAD_LY_NEG, PAD_UP },       { PAD_LY_POS, PAD_DOWN },
    };
    size_t i;
    input_map_count = 0;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int id = input_key_id(keys[i].name);
        if (id < 0) {
            port_fatal("input: the default key %s is unknown to SDL", keys[i].name);
        }
        input_map_add(id, keys[i].bits);
    }
    for (i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
        input_map_add(INPUT_PAD(pads[i].pad), pads[i].bits);
    }
}

/* input.keyboard or input.gamepad: each button named replaces that button's entries of that kind. */
static void input_map_section(const PortJson *obj, int pad) {
    const char *where = pad ? "input.gamepad" : "input.keyboard";
    size_t i, j;
    if (obj == NULL) {
        return;
    }
    if (obj->type != PORT_JSON_OBJECT) {
        port_settings_fail(where, "an object (a PS1 button -> a name or a list of names)");
    }
    for (i = 0; i < obj->count; i++) {
        int bits = input_button_bits(obj->keys[i]);
        const PortJson *v = &obj->items[i];
        char key[64];
        int k, n;
        snprintf(key, sizeof(key), "%s.%s", where, obj->keys[i]);
        if (bits < 0) {
            port_log("settings: %s: unknown PS1 button, ignored", key);
            continue;
        }
        for (k = n = 0; k < input_map_count; k++) {
            if (input_map[k].bits == bits && (input_map[k].id >= INPUT_KEYS) == pad) {
                continue; /* replaced */
            }
            input_map[n++] = input_map[k];
        }
        input_map_count = n;
        for (j = 0; j < (v->type == PORT_JSON_ARRAY ? v->count : 1); j++) {
            const PortJson *name = v->type == PORT_JSON_ARRAY ? &v->items[j] : v;
            int id;
            if (name->type != PORT_JSON_STRING) {
                port_settings_fail(key, "a %s name or a list of them", pad ? "gamepad input" : "key");
            }
            if (name->string[0] == '\0') {
                continue;
            }
            if (pad) {
                id = input_pad_index(name->string);
                if (id < 0) {
                    port_settings_fail(key, "\"%s\": not a gamepad input (docs/LAUNCHER.md \"Input bindings\")",
                                       name->string);
                }
                id = INPUT_PAD(id);
            } else {
                id = input_key_id(name->string);
                if (id < 0) {
                    port_settings_fail(key, "\"%s\": not a key name (SDL3's scancode names)", name->string);
                }
            }
            input_map_add(id, (u16)bits);
        }
    }
}

/* input.player2: "off", "first" or "second". */
static void input_players_section(const PortJson *input) {
    int p, k;
    for (p = 1; p < PORT_PADS; p++) {
        char key[16], where[32];
        const PortJson *v;
        snprintf(key, sizeof(key), "player%d", p + 1);
        snprintf(where, sizeof(where), "input.%s", key);
        v = port_json_get(input, key);
        if (v == NULL) {
            continue;
        }
        for (k = 0; k < INPUT_PLAYER_NAMES; k++) {
            if (v->type == PORT_JSON_STRING && strcmp(v->string, input_player_names[k]) == 0) {
                break;
            }
        }
        if (k == INPUT_PLAYER_NAMES) {
            port_settings_fail(where, "\"off\", \"first\" or \"second\" (the gamepad by connection order)");
        }
        input_player_gamepad[p] = k;
    }
}

void port_input_settings(const PortJson *input) {
    static const char *const known[] = { "keyboard", "gamepad", "hotkeys", "player2", NULL };
    const PortJson *hotkeys = NULL;
    size_t i;
    input_map_defaults();
    input_action_count = 0;
    memset(input_player_gamepad, 0, sizeof(input_player_gamepad));
    if (input != NULL) {
        for (i = 0; i < input->count; i++) {
            const char *const *k;
            for (k = known; *k != NULL && strcmp(*k, input->keys[i]) != 0; k++) {
            }
            if (*k == NULL) {
                port_log("settings: input.%s: unknown key, ignored", input->keys[i]);
            }
        }
        input_map_section(port_json_get(input, "keyboard"), 0);
        input_map_section(port_json_get(input, "gamepad"), 1);
        input_players_section(input);
        hotkeys = port_json_get(input, "hotkeys");
        if (hotkeys != NULL && hotkeys->type != PORT_JSON_OBJECT) {
            port_settings_fail("input.hotkeys", "an object (an action -> a binding)");
        }
        for (i = 0; hotkeys != NULL && i < hotkeys->count; i++) {
            static const char *const actions[] = { "pause", "fullscreen", "save_state", "load_state", "menu", NULL };
            const char *const *a;
            for (a = actions; *a != NULL && strcmp(*a, hotkeys->keys[i]) != 0; a++) {
            }
            if (*a == NULL) {
                port_log("settings: input.hotkeys.%s: unknown action, ignored", hotkeys->keys[i]);
            }
        }
    }
    port_action_pause = port_input_action("pause", port_json_get(hotkeys, "pause"), "input.hotkeys.pause", "\"P\"");
    port_action_fullscreen = port_input_action("fullscreen", port_json_get(hotkeys, "fullscreen"),
                                               "input.hotkeys.fullscreen", "\"F11\"");
    port_action_save_state = port_input_action("save_state", port_json_get(hotkeys, "save_state"),
                                               "input.hotkeys.save_state", "\"F5\"");
    port_action_load_state = port_input_action("load_state", port_json_get(hotkeys, "load_state"),
                                               "input.hotkeys.load_state", "\"F9\"");
    /* the menu (menu.h): Escape, or Back (SELECT) held with Start on a gamepad (the chord's first input reaches
     * the pad until it is complete: "Input bindings") */
    port_action_menu = port_input_action("menu", port_json_get(hotkeys, "menu"), "input.hotkeys.menu",
                                         "[\"Escape\", [\"pad:back\", \"pad:start\"]]");
}

#ifdef PSXSTACK_SDL
#define INPUT_AXIS_PRESSED 16384 /* half way: a trigger or a stick direction counts as pressed beyond it */
#define INPUT_MAX_GAMEPADS 8

static u8 input_key_down[INPUT_KEYS];  /* from the key events */
static u8 input_down[PORT_PADS][INPUT_IDS]; /* every input of each player, at the last poll (the keys: player 1's) */
static u8 input_masked[INPUT_IDS];     /* player 1's inputs of a latched trigger */
static SDL_Gamepad *input_gamepads[INPUT_MAX_GAMEPADS];
static SDL_JoystickID input_player_id[PORT_PADS]; /* the gamepad of player 2.. (0: none); player 1 has the others */
static u16 input_applied[PORT_PADS];   /* the buttons last sent to psyq_pad_set, per port */
static int input_applied_connected[PORT_PADS]; /* and whether it said connected */
static int input_applied_any;          /* psyq_pad_set was called by this file */

/* ---- --input-test */
static int input_test_step;  /* the current step (input_test_steps: done) */
static int input_test_phase; /* 0: press now, 1: release now */
static int input_test_checks, input_test_failures;
static long input_test_wait;          /* frames spent waiting for the virtual gamepad */
static SDL_JoystickID input_test_pad; /* the virtual gamepad, player 1's (0: not attached) */
static SDL_Joystick *input_test_joy;
static SDL_JoystickID input_test_pad2; /* the second (the players' part; with player2 "first" attached before it) */
static SDL_Joystick *input_test_joy2;
/* The steps: each a set of inputs pressed together, and the action it must fire (-1: none). */
#define TEST_MAX_STEPS 192
static struct {
    int n;
    int ids[INPUT_CHORD];
    int action;
} input_test_steps[TEST_MAX_STEPS];
static int input_test_count, input_test_keys, input_test_pads, input_test_hotkeys;

static void input_gamepad_added(SDL_JoystickID id) {
    int i;
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (input_gamepads[i] == NULL) {
            input_gamepads[i] = SDL_OpenGamepad(id);
            if (input_gamepads[i] != NULL) {
                port_log("input: gamepad %s", SDL_GetGamepadName(input_gamepads[i]));
            }
            return;
        }
    }
}

static void input_gamepad_removed(SDL_JoystickID id) {
    int i;
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (input_gamepads[i] != NULL && SDL_GetGamepadID(input_gamepads[i]) == id) {
            SDL_CloseGamepad(input_gamepads[i]);
            input_gamepads[i] = NULL;
        }
    }
}

static int input_gamepad_open(SDL_JoystickID id) {
    int i;
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (input_gamepads[i] != NULL && SDL_GetGamepadID(input_gamepads[i]) == id) {
            return 1;
        }
    }
    return 0;
}

/* Who has which gamepad (input.player<n>): player n the n-th setting's gamepad by connection order (SDL's instance ids
 * grow with each connection, so sorting the open ones gives it), player 1 every other gamepad. */
static void input_assign(void) {
    SDL_JoystickID ids[INPUT_MAX_GAMEPADS];
    int n = 0, i, j, p;
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (input_gamepads[i] != NULL) {
            SDL_JoystickID id = SDL_GetGamepadID(input_gamepads[i]);
            for (j = n++; j > 0 && ids[j - 1] > id; j--) {
                ids[j] = ids[j - 1];
            }
            ids[j] = id;
        }
    }
    for (p = 1; p < PORT_PADS; p++) {
        int k = input_player_gamepad[p];
        input_player_id[p] = k > 0 && k <= n ? ids[k - 1] : 0;
    }
}

static int input_gamepad_player(SDL_JoystickID id) {
    int p;
    for (p = 1; p < PORT_PADS; p++) {
        if (input_player_id[p] == id) {
            return p;
        }
    }
    return 0;
}

/* A player's port has a pad in it: player 1's always, another's while its gamepad is there. */
static int input_connected(int player) {
    return player == 0 || input_player_id[player] != 0;
}

/* Every input's state: the keys from their events (player 1's), the gamepads read now (each ORed into its player's). */
static void input_read(void) {
    static const SDL_GamepadAxis axes[] = { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                            SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTX,
                                            SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_LEFTY,
                                            SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_RIGHTY };
    int i, j;
    input_assign();
    memset(input_down, 0, sizeof(input_down));
    memcpy(input_down[0], input_key_down, INPUT_KEYS);
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        SDL_Gamepad *g = input_gamepads[i];
        u8 *down;
        if (g == NULL) {
            continue;
        }
        down = input_down[input_gamepad_player(SDL_GetGamepadID(g))];
        for (j = 0; j < INPUT_PAD_BUTTONS; j++) {
            down[INPUT_PAD(j)] |= SDL_GetGamepadButton(g, (SDL_GamepadButton)j) ? 1 : 0;
        }
        for (j = 0; j < INPUT_PAD_INPUTS - INPUT_PAD_BUTTONS; j++) {
            Sint16 v = SDL_GetGamepadAxis(g, axes[j]);
            /* the triggers and the + directions: above half way; the - directions: below minus half way */
            int on = (j < 2 || (j & 1)) ? v > INPUT_AXIS_PRESSED : v < -INPUT_AXIS_PRESSED;
            down[INPUT_PAD(INPUT_PAD_BUTTONS + j)] |= on ? 1 : 0;
        }
    }
}

/* The actions from player 1's inputs (no other player's reach a hotkey): triggers completed (an action pressed),
 * held, latched; the masked inputs. */
static void input_actions_update(void) {
    int a, t, k;
    memset(input_masked, 0, sizeof(input_masked));
    for (a = 0; a < input_action_count; a++) {
        input_actions[a].held = input_actions[a].pressed = 0;
        for (t = 0; t < input_actions[a].count; t++) {
            InputTrigger *tr = &input_actions[a].triggers[t];
            int all = 1, any = 0;
            for (k = 0; k < tr->n; k++) {
                all &= input_down[0][tr->ids[k]];
                any |= input_down[0][tr->ids[k]];
            }
            if (all && !tr->complete) {
                input_actions[a].pressed = 1;
                tr->latched = 1;
            }
            tr->complete = all;
            if (!any) {
                tr->latched = 0;
            }
            input_actions[a].held |= all;
            if (tr->latched) {
                for (k = 0; k < tr->n; k++) {
                    input_masked[tr->ids[k]] = 1;
                }
            }
        }
    }
}

/* A player's pad from the map (player 1's without the inputs a hotkey masks). */
static u16 input_buttons(int player) {
    u16 bits = 0;
    int i;
    for (i = 0; i < input_map_count; i++) {
        if (input_down[player][input_map[i].id] && !(player == 0 && input_masked[input_map[i].id])) {
            bits |= input_map[i].bits;
        }
    }
    return bits;
}

static void input_push_key(SDL_Scancode key, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = key;
    e.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    e.key.down = down != 0;
    if (!SDL_PushEvent(&e)) {
        port_fatal("input test: SDL_PushEvent: %s", SDL_GetError());
    }
}

/* A virtual gamepad: every button in SDL_GamepadButton order (south .. touchpad) and 6 axes in SDL_GamepadAxis
 * order (SDL's virtual driver maps them one to one when the masks are 0). */
static void input_test_attach_one(const char *name, SDL_JoystickID *pad, SDL_Joystick **joy) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = INPUT_PAD_BUTTONS;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.name = name;
    *pad = SDL_AttachVirtualJoystick(&desc);
    if (*pad == 0) {
        port_fatal("input test: SDL_AttachVirtualJoystick: %s", SDL_GetError());
    }
    *joy = SDL_OpenJoystick(*pad);
    if (*joy == NULL) {
        port_fatal("input test: SDL_OpenJoystick: %s", SDL_GetError());
    }
}

static void input_test_detach_one(SDL_JoystickID *pad, SDL_Joystick **joy) {
    if (*pad != 0) {
        SDL_CloseJoystick(*joy);
        SDL_DetachVirtualJoystick(*pad);
        *pad = 0;
        *joy = NULL;
    }
}

/* Player 1's virtual gamepad; with input.player2 "first" the second one is attached before it (it is player 2's). */
static void input_test_attach(void) {
    if (input_player_gamepad[1] == 1 && input_test_pad2 == 0) {
        input_test_attach_one(PSXSTACK_GAME_ID " input test 2", &input_test_pad2, &input_test_joy2);
    }
    input_test_attach_one(PSXSTACK_GAME_ID " input test", &input_test_pad, &input_test_joy);
}

static void input_test_set_on(SDL_Joystick *joy, int id, int down) {
    static const SDL_GamepadAxis axes[] = { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                            SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTX,
                                            SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_LEFTY,
                                            SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_RIGHTY };
    int p;
    if (id < INPUT_KEYS) {
        input_push_key((SDL_Scancode)id, down);
        return;
    }
    p = id - INPUT_KEYS;
    if (p < INPUT_PAD_BUTTONS) {
        SDL_SetJoystickVirtualButton(joy, p, down != 0);
    } else {
        int j = p - INPUT_PAD_BUTTONS;
        Sint16 rest = j < 2 ? -32768 : 0, on = (j < 2 || (j & 1)) ? 32767 : -32768;
        SDL_SetJoystickVirtualAxis(joy, axes[j], down ? on : rest);
    }
}

static void input_test_set(int id, int down) {
    input_test_set_on(input_test_joy, id, down);
}

static int input_test_has(int id) {
    int i;
    for (i = 0; i < input_test_count; i++) {
        if (input_test_steps[i].n == 1 && input_test_steps[i].action < 0 && input_test_steps[i].ids[0] == id) {
            return 1;
        }
    }
    return 0;
}

static void input_test_add(int n, const int *ids, int action) {
    if (input_test_count == TEST_MAX_STEPS) {
        port_fatal("input test: more than %d steps", TEST_MAX_STEPS);
    }
    input_test_steps[input_test_count].n = n;
    memcpy(input_test_steps[input_test_count].ids, ids, sizeof(int) * (size_t)n);
    input_test_steps[input_test_count].action = action;
    input_test_count++;
}

/* The steps: every keyboard input of the map, every gamepad input, a chord (two keys and a gamepad input), every
 * hotkey trigger. */
static void input_test_plan(void) {
    int i, t, pass, chord[3], c = 0;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < input_map_count; i++) {
            int id = input_map[i].id;
            if ((id >= INPUT_KEYS) == pass && !input_test_has(id)) {
                input_test_add(1, &id, -1);
                if (pass == 0) {
                    input_test_keys++;
                    if (c < 2) {
                        chord[c++] = id;
                    }
                } else {
                    input_test_pads++;
                    if (c == 2) {
                        chord[c++] = id;
                    }
                }
            }
        }
    }
    if (c == 3) {
        input_test_add(3, chord, -1);
    }
    for (i = 0; i < input_action_count; i++) {
        for (t = 0; t < input_actions[i].count; t++) {
            input_test_add(input_actions[i].triggers[t].n, input_actions[i].triggers[t].ids, i);
            input_test_hotkeys++;
        }
    }
}

static u16 input_test_expected;
static int input_test_players;  /* the players' part: 1 plan, 2 both gamepads open, 3 the steps, 4..7 the unplugs */
#define TEST_MAX_PSTEPS 16
static struct {
    int pads; /* 1 the first virtual gamepad, 2 the second, 3 both */
    int n;
    int ids[INPUT_CHORD];
} input_test_psteps[TEST_MAX_PSTEPS];
static int input_test_pcount, input_test_photkeys, input_test_pstep, input_test_pphase, input_test_ppending;
static int input_test_pchecks, input_test_pfailures;
static u16 input_test_pexpected[PORT_PADS];
static int input_test_pconnected[PORT_PADS];
static int input_test_players_before(void);
static void input_test_players_after(void);
static int input_test_pending; /* a step's input was applied this frame: check after the poll */
static int input_test_pause;   /* the pause's round trip: 1 press now, 2 paused (then released) */
static int input_test_pause_polls;
static long input_test_pause_frame, input_test_pause_resumed;
static int input_test_pause_before(void);
static void input_test_paused(void);
static int input_test_state;   /* the quick slot's round trip: 1 press save, 2 release, 3 press load, 4 loaded;
                                * then paused: 5 press pause, 6 the pause's polls (save, load, re-paused, pause),
                                * 7 resumed */
static int input_test_state_polls;
static long input_test_state_frame;
static char input_test_state_path[4096];
static int input_test_state_before(void);
static int input_test_menu;    /* the menu's round trip: 1 press the hotkey, 2 in the pause's polls, 3 resumed */
static int input_test_menu_polls, input_test_menu_seen;
static long input_test_menu_frame;
static int input_test_menu_before(void);
static void input_test_menu_paused(void);
static int input_test_finish(void);

static int input_test_needs_pad(int step) {
    int k;
    for (k = 0; k < input_test_steps[step].n; k++) {
        if (input_test_steps[step].ids[k] >= INPUT_KEYS) {
            return 1;
        }
    }
    return 0;
}

/* Before the poll: this frame's step (a press or its release). */
static void input_test_before(void) {
    int k, i, masked;
    if (input_test_players_before() || input_test_pause_before() || input_test_state_before() ||
        input_test_menu_before()) {
        return;
    }
    if (port_frames < 2 || input_test_step >= input_test_count) {
        return;
    }
    if (input_test_phase == 0 && input_test_needs_pad(input_test_step)) {
        if (input_test_pad == 0) {
            input_test_attach();
        }
        if (!input_gamepad_open(input_test_pad)) {
            if (++input_test_wait > 10) {
                port_fatal("input test: the virtual gamepad was not opened after 10 frames");
            }
            return; /* its SDL_EVENT_GAMEPAD_ADDED comes with a poll */
        }
    }
    input_test_expected = 0;
    for (k = 0; k < input_test_steps[input_test_step].n; k++) {
        int id = input_test_steps[input_test_step].ids[k];
        input_test_set(id, input_test_phase == 0);
        /* the map's bits, unless a single-input hotkey or this step's own hotkey masks the input */
        masked = input_test_steps[input_test_step].action >= 0;
        for (i = 0; i < input_action_count && !masked; i++) {
            int t;
            for (t = 0; t < input_actions[i].count; t++) {
                masked |= input_actions[i].triggers[t].n == 1 && input_actions[i].triggers[t].ids[0] == id;
            }
        }
        for (i = 0; i < input_map_count && input_test_phase == 0 && !masked; i++) {
            if (input_map[i].id == id) {
                input_test_expected |= input_map[i].bits;
            }
        }
    }
    input_test_pending = 1;
}

/* After the poll: the pad must show the step's bits (nothing at all with a script), a hotkey must have fired. */
static void input_test_after(void) {
    int ok, action;
    if (input_test_players != 0) {
        input_test_players_after();
        return;
    }
    if (!input_test_pending) {
        return;
    }
    input_test_pending = 0;
    input_test_checks++;
    action = input_test_steps[input_test_step].action;
    if (port_script_active) {
        ok = !input_applied_any && input_buttons(0) == input_test_expected;
    } else {
        ok = input_applied[0] == input_test_expected;
    }
    if (action >= 0 && input_test_phase == 0) {
        ok &= input_actions[action].pressed;
    }
    if (!ok) {
        input_test_failures++;
        port_log("input test: frame %ld step %d %s: pad 0x%04X (computed 0x%04X), expected 0x%04X%s%s%s", port_frames,
                 input_test_step, input_test_phase == 0 ? "press" : "release", input_applied[0], input_buttons(0),
                 input_test_expected, port_script_active ? " and nothing sent (a script owns the pad)" : "",
                 action >= 0 ? ", the action " : "", action >= 0 ? input_actions[action].name : "");
    }
    if (input_test_phase == 0) {
        input_test_phase = 1;
        return;
    }
    input_test_phase = 0;
    if (++input_test_step < input_test_count) {
        return;
    }
    port_log("input test: %d of %d checks passed (%d keys, %d gamepad inputs, 1 chord, %d hotkey triggers)%s",
             input_test_checks - input_test_failures, input_test_checks, input_test_keys, input_test_pads,
             input_test_hotkeys, port_script_active ? "; the script owned the pad: nothing was sent" : "");
    if (input_test_failures != 0) {
        port_exit(6, "input test failed");
    }
    input_test_players = 1; /* then the players' part */
}

/* After the players' part: the virtual gamepads go, then (without a script) the pause's round trip. */
static void input_test_players_done(void) {
    input_test_detach_one(&input_test_pad2, &input_test_joy2);
    input_test_detach_one(&input_test_pad, &input_test_joy);
    if (!port_script_active && input_actions[port_action_pause].count > 0) {
        input_test_pause = 1; /* then the pause's round trip */
        return;
    }
    input_test = input_test_mute = 0;
    if (!port_script_active) {
        port_exit(0, "input test passed");
    }
}

/* The players' part (after the steps): `input.player2` decides where each virtual gamepad goes. The first (player 1's
 * by construction: with "first" the second was attached before it) always drives port 0; the second drives port 1
 * with "first" or "second", else port 0 too, and port 1 is connected exactly then. Steps: a gamepad input of the map
 * (one no single-input hotkey takes) on the first, on the second, on both; with a player 2, each hotkey trigger made of
 * gamepad inputs only, on the second (it must reach pad 2 and fire nothing). Each a press in one frame, the release in
 * the next; then the second detached (port 1 then has the first with "first", nothing otherwise), then the first
 * (nothing at all). Skipped when the map has no such gamepad input, or other gamepads are open. */
static void input_test_players_add(int pads, int n, const int *ids) {
    if (input_test_pcount == TEST_MAX_PSTEPS) {
        port_fatal("input test: more than %d players' steps", TEST_MAX_PSTEPS);
    }
    input_test_psteps[input_test_pcount].pads = pads;
    input_test_psteps[input_test_pcount].n = n;
    memcpy(input_test_psteps[input_test_pcount].ids, ids, sizeof(int) * (size_t)n);
    input_test_pcount++;
}

static int input_test_single_hotkey(int id) {
    int a, t;
    for (a = 0; a < input_action_count; a++) {
        for (t = 0; t < input_actions[a].count; t++) {
            if (input_actions[a].triggers[t].n == 1 && input_actions[a].triggers[t].ids[0] == id) {
                return 1;
            }
        }
    }
    return 0;
}

static void input_test_players_plan(void) {
    int i, a, t, k, g = -1;
    for (i = 0; i < input_map_count && g < 0; i++) {
        if (input_map[i].id >= INPUT_KEYS && !input_test_single_hotkey(input_map[i].id)) {
            g = input_map[i].id;
        }
    }
    input_test_pcount = 0;
    if (g < 0) {
        return;
    }
    input_test_players_add(1, 1, &g);
    input_test_players_add(2, 1, &g);
    input_test_players_add(3, 1, &g);
    for (a = 0; a < input_action_count && input_player_gamepad[1] != 0; a++) {
        for (t = 0; t < input_actions[a].count; t++) {
            const InputTrigger *tr = &input_actions[a].triggers[t];
            int pads_only = 1;
            for (k = 0; k < tr->n; k++) {
                pads_only &= tr->ids[k] >= INPUT_KEYS;
            }
            if (pads_only) {
                input_test_players_add(2, tr->n, tr->ids);
                input_test_photkeys++;
            }
        }
    }
}

static int input_test_gamepads_open(void) {
    int i, n = 0;
    for (i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        n += input_gamepads[i] != NULL;
    }
    return n;
}

/* Before the poll. Returns 1 while the players' part runs (the other parts wait). */
static int input_test_players_before(void) {
    int k, q, pad;
    switch (input_test_players) {
    case 1: /* the plan and the second gamepad */
        input_test_players_plan();
        if (input_test_pcount == 0) {
            port_log("input test: the players: skipped (no gamepad input in the map that no hotkey takes alone)");
            input_test_players = 0;
            input_test_players_done();
            return 1;
        }
        if (input_test_pad == 0) {
            input_test_attach();
        }
        if (input_test_pad2 == 0) {
            input_test_attach_one(PSXSTACK_GAME_ID " input test 2", &input_test_pad2, &input_test_joy2);
        }
        input_test_wait = 0;
        input_test_players = 2;
        return 1;
    case 2: /* both open (their SDL_EVENT_GAMEPAD_ADDED come with a poll) */
        if (!input_gamepad_open(input_test_pad) || !input_gamepad_open(input_test_pad2)) {
            if (++input_test_wait > 10) {
                port_fatal("input test: the second virtual gamepad was not opened after 10 frames");
            }
            return 1;
        }
        if (input_test_gamepads_open() != 2) {
            port_log("input test: the players: skipped (%d other gamepads are open)", input_test_gamepads_open() - 2);
            input_test_players = 0;
            input_test_players_done();
            return 1;
        }
        input_test_pstep = input_test_pphase = 0;
        input_test_players = 3; /* the first step at once */
        /* fallthrough */
    case 3:
        memset(input_test_pexpected, 0, sizeof(input_test_pexpected));
        input_test_pconnected[0] = 1;
        input_test_pconnected[1] = input_player_gamepad[1] != 0;
        for (pad = 1; pad <= 2; pad++) {
            if (!(input_test_psteps[input_test_pstep].pads & pad)) {
                continue;
            }
            q = pad == 2 && input_player_gamepad[1] != 0 ? 1 : 0;
            for (k = 0; k < input_test_psteps[input_test_pstep].n; k++) {
                int id = input_test_psteps[input_test_pstep].ids[k], i;
                input_test_set_on(pad == 1 ? input_test_joy : input_test_joy2, id, input_test_pphase == 0);
                for (i = 0; i < input_map_count && input_test_pphase == 0; i++) {
                    if (input_map[i].id == id) {
                        input_test_pexpected[q] |= input_map[i].bits;
                    }
                }
            }
        }
        input_test_ppending = 1;
        return 1;
    case 4: /* unplug the second, then the first */
    case 6:
        if (input_test_players == 4) {
            input_test_detach_one(&input_test_pad2, &input_test_joy2);
        } else {
            input_test_detach_one(&input_test_pad, &input_test_joy);
        }
        input_test_wait = 0;
        input_test_players++;
        return 1;
    case 5:
    case 7:
        return 1;
    default:
        return 0;
    }
}

/* After the poll: both ports as expected, no hotkey fired; the unplugs once their gamepad is closed. */
static void input_test_players_after(void) {
    const char *mode = input_player_names[input_player_gamepad[1]], *fired = NULL;
    int ok = 1, p, a;
    if (input_test_players == 5 || input_test_players == 7) {
        if (input_gamepad_open(input_test_players == 5 ? input_test_pad2 : input_test_pad)) {
            /* not yet: SDL_EVENT_GAMEPAD_REMOVED comes with a poll */
            if (++input_test_wait > 10) {
                port_fatal("input test: a detached virtual gamepad was still open after 10 frames");
            }
            return;
        }
        memset(input_test_pexpected, 0, sizeof(input_test_pexpected));
        input_test_pconnected[0] = 1;
        /* the first is now the only gamepad: player 2's with "first" */
        input_test_pconnected[1] = input_test_players == 5 && input_player_gamepad[1] == 1;
    } else if (input_test_players != 3 || !input_test_ppending) {
        return;
    }
    input_test_ppending = 0;
    input_test_pchecks++;
    for (p = 0; p < PORT_PADS; p++) {
        u16 sent = port_script_active ? input_buttons(p) : input_applied[p];
        int connected = port_script_active ? input_connected(p) : input_applied_connected[p];
        ok &= sent == input_test_pexpected[p] && connected == input_test_pconnected[p];
    }
    if (port_script_active) {
        ok &= !input_applied_any;
    }
    for (a = 0; a < input_action_count; a++) {
        if (input_actions[a].pressed) {
            fired = input_actions[a].name;
            ok = 0;
        }
    }
    if (!ok) {
        input_test_pfailures++;
        port_log("input test: frame %ld the players (input.player2 \"%s\"), %s: port 0 0x%04X, port 1 0x%04X%s; "
                 "expected 0x%04X, 0x%04X%s%s%s%s",
                 port_frames, mode,
                 input_test_players == 3 ? (input_test_pphase == 0 ? "press" : "release")
                 : input_test_players == 5 ? "the second unplugged" : "the first unplugged",
                 port_script_active ? input_buttons(0) : input_applied[0],
                 port_script_active ? input_buttons(1) : input_applied[1],
                 (port_script_active ? input_connected(1) : input_applied_connected[1]) ? "" : " (none)",
                 input_test_pexpected[0], input_test_pexpected[1], input_test_pconnected[1] ? "" : " (none)",
                 port_script_active ? " and nothing sent (a script owns the pad)" : "",
                 fired != NULL ? ", and no hotkey, but this fired: " : "", fired != NULL ? fired : "");
    }
    if (input_test_players == 5 || input_test_players == 7) {
        input_test_players++;
    } else if (input_test_pphase == 0) {
        input_test_pphase = 1;
        return;
    } else {
        input_test_pphase = 0;
        if (++input_test_pstep == input_test_pcount) {
            input_test_players = 4;
        }
        return;
    }
    if (input_test_players != 8) {
        return;
    }
    port_log("input test: the players (input.player2 \"%s\"): %d of %d checks passed (%d presses, %d hotkey triggers "
             "on the second gamepad, 2 unplugs)%s",
             mode, input_test_pchecks - input_test_pfailures, input_test_pchecks,
             input_test_pcount - input_test_photkeys, input_test_photkeys, port_script_active ? "; the script owned the pad: nothing was sent" : "");
    input_test_players = 0;
    if (input_test_pfailures != 0) {
        port_exit(6, "input test failed");
    }
    input_test_players_done();
}

/* The pause's round trip (after the steps, without a script): the pause key's first trigger pressed at a vsync (the
 * pump pauses at its end), released at the first poll of the pause and pressed again at the third (the pump goes
 * on), released at the next vsync: the pause must have polled three times without a vsync. */
static void input_test_pause_set(int down) {
    const InputTrigger *tr = &input_actions[port_action_pause].triggers[0];
    int k;
    for (k = 0; k < tr->n; k++) {
        input_test_set(tr->ids[k], down);
    }
}

static int input_test_pause_before(void) {
    if (input_test_pause == 1) {
        input_test_mute = 0;
        input_test_pause_set(1);
        input_test_pause_frame = port_frames;
        input_test_pause = 2;
        return 1;
    }
    if (input_test_pause == 2) {
        int ok = input_test_pause_polls == 3 && input_test_pause_resumed == input_test_pause_frame;
        input_test_pause_set(0);
        port_log("input test: the pause: %s (%d polls paused at frame %ld, resumed at frame %ld)",
                 ok ? "passed" : "FAILED", input_test_pause_polls, input_test_pause_frame, input_test_pause_resumed);
        input_test_pause = 0;
        if (!ok) {
            port_exit(6, "input test failed");
        }
        if (port_savestate_armed && port_savestate_quick_get() != NULL &&
            input_actions[port_action_save_state].count > 0 && input_actions[port_action_load_state].count > 0) {
            input_test_state = 1; /* then the quick slot's round trip */
            return 1;
        }
        port_log("input test: the quick slot: skipped (%s)",
                 !port_savestate_armed ? "save states are off in this run" : "no slot, or a hotkey unbound");
        return input_test_finish();
    }
    return 0;
}

/* The quick slot's round trip (after the pause's), running: the save_state hotkey's first trigger pressed at a
 * vsync (the slot, a file of the test's own beside the real one, is written at its end), released at the next, where
 * the file must exist; the load_state hotkey's pressed at the next (the load goes through main at its end), so the
 * vsync after it is the saved one's successor again: the frame count went back. Then paused: the pause key at a
 * vsync G; in the pause's polls, the pause key released, save pressed (the slot as of G's end) and released, load
 * pressed (the pump leaves the pause, loads, and pauses again at G's end), released, the pause key again; the next
 * vsync must be G + 1 after six polls. The file is removed. */
static void input_test_state_set(int action, int down) {
    const InputTrigger *tr = &input_actions[action].triggers[0];
    int k;
    for (k = 0; k < tr->n; k++) {
        input_test_set(tr->ids[k], down);
    }
}

static int input_test_state_before(void) {
    FILE *f;
    switch (input_test_state) {
    case 1:
        snprintf(input_test_state_path, sizeof(input_test_state_path), "%s.input-test", port_savestate_quick_get());
        port_savestate_quick_path(input_test_state_path);
        input_test_state_set(port_action_save_state, 1);
        input_test_state_frame = port_frames;
        input_test_state = 2;
        return 1;
    case 2:
        input_test_state_set(port_action_save_state, 0);
        f = fopen(input_test_state_path, "rb");
        if (f == NULL) {
            port_log("input test: the quick slot: FAILED (nothing at %s after the save_state hotkey)",
                     input_test_state_path);
            port_exit(6, "input test failed");
        }
        fclose(f);
        input_test_state = 3;
        return 1;
    case 3:
        input_test_state_set(port_action_load_state, 1);
        input_test_state = 4;
        return 1;
    case 4: {
        int ok = port_frames == input_test_state_frame + 1;
        input_test_state_set(port_action_load_state, 0);
        remove(input_test_state_path);
        port_log("input test: the quick slot, running: %s (saved at the end of frame %ld, loaded by the load_state "
                 "hotkey: frame %ld follows)",
                 ok ? "passed" : "FAILED", input_test_state_frame, port_frames);
        if (!ok) {
            port_exit(6, "input test failed");
        }
        input_test_state = 5;
        return 1;
    }
    case 5:
        input_test_state_set(port_action_pause, 1);
        input_test_state_frame = port_frames;
        input_test_state = 6;
        return 1;
    case 7: {
        int ok = port_frames == input_test_state_frame + 1 && input_test_state_polls == 6;
        input_test_state_set(port_action_pause, 0);
        f = fopen(input_test_state_path, "rb");
        if (f == NULL) {
            ok = 0;
        } else {
            fclose(f);
        }
        remove(input_test_state_path);
        port_log("input test: the quick slot, paused: %s (%d polls paused at frame %ld, the slot %s, frame %ld "
                 "follows)",
                 ok ? "passed" : "FAILED", input_test_state_polls, input_test_state_frame,
                 f != NULL ? "written" : "NOT written", port_frames);
        if (!ok) {
            port_exit(6, "input test failed");
        }
        input_test_state = 0;
        return input_test_finish();
    }
    default:
        return 0;
    }
}

/* After the quick slot's round trip (or its skip): the menu's, when this build has the menu and a hotkey for it
 * (1: it starts at the next vsync); else the test is over. */
static int input_test_finish(void) {
    if (port_menu_compiled() && input_actions[port_action_menu].count > 0) {
        input_test_menu = 1;
        return 1;
    }
    port_log("input test: the menu: skipped (%s)",
             !port_menu_compiled() ? "not in this build (PSXSTACK_MENU)" : "its hotkey is unbound");
    port_exit(0, "input test passed");
}

/* The menu's round trip: the menu hotkey's first trigger pressed at a vsync (the pump pauses at its end with the
 * menu up), released at the first poll of the pause, where the menu must be shown (the second poll's present is
 * the <PREFIX>_PORT_MENU_SHOT picture), pressed again at the third (the pump closes the menu and goes on), released at
 * the next vsync, which must follow the paused one: three polls, the menu seen, no menu after. */
static int input_test_menu_before(void) {
    if (input_test_menu == 1) {
        input_test_state_set(port_action_menu, 1);
        input_test_menu_frame = port_frames;
        input_test_menu = 2;
        return 1;
    }
    if (input_test_menu == 3) {
        int ok = input_test_menu_polls == 3 && input_test_menu_seen && !port_menu_shown() &&
                 port_frames == input_test_menu_frame + 1;
        input_test_state_set(port_action_menu, 0);
        port_log("input test: the menu: %s (%d polls paused at frame %ld, the menu %s, %s after, frame %ld follows)",
                 ok ? "passed" : "FAILED", input_test_menu_polls, input_test_menu_frame,
                 input_test_menu_seen ? "shown" : "NOT shown", port_menu_shown() ? "still up" : "closed", port_frames);
        port_exit(ok ? 0 : 6, ok ? "input test passed" : "input test failed");
    }
    return 0;
}

/* <PREFIX>_PORT_MENU_LIVE_FILE=PATH (the test of the live apply with a change: tests/hello_test.py --menu): that file's
 * text written over the settings file before it is re-read, as the menu's save would; the menu hotkey pressed at
 * the next poll is the file's binding, since the step reads the action's trigger then. */
static void input_test_live_file(void) {
    const char *path = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_MENU_LIVE_FILE");
    FILE *in, *out;
    char buf[4096];
    size_t n;
    if (path == NULL) {
        return;
    }
    in = fopen(path, "rb");
    out = in != NULL ? fopen(port_settings.file, "wb") : NULL;
    if (in == NULL || out == NULL) {
        port_log("input test: live file %s: cannot copy over %s", path, port_settings.file);
        port_exit(6, "input test failed");
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    port_log("input test: live file %s in place of the settings", path);
}

static void input_test_menu_paused(void) {
    if (input_test_menu != 2) {
        return;
    }
    switch (++input_test_menu_polls) {
    case 1:
        input_test_state_set(port_action_menu, 0);
        input_test_menu_seen = port_menu_shown();
        break;
    case 2:
        /* the settings applied live as the menu does after a save (runtime/live.c): the hotkey table starts over,
         * and the menu hotkey pressed at the next poll must still close the menu */
        if (port_settings_loaded) {
            int ok;
            input_test_live_file(); /* <PREFIX>_PORT_MENU_LIVE_FILE: another settings file put in place first */
            ok = port_live_apply();
            port_log("input test: the settings re-applied live in the menu: %s", ok ? "passed" : "FAILED (not re-read)");
            if (!ok) {
                port_exit(6, "input test failed");
            }
        }
        break;
    case 3:
        input_test_state_set(port_action_menu, 1);
        input_test_menu = 3;
        break;
    default:
        break;
    }
}

/* The paused part's polls (port_input_poll_paused, from the pump's pause loop). */
static void input_test_state_paused(void) {
    if (input_test_state != 6) {
        return;
    }
    switch (++input_test_state_polls) {
    case 1:
        input_test_state_set(port_action_pause, 0);
        break;
    case 2:
        input_test_state_set(port_action_save_state, 1);
        break;
    case 3:
        input_test_state_set(port_action_save_state, 0);
        break;
    case 4:
        input_test_state_set(port_action_load_state, 1); /* the pump leaves the pause, loads, pauses again */
        break;
    case 5:
        input_test_state_set(port_action_load_state, 0);
        break;
    case 6:
        input_test_state_set(port_action_pause, 1);
        input_test_state = 7;
        break;
    default:
        break;
    }
}

static void input_test_paused(void) {
    if (input_test_pause != 2) {
        return;
    }
    input_test_pause_polls++;
    if (input_test_pause_polls == 1) {
        input_test_pause_set(0);
    } else if (input_test_pause_polls == 3) {
        input_test_pause_set(1);
        input_test_pause_resumed = port_frames;
    }
}

void port_input_init(int test) {
    input_test = input_test_mute = test;
    if (test) {
        input_test_plan();
    }
}

/* SDL's events, the inputs and the actions; the fullscreen hotkey acts here. */
static void input_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        port_menu_event(&e); /* the menu's own navigation while it is shown; the keys below still count */
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            port_exit(0, "window closed");
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if ((int)e.key.scancode >= 0 && (int)e.key.scancode < INPUT_KEYS) {
                input_key_down[e.key.scancode] = e.key.down ? 1 : 0;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            memset(input_key_down, 0, sizeof(input_key_down)); /* no key stays held while another window has them */
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            input_gamepad_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            input_gamepad_removed(e.gdevice.which);
            break;
        default:
            break;
        }
    }
    input_read();
    input_actions_update();
    if (port_input_pressed(port_action_fullscreen)) {
        port_video_toggle_fullscreen();
    }
}

void port_input_frame(void) {
    if (input_test) {
        input_test_before();
    }
    input_poll();
    if (!port_script_active) {
        int p;
        for (p = 0; p < PORT_PADS; p++) {
            int connected = input_connected(p);
            if (port_debug_pad_owned & (1 << p)) {
                continue; /* the debug channel drives that port */
            }
            u16 bits = connected ? input_buttons(p) : 0;
            psyq_pad_set(p, connected, bits);
            port_framelog_input(p, connected, bits);
            input_applied[p] = bits;
            input_applied_connected[p] = connected;
            input_applied_any = 1;
        }
    }
    if (input_test) {
        input_test_after();
    }
}

void port_input_poll_paused(void) {
    if (input_test) {
        input_test_paused();
        input_test_state_paused();
        input_test_menu_paused();
    }
    input_poll();
}
#else
void port_input_init(int test) {
    input_test = input_test_mute = test;
}

void port_input_frame(void) {
}

void port_input_poll_paused(void) {
}
#endif
