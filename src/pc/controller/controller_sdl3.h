#ifndef CONTROLLER_SDL3_H
#define CONTROLLER_SDL3_H

#include "controller_api.h"
#include <SDL3/SDL.h>

// Mirrors controller_sdl.h's base (controller_sdl.h itself can't be included
// here because it pulls in <SDL2/SDL.h>, which would clash with SDL3).
#ifndef VK_BASE_SDL_GAMEPAD
#define VK_BASE_SDL_GAMEPAD 0x1000
#endif

extern struct ControllerAPI controller_sdl3;

// Forward gamepad add/remove events (already pumped by the Foundation host's
// SDL event loop) so the backend can open/close the gamepad.
extern void controller_sdl3_event(SDL_Event *event);

#endif
