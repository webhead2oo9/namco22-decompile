/*
 * rr_gl.h -- Rave Racer's picture through the shared engine's OpenGL
 * pipeline (src/rr_gl.c). The software renderer (rr_video.c) is the oracle.
 */
#ifndef RR_GL_H
#define RR_GL_H
#include <stdbool.h>
#include <stdint.h>

bool rr_gl_init(const char *rom_dir);          /* texture ROMs, gamma PROMs, point data */
void rr_gl_context_attributes(void);           /* SDL_GL attributes, before creating the context */
bool rr_gl_open_headless(int w, int h);        /* an offscreen w x h context for --gl runs */
void rr_gl_prepare(bool slave_active);         /* once per screen update: walk + sort */
void rr_gl_draw(int vw, int vh);               /* draw the prepared frame, vw x vh pixels */
bool rr_gl_write_ppm(const char *path, int vw, int vh);
int  rr_gl_quads(void);                        /* quads in the prepared frame */
/* STEREO (a VR headset, engine/eng_xr.h): from the next prepare on, two eyes sep apart (the game's view-space units) converging at
 * depth zconv, a lens longer than focal_max pixels moving them in (engine/slave_list.h eng_eye; 0 = no limit); sep 0 = the game's one
 * camera. Each eye's quads come from their own walk of the list; rr_gl_draw_eye draws one. */
void  rr_gl_set_stereo(int32_t sep, int32_t zconv, float focal_max);
bool  rr_gl_stereo_frame(void);               /* the prepared frame has two eyes */
float rr_gl_focal(void);                      /* its full-frame viewport's focal length, pixels, capped at focal_max (0 = none) */
void  rr_gl_draw_eye(int eye, int vw, int vh); /* 0 left, 1 right (the one camera's picture when not stereo) */
#endif
