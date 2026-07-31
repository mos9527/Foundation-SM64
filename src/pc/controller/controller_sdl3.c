// SDL3 gamepad backend for sm64ex, ported from controller_sdl2.c.
//
// Mirrors the SDL2 backend's behaviour (single global gamepad for player 1,
// polling in read(), rumble, mouse-relative binding) but uses the SDL3 gamepad
// API. Unlike the SDL2 backend this is built for the Foundation host, which
// already initialises SDL3 (SDL_INIT_GAMEPAD) and pumps SDL events itself, so
// this backend only needs controller_sdl3_event() to be fed gamepad add/remove
// events; everything else is read by polling in read().
#ifdef CAPI_SDL3

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>

// Analog camera movement by Pathétique (github.com/vrmiguel), y0shin and Mors
// Contribute or communicate bugs at github.com/vrmiguel/sm64-analog-camera

#include <ultra64.h>

#include "controller_api.h"
#include "controller_sdl3.h"
#include "../configfile.h"
#include "../platform.h"
#include "../fs/fs.h"

#include "game/level_update.h"

// mouse buttons are also in the controller namespace (why), just offset 0x100
#define VK_OFS_SDL_MOUSE 0x0100
#define VK_BASE_SDL_MOUSE (VK_BASE_SDL_GAMEPAD + VK_OFS_SDL_MOUSE)
#define MAX_JOYBINDS 32
#define MAX_MOUSEBUTTONS 8 // arbitrary
#define MAX_JOYBUTTONS 64  // covers every SDL3 gamepad button index + trigger virtual keys
#define AXIS_THRESHOLD (30 * 256)

float mouse_x;
float mouse_y;

#ifdef BETTERCAMERA
extern u8 newcam_mouse;
#endif

static bool init_ok;
static SDL_Gamepad *sdl_gp;

static u32 num_joy_binds = 0;
static u32 num_mouse_binds = 0;
static u32 joy_binds[MAX_JOYBINDS][2];
static u32 mouse_binds[MAX_JOYBINDS][2];

static bool joy_buttons[MAX_JOYBUTTONS] = { false };
static u32 mouse_buttons = 0;
static u32 last_mouse = VK_INVALID;
static u32 last_joybutton = VK_INVALID;

// Open the first connected gamepad if we don't have one yet. Called from read()
// and from controller_sdl3_event() on SDL_EVENT_GAMEPAD_ADDED.
static void controller_sdl3_open_first(void) {
    if (sdl_gp != NULL)
        return;
    int count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
    if (gamepads) {
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad *gp = SDL_OpenGamepad(gamepads[i]);
            if (gp != NULL) {
                sdl_gp = gp;
                fprintf(stderr, "[sdl3] opened gamepad '%s' (instance %u)\n",
                        SDL_GetGamepadName(gp), (unsigned)gamepads[i]);
                break;
            }
        }
        SDL_free(gamepads);
    }
}

static inline void controller_add_binds(const u32 mask, const u32 *btns) {
    for (u32 i = 0; i < MAX_BINDS; ++i) {
        if (btns[i] >= VK_BASE_SDL_GAMEPAD && btns[i] <= VK_BASE_SDL_GAMEPAD + VK_SIZE) {
            if (btns[i] >= VK_BASE_SDL_MOUSE && num_joy_binds < MAX_JOYBINDS) {
                mouse_binds[num_mouse_binds][0] = btns[i] - VK_BASE_SDL_MOUSE;
                mouse_binds[num_mouse_binds][1] = mask;
                ++num_mouse_binds;
            } else if (num_mouse_binds < MAX_JOYBINDS) {
                joy_binds[num_joy_binds][0] = btns[i] - VK_BASE_SDL_GAMEPAD;
                joy_binds[num_joy_binds][1] = mask;
                ++num_joy_binds;
            }
        }
    }
}

