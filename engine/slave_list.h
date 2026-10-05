/* slave_list.h -- walk the master DSP's display list (see slave_list.c). */
#ifndef ENG_SLAVE_LIST_H
#define ENG_SLAVE_LIST_H
#include <stdint.h>
#include "geo_hw.h"

/* One polygon-RAM word (24-bit value, sign-extended or not), index & 0x7FFF. */
typedef uint32_t (*eng_word_fn)(int index);

/* STEREO: one eye of a pair, beside the game's camera. The eye sits at x = dx (view space, the game's units) and shares the camera's
 * zero-parallax plane z = zconv (an off-axis frustum): a point at depth zconv lands where the game's own camera puts it, nearer ones
 * move apart, farther ones together. In view space: x' = x - dx + dx * z / zconv. Only FULL-FRAME viewports see it (a radar's or a
 * mirror's sub-window stays the game's flat picture). The walk reports the focal length of the last such viewport (0 = none: no
 * world this frame), which a flat billboard needs for its own parallax. */
typedef struct {
    int32_t dx, zconv;      /* zconv <= 0: parallel (infinity at zero parallax) */
    float   focal;          /* out: pixels */
} eng_eye;

typedef struct {
    int head;               /* ENG_LIST_HEAD_SS22 (0x304) or ENG_LIST_HEAD_S22 (0x2FF) */
    int s22_objectflags;    /* System 22: the 0x10 record carries object flags */
    /* optional: the last viewport's projection, for callers that report it */
    int32_t *out_zoom_mant; int *out_zoom_shift; int32_t *out_vx, *out_vy;
    eng_eye *eye;           /* NULL = the game's own camera */
} eng_list_cfg;
#define ENG_LIST_HEAD_SS22 0x304
#define ENG_LIST_HEAD_S22  0x2FF

/* Returns the number of primitives walked. */
int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user);
#endif
