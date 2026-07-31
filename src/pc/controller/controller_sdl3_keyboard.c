// SDL3 keyboard backend for the Foundation host, ported from gfx_sdl2.c.
//
// sm64ex's controller_keyboard backend expects DOS scancode-set-1 values, but
// SDL reports USB usage-page scancodes, so we invert windows_scancode_table the
// same way gfx_sdl2.c does. In upstream sm64ex this logic lives in the SDL
// window manager (gfx_sdl2.c); the Foundation port has no SDL-based window
// manager (Foundation owns the window + event loop), so it is kept here next to
// the gamepad backend (controller_sdl3.c). Events arrive via the host's
// Examples_PollEvents callback (OnSdlEvent), which forwards to
// keyboard_sdl3_event().
#ifdef CAPI_SDL3

#include <stddef.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "controller_api.h"
#include "controller_sdl3_keyboard.h"

// Matches controller_sdl3.c — mouse buttons share the gamepad VK space.
#ifndef VK_BASE_SDL_GAMEPAD
#define VK_BASE_SDL_GAMEPAD 0x1000
#endif
#define VK_OFS_SDL_MOUSE 0x0100
#define VK_BASE_SDL_MOUSE (VK_BASE_SDL_GAMEPAD + VK_OFS_SDL_MOUSE)

static int s_inverted_scancode_table[512];
static bool s_scancode_table_ready = false;

// Keep in lockstep with gfx_sdl2.c — DOS set-1 index -> SDL_Scancode.
static const SDL_Scancode windows_scancode_table[] = {
  /*  0                        1                            2                         3                            4                     5                            6                            7  */
  /*  8                        9                            A                         B                            C                     D                            E                            F  */
  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_ESCAPE,         SDL_SCANCODE_1,           SDL_SCANCODE_2,              SDL_SCANCODE_3,       SDL_SCANCODE_4,              SDL_SCANCODE_5,              SDL_SCANCODE_6,          /* 0 */
  SDL_SCANCODE_7,              SDL_SCANCODE_8,              SDL_SCANCODE_9,           SDL_SCANCODE_0,              SDL_SCANCODE_MINUS,   SDL_SCANCODE_EQUALS,         SDL_SCANCODE_BACKSPACE,      SDL_SCANCODE_TAB,        /* 0 */

  SDL_SCANCODE_Q,              SDL_SCANCODE_W,              SDL_SCANCODE_E,           SDL_SCANCODE_R,              SDL_SCANCODE_T,       SDL_SCANCODE_Y,              SDL_SCANCODE_U,              SDL_SCANCODE_I,          /* 1 */
  SDL_SCANCODE_O,              SDL_SCANCODE_P,              SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,   SDL_SCANCODE_RETURN,  SDL_SCANCODE_LCTRL,          SDL_SCANCODE_A,              SDL_SCANCODE_S,          /* 1 */

  SDL_SCANCODE_D,              SDL_SCANCODE_F,              SDL_SCANCODE_G,           SDL_SCANCODE_H,              SDL_SCANCODE_J,       SDL_SCANCODE_K,              SDL_SCANCODE_L,              SDL_SCANCODE_SEMICOLON,  /* 2 */
  SDL_SCANCODE_APOSTROPHE,     SDL_SCANCODE_GRAVE,          SDL_SCANCODE_LSHIFT,      SDL_SCANCODE_BACKSLASH,      SDL_SCANCODE_Z,       SDL_SCANCODE_X,              SDL_SCANCODE_C,              SDL_SCANCODE_V,          /* 2 */

  SDL_SCANCODE_B,              SDL_SCANCODE_N,              SDL_SCANCODE_M,           SDL_SCANCODE_COMMA,          SDL_SCANCODE_PERIOD,  SDL_SCANCODE_SLASH,          SDL_SCANCODE_RSHIFT,         SDL_SCANCODE_PRINTSCREEN,/* 3 */
  SDL_SCANCODE_LALT,           SDL_SCANCODE_SPACE,          SDL_SCANCODE_CAPSLOCK,    SDL_SCANCODE_F1,             SDL_SCANCODE_F2,      SDL_SCANCODE_F3,             SDL_SCANCODE_F4,             SDL_SCANCODE_F5,         /* 3 */

  SDL_SCANCODE_F6,             SDL_SCANCODE_F7,             SDL_SCANCODE_F8,          SDL_SCANCODE_F9,             SDL_SCANCODE_F10,     SDL_SCANCODE_NUMLOCKCLEAR,   SDL_SCANCODE_SCROLLLOCK,     SDL_SCANCODE_HOME,       /* 4 */
  SDL_SCANCODE_UP,             SDL_SCANCODE_PAGEUP,         SDL_SCANCODE_KP_MINUS,    SDL_SCANCODE_LEFT,           SDL_SCANCODE_KP_5,    SDL_SCANCODE_RIGHT,          SDL_SCANCODE_KP_PLUS,        SDL_SCANCODE_END,        /* 4 */

  SDL_SCANCODE_DOWN,           SDL_SCANCODE_PAGEDOWN,       SDL_SCANCODE_INSERT,      SDL_SCANCODE_DELETE,         SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,        /* 5 */
  SDL_SCANCODE_F12,            SDL_SCANCODE_PAUSE,          SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_LGUI,           SDL_SCANCODE_RGUI,    SDL_SCANCODE_APPLICATION,    SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 5 */

  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_F13,     SDL_SCANCODE_F14,            SDL_SCANCODE_F15,            SDL_SCANCODE_F16,        /* 6 */
  SDL_SCANCODE_F17,            SDL_SCANCODE_F18,            SDL_SCANCODE_F19,         SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 6 */

  SDL_SCANCODE_INTERNATIONAL2, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 7 */
  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_INTERNATIONAL4, SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_INTERNATIONAL5, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN     /* 7 */
};

