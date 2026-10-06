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
 * mirror's sub-window stays the game's flat picture).
 *   A LONG LENS: on screen a point's disparity is focal * dx * (1/zconv - 1/z), so it grows with the camera's focal length -- a
 * telephoto attract shot (Tokyo Wars' reach 9x the battle view's) would put the far world past infinity and make the eyes diverge.
 * focal_max is the game's own lens on the headset's screen (eng_xr_stereo); a viewport with a longer lens gets its eyes moved in by
 * focal_max / focal, so its far world goes no farther than the game's own lens puts it (it flattens, as a zoom does). At 3D depth
 * 100 % that is infinity -- the eyes' own separation; a deeper setting scales every lens alike, the player's choice. The
 * zero-parallax plane stays put.
 *   The walk reports the last such viewport's focal length, capped at focal_max -- what the parallax follows, so a flat billboard's
 * own shift, dx * focal * (1/zconv - 1/z), matches the world's (0 = none: no world this frame). */
typedef struct {
    int32_t dx, zconv;      /* zconv <= 0: parallel (infinity at zero parallax) */
    float   focal_max;      /* in: pixels; 0 = no limit */
    float   focal;          /* out: pixels */
} eng_eye;
/* One object's view (its view * object matrix and view-space translation, geo_hw.h) as eye e sees it through a viewport of focal
 * length `focal` (pixels): the shear above. The caller applies it only to a full-frame viewport's objects. A renderer with its own
 * walker (Prop Cycle's) uses it as eng_walk_list does. */
void eng_eye_view(eng_eye *e, geo_view *gv, float focal);
/* ENG_FOV_PROBE=<k>[:<yaw>] (tests, mono): a full-frame viewport's objects through a k times wider lens, turned yaw degrees
 * (slave_list.c); eng_fov_probe_k() > 0 = on */
double eng_fov_probe_k(void);
void eng_fov_probe(geo_view *gv);

/* INSIDE: an eye anywhere around the game's camera -- a VR headset's, looking round the game's world from where the camera is
 * (engine/eng_xr.h, View: Inside). Its place p and its axes r (rows: its right, its up, its forward) are in the camera's view
 * space (x right, y up, z forward; the game's units), so a point v of that space is r (v - p) to the eye; its picture is the
 * frustum tl .. tr (tangents, the left one negative) by td .. tu (the bottom one negative) laid over the 640 x 480 picture, in
 * whatever pixels it is drawn at. Only FULL-FRAME viewports are the world: every other one (a radar's, a mirror's, a sub-window)
 * is left out of an Inside walk -- it is the screen's, drawn from the game's own camera. The geometry stage needs nothing new:
 * the eye's turn, its place and the frustum's stretch are all one affine map of the object's view (eng_inside_view), and the
 * back faces, the near plane at z = 1, the guard band and the depth order all follow from it as they did from the camera. The
 * lighting keeps the camera's own matrix (geo_hw.h have_lm).
 *   The walk reports the camera's own lens and centre (the last full-frame viewport's): what the game's light gun sees through. */
typedef struct {
    double  r[3][3], p[3];  /* in: the eye in the camera's view space */
    double  tl, tr, tu, td; /* in: its frustum's tangents */
    float   focal;          /* out: the full-frame viewport's lens, pixels (0 = no world this frame) */
    int32_t vx, vy;         /* out: its centre's offset from (320, 240) */
} eng_inside;
void eng_inside_view(const eng_inside *in, geo_view *gv, float focal);

typedef struct {
    int head;               /* ENG_LIST_HEAD_SS22 (0x304) or ENG_LIST_HEAD_S22 (0x2FF) */
    int s22_objectflags;    /* System 22: the 0x10 record carries object flags */
    /* optional: the last viewport's projection, for callers that report it */
    int32_t *out_zoom_mant; int *out_zoom_shift; int32_t *out_vx, *out_vy;
    eng_eye *eye;           /* NULL = the game's own camera */
    eng_inside *inside;     /* an Inside eye instead (eye is then ignored); NULL = none */
} eng_list_cfg;
#define ENG_LIST_HEAD_SS22 0x304
#define ENG_LIST_HEAD_S22  0x2FF

/* Returns the number of primitives walked. */
int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user);
#endif
