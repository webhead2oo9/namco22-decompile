/*
 * eng_xr.h -- the game in a VR headset (engine/eng_xr.c): OpenXR, the picture on a big virtual screen in front of the player, in
 * stereo, and the motion controllers as the cabinet's light gun or as a game pad. Every game's host uses it the same way.
 *
 * The OpenXR loader is found at run time (libopenxr_loader.so.1, or openxr_loader.dll beside the .exe; on the Steam Frame, SteamVR's
 * own): a build needs no OpenXR library, and without a runtime or a headset --vr says why and the game stays in its window.
 *
 * THE SCREEN is two quad layers at one place in the room (LOCAL space), one per eye (eyeVisibility LEFT / RIGHT), each eye's picture
 * drawn by the game's renderer from that eye (eng_xr_stereo: engine/slave_list.h eng_eye). The runtime composites them at the
 * headset's own rate, so looking around is smooth whatever the game's 60 Hz. The VR settings (any menu shows them: eng_xr_rows):
 *   View             (a game that draws one) the screen, or INSIDE the game's world -- below
 *   Outside the game's view  (the same games) how bright the world is where the game's camera does not look, 80 % by default
 *   Screen distance  D (m); the world's depth D * units_per_m shows ON the screen, nearer things stand out in front of it
 *   Screen size      100 % = the 4:3 picture spans the game's own field of view (life size), wider with widescreen
 *   3D depth         the eyes' distance: 100 % = 64 mm at the game's scale, 0 = a flat screen
 *   Recenter         the screen straight in front of you again
 * INSIDE (View: Inside the game's world): the game's camera is your head where it was at the last recenter (facing that way, the
 * horizon level), and the headset looks round the game's world from there -- each eye drawn by the game's renderer from where that
 * eye is, through the headset's own field of view (engine/slave_list.h eng_inside), a projection layer; your head's every move
 * moves the eye (3D depth scales them with the eyes' distance: the world's scale). What is not the world -- the HUD's sprites, the
 * text layer, sub-window viewports -- stays on the screen, which is clear wherever the game drew nothing there: a quad layer over
 * the world, at the screen's distance and size. The light gun aims at the world: the host finds what the ray meets and where the
 * game's own camera sees it (eng_xr_host.inside_aim). The game itself pins every shot to its camera's picture, so what lies outside
 * that picture cannot be shot: it is drawn dimmer (Outside the game's view). The headset gets a picture every one of its frames, the
 * game's frame again from where the head is now between the game's own (the host's pacing; eng_xr_next_frame). The window shows
 * the left eye with the screen over it.
 * THE CONTROLLERS. A light-gun game: the aim ray hits the screen's plane, inside the 4:3 picture is where the gun points, anywhere else
 * is off-screen (a reload shot); trigger = the trigger, grip = the pedal, A / X = coin, B / Y = recenter. Any other game: the two
 * controllers are one game pad (eng_xr_get_pad): the sticks, the triggers, the grips as the shoulders, A B (right) X Y (left), a left
 * stick click = Back (coin), a right stick click = Start. The Steam Frame's controllers (XR_VALVE_frame_controller_interaction) are
 * that pad button for button: A B X Y on the right, the d-pad on the left, Menu = Start, View = the menu, the bumpers = the shoulders.
 * THE MENU is drawn on the screen, in both eyes. The menu button opens it (Touch: the left controller's; Index, WMR: a right stick click;
 * Vive: a right trackpad click; the Frame: View); then the controller is a pointer (the trigger clicks), the stick steps, A is OK, B / Y
 * go back.
 * TESTS: ENG_XRTIME=1 logs where a headset frame's time goes (and the headset's rate) every 600 frames; ENG_XRPAD=1 logs every change
 * of what the controllers give the game; ENG_XR_INSIDE_PX=<px> holds an Inside eye's picture to that size (1600 by default);
 * ENG_XR_MIRROR=2 shows both eyes side by side in the window (a recording of the whole view), not the left one.
 */
#ifndef ENG_XR_H
#define ENG_XR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "slave_list.h"

/* THE HOST'S SIDE: what eng_xr needs from the program it runs in. eng_xr.c links nothing of any host (the Super 22 games' eng_ui.c,
 * Rave Racer's rr_ui.c, Prop Cycle's ui_menu.c): each fills this in. */
typedef struct {
    const char *app;                         /* the game's name (the runtime shows it) */
    int32_t units_per_m;                     /* the game's view-space units in a metre (the stereo's scale); 0 = 15000 */
    float   hfov_deg;                        /* the game's own horizontal field of view across its 4:3 picture ("Screen size 100 %"); 0 = 45 */
    bool    light_gun;                       /* the controllers aim at the screen (eng_xr_gun); else they are a pad (eng_xr_get_pad) */
    bool  (*wide)(void);                     /* the picture is widescreen (16:9); NULL = never */
    bool  (*menu_open)(void);                /* the host's menu is open: the controllers drive it, not the game */
    int   (*cfg_get)(const char *key, int def);   /* the host's settings file, for the VR settings; NULL = defaults, not kept */
    void  (*cfg_set)(const char *key, int v);
    bool    inside;                          /* the host draws an Inside view (eng_xr_present's ENG_XR_INSIDE and ENG_XR_SCREEN) */
    /* INSIDE, a light-gun game: the gun's ray (an eng_inside looking along it, in the game camera's space) -> where the game's camera
     * sees what it meets, in the 4:3 picture, 0..1 each way; *on = on it. false = this frame has no world (the screen's plane aims). */
    bool  (*inside_aim)(eng_inside *ray, float *nx, float *ny, bool *on);
} eng_xr_host;

