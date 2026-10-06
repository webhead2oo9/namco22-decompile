/* post_gl.h -- the final per-channel colour stage over the whole frame (engine/post_gl.c). */
#ifndef ENG_POST_GL_H
#define ENG_POST_GL_H
#include <stdint.h>
/* Replace every pixel of the vw x vh framebuffer (the current viewport origin)
 * by lut[channel][value]. */
void eng_post_lut(const uint8_t lut[3][256], int vw, int vh);
/* The same over a picture whose colour is premultiplied by its alpha (a layer with see-through parts: the VR Inside view's screen,
 * engine/eng_xr.h): each pixel's colour unpremultiplied, through the table, premultiplied again; the alpha kept. */
void eng_post_lut_premul(const uint8_t lut[3][256], int vw, int vh);
#endif