static void controller_sdl3_bind(void) {
    memset(joy_binds, 0, sizeof(joy_binds));
    memset(mouse_binds, 0, sizeof(mouse_binds));
    num_joy_binds = 0;
    num_mouse_binds = 0;

    controller_add_binds(A_BUTTON,     configKeyA);
    controller_add_binds(B_BUTTON,     configKeyB);
    controller_add_binds(Z_TRIG,       configKeyZ);
    controller_add_binds(STICK_UP,     configKeyStickUp);
    controller_add_binds(STICK_LEFT,   configKeyStickLeft);
    controller_add_binds(STICK_DOWN,   configKeyStickDown);
    controller_add_binds(STICK_RIGHT,  configKeyStickRight);
    controller_add_binds(U_CBUTTONS,   configKeyCUp);
    controller_add_binds(L_CBUTTONS,   configKeyCLeft);
    controller_add_binds(D_CBUTTONS,   configKeyCDown);
    controller_add_binds(R_CBUTTONS,   configKeyCRight);
    controller_add_binds(L_TRIG,       configKeyL);
    controller_add_binds(R_TRIG,       configKeyR);
    controller_add_binds(START_BUTTON, configKeyStart);
}

static void controller_sdl3_init(void) {
    // The gamepad subsystem is already initialized by the host's wm_init()
    // (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)). Make sure it's up here too,
    // but don't bail on a non-zero return: re-initializing already-active
    // subsystems (or the SDL3 no-op SDL_INIT_EVENTS) can return != 0 without
    // meaning failure, and a failed gate here would leave init_ok=false and
    // silently disable the entire backend.
    SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

#ifdef BETTERCAMERA
    if (newcam_mouse == 1)
        SDL_SetRelativeMouseMode(SDL_TRUE);
#endif

    SDL_GetRelativeMouseState(&mouse_x, &mouse_y);

    controller_sdl3_bind();

    init_ok = true;

    int num = 0;
    SDL_JoystickID *gps = SDL_GetGamepads(&num);
    SDL_free(gps);
    fprintf(stderr, "[sdl3] controller init ok; %d gamepad(s) currently connected\n", num);

    // Match SM64.cpp: probe for an already-connected gamepad right away instead
    // of waiting for the first read() frame.
    controller_sdl3_open_first();
}

static inline void update_button(const int i, const bool new) {
    const bool pressed = !joy_buttons[i] && new;
    joy_buttons[i] = new;
    if (pressed) last_joybutton = i;
}

static void controller_sdl3_read(OSContPad *pad) {
    if (!init_ok) {
        return;
    }

#ifdef BETTERCAMERA
    if (newcam_mouse == 1 && sCurrPlayMode != 2)
        SDL_SetRelativeMouseMode(SDL_TRUE);
    else
        SDL_SetRelativeMouseMode(SDL_FALSE);
#endif

    u32 mouse = SDL_GetRelativeMouseState(&mouse_x, &mouse_y);

    for (u32 i = 0; i < num_mouse_binds; ++i)
        if (mouse & SDL_BUTTON_MASK(mouse_binds[i][0]))
            pad->button |= mouse_binds[i][1];

    // remember buttons that changed from 0 to 1
    last_mouse = (mouse_buttons ^ mouse) & mouse;
    mouse_buttons = mouse;

    SDL_UpdateGamepads();

    if (sdl_gp != NULL && !SDL_GamepadConnected(sdl_gp)) {
        fprintf(stderr, "[sdl3] gamepad disconnected, closing\n");
        SDL_CloseGamepad(sdl_gp);
        sdl_gp = NULL;
    }

    if (sdl_gp == NULL) {
        controller_sdl3_open_first();
        if (sdl_gp == NULL) {
            return;
        }
    }

    int16_t leftx = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_LEFTX);
    int16_t lefty = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_LEFTY);
    int16_t rightx = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_RIGHTX);
    int16_t righty = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_RIGHTY);

    int16_t ltrig = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    int16_t rtrig = SDL_GetGamepadAxis(sdl_gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    for (u32 i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        const bool new = SDL_GetGamepadButton(sdl_gp, (SDL_GamepadButton)i);
        update_button((int)i, new);
    }

    update_button(VK_LTRIGGER - VK_BASE_SDL_GAMEPAD, ltrig > AXIS_THRESHOLD);
    update_button(VK_RTRIGGER - VK_BASE_SDL_GAMEPAD, rtrig > AXIS_THRESHOLD);

    u32 buttons_down = 0;
    for (u32 i = 0; i < num_joy_binds; ++i)
        if (joy_buttons[joy_binds[i][0]])
            buttons_down |= joy_binds[i][1];

    pad->button |= buttons_down;

    const u32 xstick = buttons_down & STICK_XMASK;
    const u32 ystick = buttons_down & STICK_YMASK;
    if (xstick == STICK_LEFT)
        pad->stick_x = -128;
    else if (xstick == STICK_RIGHT)
        pad->stick_x = 127;
    if (ystick == STICK_DOWN)
        pad->stick_y = -128;
    else if (ystick == STICK_UP)
        pad->stick_y = 127;

    if (rightx < -0x4000) pad->button |= L_CBUTTONS;
    if (rightx > 0x4000) pad->button |= R_CBUTTONS;
    if (righty < -0x4000) pad->button |= U_CBUTTONS;
    if (righty > 0x4000) pad->button |= D_CBUTTONS;

    uint32_t magnitude_sq = (uint32_t)(leftx * leftx) + (uint32_t)(lefty * lefty);
    uint32_t stickDeadzoneActual = configStickDeadzone * DEADZONE_STEP;
    if (magnitude_sq > (uint32_t)(stickDeadzoneActual * stickDeadzoneActual)) {
        pad->stick_x = leftx / 0x100;
        int stick_y = -lefty / 0x100;
        pad->stick_y = stick_y == 128 ? 127 : stick_y;
    }

    magnitude_sq = (uint32_t)(rightx * rightx) + (uint32_t)(righty * righty);
    stickDeadzoneActual = configStickDeadzone * DEADZONE_STEP;
    if (magnitude_sq > (uint32_t)(stickDeadzoneActual * stickDeadzoneActual)) {
        pad->ext_stick_x = rightx / 0x100;
        int stick_y = -righty / 0x100;
        pad->ext_stick_y = stick_y == 128 ? 127 : stick_y;
    }
}

