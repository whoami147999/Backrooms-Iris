// Backrooms Iris - SDL2 GameController handling, shared by PC and Vita
// (on Vita the built-in buttons/sticks show up as an SDL game controller).
#pragma once
#include <SDL.h>

#include "platform.h"

namespace iris {

void gamepad_init();
void gamepad_event(const SDL_Event& e, InputState& in);
void gamepad_poll(InputState& in, double now); // adds stick/button state into `in`
bool gamepad_connected();
void gamepad_shutdown();

} // namespace iris