static const SDL_Scancode scancode_rmapping_extended[][2] = {
    { SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_RETURN },
    { SDL_SCANCODE_RALT,     SDL_SCANCODE_LALT },
    { SDL_SCANCODE_RCTRL,    SDL_SCANCODE_LCTRL },
    { SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_SLASH },
};

static const SDL_Scancode scancode_rmapping_nonextended[][2] = {
    { SDL_SCANCODE_KP_7,      SDL_SCANCODE_HOME },
    { SDL_SCANCODE_KP_8,      SDL_SCANCODE_UP },
    { SDL_SCANCODE_KP_9,      SDL_SCANCODE_PAGEUP },
    { SDL_SCANCODE_KP_4,      SDL_SCANCODE_LEFT },
    { SDL_SCANCODE_KP_6,      SDL_SCANCODE_RIGHT },
    { SDL_SCANCODE_KP_1,      SDL_SCANCODE_END },
    { SDL_SCANCODE_KP_2,      SDL_SCANCODE_DOWN },
    { SDL_SCANCODE_KP_3,      SDL_SCANCODE_PAGEDOWN },
    { SDL_SCANCODE_KP_0,      SDL_SCANCODE_INSERT },
    { SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_DELETE },
    { SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_PRINTSCREEN },
};

void keyboard_sdl3_init(void) {
    if (s_scancode_table_ready)
        return;
    for (size_t i = 0; i < sizeof(windows_scancode_table) / sizeof(SDL_Scancode); ++i)
        s_inverted_scancode_table[windows_scancode_table[i]] = (int)i;
    for (size_t i = 0; i < sizeof(scancode_rmapping_extended) / sizeof(scancode_rmapping_extended[0]); ++i)
        s_inverted_scancode_table[scancode_rmapping_extended[i][0]] =
            s_inverted_scancode_table[scancode_rmapping_extended[i][1]] + 0x100;
    for (size_t i = 0; i < sizeof(scancode_rmapping_nonextended) / sizeof(scancode_rmapping_nonextended[0]); ++i) {
        s_inverted_scancode_table[scancode_rmapping_nonextended[i][0]] =
            s_inverted_scancode_table[scancode_rmapping_nonextended[i][1]];
        s_inverted_scancode_table[scancode_rmapping_nonextended[i][1]] += 0x100;
    }
    s_scancode_table_ready = true;
}

static int translate_scancode(int scancode) {
    if (scancode < 512)
        return s_inverted_scancode_table[scancode];
    return 0;
}

static kb_callback_t s_on_key_down = NULL;
static kb_callback_t s_on_key_up = NULL;
static void (*s_on_all_keys_up)(void) = NULL;

void keyboard_sdl3_set_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up,
                                 void (*on_all_keys_up)(void)) {
    s_on_key_down = on_key_down;
    s_on_key_up = on_key_up;
    s_on_all_keys_up = on_all_keys_up;
}