static void controller_sdl3_rumble_play(f32 strength, f32 length) {
    if (sdl_gp) {
        uint16_t scaled = (uint16_t)(strength * 0xFFFF);
        SDL_RumbleGamepad(sdl_gp, scaled, scaled, (u32)(length * 1000.0f));
    }
}

static void controller_sdl3_rumble_stop(void) {
    if (sdl_gp)
        SDL_RumbleGamepad(sdl_gp, 0, 0, 0);
}

static u32 controller_sdl3_rawkey(void) {
    if (last_joybutton != VK_INVALID) {
        const u32 ret = last_joybutton;
        last_joybutton = VK_INVALID;
        return ret;
    }

    for (int i = 0; i < MAX_MOUSEBUTTONS; ++i) {
        if (last_mouse & SDL_BUTTON_MASK(i)) {
            const u32 ret = VK_OFS_SDL_MOUSE + i;
            last_mouse = 0;
            return ret;
        }
    }
    return VK_INVALID;
}

static void controller_sdl3_shutdown(void) {
    if (sdl_gp) {
        SDL_CloseGamepad(sdl_gp);
        sdl_gp = NULL;
    }
    init_ok = false;
}

void controller_sdl3_event(SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        fprintf(stderr, "[sdl3] GAMEPAD_ADDED event (instance %u)\n",
                (unsigned)event->gdevice.which);
        if (sdl_gp == NULL)
            controller_sdl3_open_first();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (sdl_gp != NULL && SDL_GetGamepadID(sdl_gp) == event->gdevice.which) {
            fprintf(stderr, "[sdl3] GAMEPAD_REMOVED event (instance %u), closing\n",
                    (unsigned)event->gdevice.which);
            SDL_CloseGamepad(sdl_gp);
            sdl_gp = NULL;
        }
        break;
    default:
        break;
    }
}

struct ControllerAPI controller_sdl3 = {
    VK_BASE_SDL_GAMEPAD,
    controller_sdl3_init,
    controller_sdl3_read,
    controller_sdl3_rawkey,
    controller_sdl3_rumble_play,
    controller_sdl3_rumble_stop,
    controller_sdl3_bind,
    controller_sdl3_shutdown
};

#endif // CAPI_SDL3
