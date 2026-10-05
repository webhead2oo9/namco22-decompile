/*
 * eng_xr.h -- the game in a VR headset (engine/eng_xr.c): OpenXR, the picture on a big virtual screen in front of the player, in
 * stereo, and the motion controllers as the cabinet's light gun.
 *
 * The OpenXR loader is found at run time (libopenxr_loader.so.1, or openxr_loader.dll beside the .exe): a build needs no OpenXR
 * library, and without a runtime or a headset --vr says why and the game stays in its window.
 *
 * THE SCREEN is two quad layers at one place in the room (LOCAL space), one per eye (eyeVisibility LEFT / RIGHT), each eye's picture
 * drawn by the engine from that eye (ss22_set_stereo, engine/slave_list.h eng_eye). The runtime composites them at the headset's own
 * rate, so looking around is smooth whatever the game's 60 Hz. The VR page of the menu sets:
 *   Screen distance  D (m); the world's depth D * units_per_m shows ON the screen, nearer things stand out in front of it
 *   Screen size      100 % = the 4:3 picture spans 45 degrees (Time Crisis' own field of view: life size), wider with widescreen
 *   3D depth         the eyes' distance: 100 % = 64 mm at the game's scale, 0 = a flat screen
 *   Recenter         the screen straight in front of you again (also B / Y)
 * THE GUN (a light-gun game): the controller's aim ray hits the screen's plane; inside the 4:3 picture is where the gun points,
 * anywhere else is off-screen (a reload shot), as at the cabinet. Trigger = the trigger, grip = the pedal, A / X = coin.
 * THE MENU is drawn on the screen, in both eyes. The controllers open it with the menu button (Touch: the left one's; Index, WMR:
 * a stick click; Vive: the trackpad click); then the stick steps, trigger / A is OK, B / Y or the menu button go back.
 */
#ifndef ENG_XR_H
#define ENG_XR_H
#include <stdbool.h>
#include <stdint.h>
#include "eng_ui.h"

/* With the window's OpenGL context current: an OpenXR session that shares it. app = the game's name, units_per_m = the game's
 * view-space units in a metre, light_gun = the controllers aim. false = no VR (the reason is printed): the game stays in its window. */
bool eng_xr_start(const char *app, int32_t units_per_m, bool light_gun);
void eng_xr_stop(void);
bool eng_xr_running(void);                 /* the session is showing frames: draw the eyes, not the window's picture */
void eng_xr_poll(void);                    /* once per host frame, before the controls are read: the runtime's events, the controllers */

/* THE EYES' STEREO for ss22_set_stereo, from the settings: sep = the eyes' distance, zconv = the depth on the screen's plane (game units) */
void eng_xr_stereo(int32_t *sep, int32_t *zconv);
/* One headset frame: begin (true = draw both eyes now, w x h pixels each, into eng_xr_eye_target(e)), then end (always, after a
 * begin). The pictures' shape follows the Widescreen setting. */
bool eng_xr_frame_begin(int *w, int *h);
void eng_xr_eye_target(int eye);           /* 0 left, 1 right: that eye's picture is GL's draw framebuffer */
void eng_xr_frame_end(void);
void eng_xr_mirror(int x, int y, int w, int h, bool sharp);   /* the left eye into the window's rectangle (x, y from the top-left) */
bool eng_xr_read_eye(int eye);             /* that eye's last picture as GL's read framebuffer (screenshots); false = none */
void eng_xr_read_done(void);
/* THE MENU in the headset, inside a frame that renders (after eng_xr_frame_begin returned true): overlay_begin makes a clear w x h
 * overlay GL's draw framebuffer (the window's drawable size: the menu lays itself out in the window), the menu draws, overlay_end;
 * then eng_xr_overlay_draw() over each eye's picture lays it over the whole picture. Without an overlay this frame it does nothing. */
bool eng_xr_overlay_begin(int w, int h);
void eng_xr_overlay_end(void);
void eng_xr_overlay_draw(void);

/* THE GUN: where the controller points in the 4:3 picture, 0..1 each way; inside = on it. false = no tracked controller. */
bool eng_xr_gun(float *nx, float *ny, bool *inside);
enum { ENG_XR_TRIGGER = 1, ENG_XR_PEDAL = 2, ENG_XR_COIN = 4 };
unsigned eng_xr_buttons(void);             /* ENG_XR_* held */
void eng_xr_rumble(float amplitude, uint32_t ms);   /* the gun hand's controller (a recoil) */
/* The controllers' menu steps since the last eng_xr_poll, one per call, 0 = none: 'm' = open the menu (its button, while it is
 * closed), else eng_ui_nav's letters (u d l r o b) while it is open */
char eng_xr_menu_key(void);
#define ENG_XR_MENU_HINT "Menu: the controller's menu button (Index / WMR: click the stick)"

const eng_ui_page *eng_xr_page(void);      /* the menu's VR page */
#endif
