/* eng_pad.h -- what a game pad needs that no game's own table has (header only: every game includes it, no build change).
 *
 * THE STEAM DECK HAS NO KEYBOARD. Esc, F2 and 9 are out of reach, so every game gives the pad a way to the menu:
 *   R3 (right-stick click)        opens and closes it (each game's event loop)
 *   Start held for one second     the Deck's Menu button; a TAP is still the game's own Start (eng_pad_start_hold)
 * and the menu carries what the keyboard shortcuts do (Test mode, Service) -- see the File page of each game.
 */
#ifndef ENG_PAD_H
#define ENG_PAD_H
#include <SDL2/SDL.h>
#include <stdbool.h>

#define ENG_PAD_MENU_HINT "Menu: R3 (right stick click) or hold Start"

/* is this button held on ANY attached game controller? (the ones the game opened) */
static inline bool eng_pad_held(SDL_GameControllerButton b)
{
    for (int j = 0, n = SDL_NumJoysticks(); j < n; j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_GameController *c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
        if (c && SDL_GameControllerGetButton(c, b)) return true;
    }
    return false;
}

/* call once per frame: true ONCE when Start has been held for `frames` frames in a row; false again until it is released */
static inline bool eng_pad_start_hold(int frames)
{
    static int n; static bool fired;
    if (!eng_pad_held(SDL_CONTROLLER_BUTTON_START)) { n = 0; fired = false; return false; }
    if (fired) return false;
    if (++n >= frames) { fired = true; return true; }
    return false;
}

/* is any game controller attached? (for the on-screen hint) */
static inline bool eng_pad_present(void)
{
    for (int j = 0, n = SDL_NumJoysticks(); j < n; j++) if (SDL_IsGameController(j)) return true;
    return false;
}

/* A SENSOR OF THE MACHINE ITSELF that Linux lists as a joystick: not a game device, so never one of the game's. The Steam Frame's
 * als31300 (a Hall-effect sensor in the headset) is one -- on the I2C bus, four axes resting off their centre, no buttons -- and
 * taken for a wheel it held Prop Cycle's handlebar off centre. A game device comes over USB or Bluetooth, or has buttons. Ask on an
 * opened joystick (the buttons need it) and close it again on a yes. */
static inline bool eng_pad_is_sensor(SDL_Joystick *js)
{
    const SDL_JoystickGUID g = SDL_JoystickGetGUID(js);                         /* SDL's first word: the bus (linux/input.h BUS_*) */
    return (g.data[0] | g.data[1] << 8) == 0x18 && SDL_JoystickNumButtons(js) == 0;   /* BUS_I2C */
}
#endif