/* With the window's OpenGL context current: an OpenXR session that shares it (*host is copied). false = no VR (the reason is printed):
 * the game stays in its window. On Linux the window must be an X11 one (SDL's x11 driver: set it before SDL starts its video). */
bool eng_xr_start(const eng_xr_host *host);
void eng_xr_stop(void);
bool eng_xr_running(void);                 /* the session is showing frames: eng_xr_present draws the window */
/* Once per host frame, BEFORE the host reads its events: the runtime's events and the controllers. While the menu is open the
 * controller's pointer reaches it as SDL mouse events (motion, left button), pushed onto SDL's queue for the host's own loop. */
void eng_xr_poll(void);

/* THE EYES' STEREO for the game's renderer, from the settings: sep = the eyes' distance, zconv = the depth on the screen's plane, both
 * in the game's view-space units (engine/slave_list.h eng_eye: eye 0 at dx = -sep / 2, eye 1 at +sep / 2), and focal_max = the
 * game's life-size lens (pixels) on the headset's screen -- shorter on a bigger screen. A longer lens, a zoom, has its eyes moved in
 * (eng_eye.focal_max), so its far world goes no farther than the game's own lens puts it: infinity at 3D depth 100 %. */
void eng_xr_stereo(int32_t *sep, int32_t *zconv, float *focal_max);

/* ONE HOST FRAME IN THE HEADSET. draw_eye(eye, w, h, u) draws the game's picture for that eye (0 left, 1 right) into the bound eye
 * picture, w x h pixels, the viewport already set; draw_menu(u), when menu_visible, draws the host's menu (Nuklear) once, into an
 * overlay the size of the window's drawable, which lies over both eyes' pictures with the pointer's dot on it. The window then shows
 * the left eye, letterboxed: *mirror is that rectangle (drawable pixels, from the top-left). false = no session: draw the window.
 * INSIDE (eng_xr_inside): draw_eye gets ENG_XR_INSIDE + 0 and + 1 instead -- the world from each eye (eng_xr_inside_eye) -- then
 * ENG_XR_SCREEN, the screen's picture over a clear one, its colour premultiplied by its alpha; the menu goes on the screen. */
enum { ENG_XR_INSIDE = 2, ENG_XR_SCREEN = 4 };
bool eng_xr_present(SDL_Window *win, void (*draw_eye)(int eye, int w, int h, void *u), void (*draw_menu)(void *u),
                    bool menu_visible, void *u, bool sharp, SDL_Rect *mirror);
void eng_xr_eye_size(int *w, int *h);      /* the eye pictures' size (0 x 0 before the first; Inside, its eyes') */
bool eng_xr_read_eye(int eye);             /* that eye's last picture as GL's read framebuffer (screenshots); false = none */
void eng_xr_read_done(void);
bool eng_xr_inside(void);                  /* the frames are Inside ones (View: Inside, and the host draws them) */
void eng_xr_inside_eye(int eye, eng_inside *v);   /* this frame's eye 0 / 1 in the game camera's space (during eng_xr_present) */
/* THE HEADSET'S NEXT FRAME: *ns = nanoseconds until the runtime is expected to want its next picture (xrWaitFrame's next return,
 * estimated from the last one and the headset's period; <= 0 = already). false = no session or no frame yet. */
bool eng_xr_next_frame(int64_t *ns);

/* THE GUN (a light-gun game): where the controller points in the 4:3 picture, 0..1 each way; inside = on it. false = not tracked. */
bool eng_xr_gun(float *nx, float *ny, bool *inside);
enum { ENG_XR_TRIGGER = 1, ENG_XR_PEDAL = 2, ENG_XR_COIN = 4 };
unsigned eng_xr_buttons(void);             /* ENG_XR_* held (a light-gun game) */
void eng_xr_rumble(float amplitude, uint32_t ms);   /* the gun hand's controller (a recoil); a pad game: both */

/* THE PAD (any other game): the two controllers in a game pad's layout, the values SDL's GameController gives -- sticks -32768..32767
 * (y down), triggers 0..32767, buttons 1u << SDL_CONTROLLER_BUTTON_*. false = no pad now (no session, not focused, a light-gun game,
 * or the menu has the controllers). */
typedef struct { int16_t axis[SDL_CONTROLLER_AXIS_MAX]; uint32_t buttons; } eng_xr_pad;
bool eng_xr_get_pad(eng_xr_pad *p);

/* THE MENU from the controllers: the steps since the last eng_xr_poll, one per call, 0 = none: 'm' = open the menu (its button, while
 * it is closed), else (while it is open) u d l r = the stick, o = OK (A), b = back (B / Y / the menu button) */
char eng_xr_menu_key(void);
#define ENG_XR_MENU_HINT "Menu: the controller's menu button (Index / WMR: click the right stick)"

/* THE VR SETTINGS, for any menu: rows of a label and either a value (Left / Right change it, dir -1 / +1) or an action (dir 0); then
 * the notes, a few short lines */
int  eng_xr_rows(void);
float eng_xr_outside(void);                /* Inside: the brightness of what the game's camera does not see, 0..1 (1 = as bright) */
bool eng_xr_row_value(int r);
void eng_xr_row_text(int r, char *label, size_t ln, char *value, size_t vn);
void eng_xr_row_change(int r, int dir);
void eng_xr_notes(void (*line)(const char *fmt, ...));
#endif
