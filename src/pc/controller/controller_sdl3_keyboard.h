#ifndef CONTROLLER_SDL3_KEYBOARD_H
#define CONTROLLER_SDL3_KEYBOARD_H

#include <SDL3/SDL.h>
#include "../gfx/gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the SDL3-scancode -> DOS-scancode inversion table. Idempotent; safe to
// call before the first event (also called lazily by keyboard_sdl3_event()).
void keyboard_sdl3_init(void);

// Register the keyboard controller callbacks supplied via
// GfxWindowManagerAPI::set_keyboard_callbacks (i.e. controller_keyboard's
// keyboard_on_key_down / keyboard_on_key_up / keyboard_on_all_keys_up).
void keyboard_sdl3_set_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up,
                                 void (*on_all_keys_up)(void));

// Feed one SDL event (already pumped by the Foundation host's event loop) into
// the keyboard backend: it translates the SDL scancode to the DOS scancode
// sm64ex expects and dispatches it to the registered callbacks.
void keyboard_sdl3_event(SDL_Event *event);

// Human-readable label for a configKey* virtual key (keyboard / gamepad /
// mouse). Returns a stable pointer valid until the next call that needs the
// hex fallback buffer; safe to use for a single HUD frame's worth of labels.
const char *keyboard_sdl3_vk_name(unsigned int vk);

#ifdef __cplusplus
}
#endif

#endif // CONTROLLER_SDL3_KEYBOARD_H