void keyboard_sdl3_event(SDL_Event *event) {
    keyboard_sdl3_init();
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (s_on_key_down)
            s_on_key_down(translate_scancode((int)event->key.scancode));
        break;
    case SDL_EVENT_KEY_UP:
        if (s_on_key_up)
            s_on_key_up(translate_scancode((int)event->key.scancode));
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_QUIT:
        if (s_on_all_keys_up)
            s_on_all_keys_up();   // don't let Mario keep running with no focus
        break;
    default:
        break;
    }
}

// Prefer a short HUD-friendly label (SDL_GetScancodeName is often verbose).
static const char *shorten_scancode_name(const char *name) {
    if (!name || !name[0])
        return "?";
    if (SDL_strcasecmp(name, "Left Shift") == 0)  return "LShift";
    if (SDL_strcasecmp(name, "Right Shift") == 0) return "RShift";
    if (SDL_strcasecmp(name, "Left Ctrl") == 0)   return "LCtrl";
    if (SDL_strcasecmp(name, "Right Ctrl") == 0)  return "RCtrl";
    if (SDL_strcasecmp(name, "Left Alt") == 0)    return "LAlt";
    if (SDL_strcasecmp(name, "Right Alt") == 0)   return "RAlt";
    if (SDL_strcasecmp(name, "Left") == 0)        return "Left";
    if (SDL_strcasecmp(name, "Right") == 0)       return "Right";
    if (SDL_strcasecmp(name, "Up") == 0)          return "Up";
    if (SDL_strcasecmp(name, "Down") == 0)        return "Down";
    if (SDL_strcasecmp(name, "Return") == 0)      return "Enter";
    if (SDL_strcasecmp(name, "Escape") == 0)      return "Esc";
    return name;
}

const char *keyboard_sdl3_vk_name(unsigned int vk) {
    static char s_hex_ring[8][8];
    static int s_hex_i = 0;
    keyboard_sdl3_init();

    if (vk == VK_INVALID)
        return "-";

    // Keyboard (DOS scancode, including E0-extended as 0x1xx).
    if (vk < VK_BASE_SDL_GAMEPAD) {
        for (int i = 0; i < 512; ++i) {
            if (s_inverted_scancode_table[i] == (int)vk) {
                const char *name = SDL_GetScancodeName((SDL_Scancode)i);
                if (name && name[0])
                    return shorten_scancode_name(name);
                break;
            }
        }
        const unsigned int idx = vk & 0xFF;
        if (idx < sizeof(windows_scancode_table) / sizeof(SDL_Scancode)) {
            const char *name = SDL_GetScancodeName(windows_scancode_table[idx]);
            if (name && name[0])
                return shorten_scancode_name(name);
        }
        snprintf(s_hex_ring[s_hex_i], sizeof(s_hex_ring[0]), "%03X", vk);
        const char *out = s_hex_ring[s_hex_i];
        s_hex_i = (s_hex_i + 1) % 8;
        return out;
    }

    // Mouse buttons live in the gamepad VK space at +0x100.
    if (vk >= VK_BASE_SDL_MOUSE && vk < VK_BASE_SDL_MOUSE + 8) {
        static const char *const mouseNames[] = {
            "MLeft", "MMid", "MRight", "MX1", "MX2", "M5", "M6", "M7"
        };
        return mouseNames[vk - VK_BASE_SDL_MOUSE];
    }

    if (vk == VK_LTRIGGER) return "LTrig";
    if (vk == VK_RTRIGGER) return "RTrig";

    // Gamepad button index (SDL_GamepadButton enum values in the default binds).
    if (vk >= VK_BASE_SDL_GAMEPAD && vk < VK_BASE_SDL_MOUSE) {
        const int btn = (int)(vk - VK_BASE_SDL_GAMEPAD);
        const char *name = SDL_GetGamepadStringForButton((SDL_GamepadButton)btn);
        if (name && name[0])
            return name;
    }

    snprintf(s_hex_ring[s_hex_i], sizeof(s_hex_ring[0]), "%03X", vk);
    const char *out = s_hex_ring[s_hex_i];
    s_hex_i = (s_hex_i + 1) % 8;
    return out;
}

#endif // CAPI_SDL3
