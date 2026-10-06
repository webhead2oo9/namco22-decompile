/*
 * slave_list.c -- the SLAVE DSP's job: walk the master DSP's display list in
 * polygon RAM and hand every primitive to the geometry stage (geo_hw.c).
 *
 * One walker for both boards (namcos22_v.cpp simulate_slavedsp). Record
 * layout, from tools/pc_geo_fixed.py (byte-exact vs MAME):
 *   len 0x15  bb0003  viewport: zoom = dspfloat(src[6]),
 *                     view[row][col] = q15(src[0x0c + col*3 + row])
 *   len 0x10  233002  object shift / cz adjust (System 22: + object flags)
 *   len 0x0a  300000  view transform: view[row][col] = q15(src[1+col*3+row])
 *   len 0x0d  200002  primitive (code 0x5 or >= 0x45):
 *                     m[row][col] = q15(src[1+col*3+row])
 *                     t = sext24(src[0xa..0xc])
 *                     combined = (m . view) >> 15, t' = (t . view) >> 15
 * Moved here from src/framedump.c unchanged, apart from the board settings.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>                                  /* sscanf: ENG_FOV_PROBE */
#include <stdlib.h>                                 /* getenv, atof: ENG_FOV_PROBE */
#include <string.h>
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"

static int32_t sext_n(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return (int32_t)((v ^ m) - m);
}

static int32_t q15v(uint32_t v) { return sext_n(v & 0xffff, 16); }

/* namcos22_v.cpp:699 dspfloat, as a float (the clip window needs the real
 * value, not the mantissa/shift pair the projection uses). */
static float dspfloatf(uint32_t v)
{
    float mant = (float)sext_n(v & 0xffff, 16);
    int exp = (int)((v >> 16) & 0x3f);
    while (exp < 0x2e) { mant /= 2.0f; exp++; }
    return mant;
}

/* GeoFixed._reflectq: mirror the view by negating a COLUMN of the matrix. */
static void reflectq(int32_t m[3][3], int reflection)
{
    if (reflection & 0x10) for (int r = 0; r < 3; r++) m[r][0] = -m[r][0];
    if (reflection & 0x20) for (int r = 0; r < 3; r++) m[r][1] = -m[r][1];
}

static int32_t rdiv(int64_t a, int64_t b) { return (int32_t)((a >= 0 ? a + b / 2 : a - b / 2) / b); }   /* b > 0, to the nearest */
void eng_eye_view(eng_eye *e, geo_view *gv, float focal)
{
    const float pf = e->focal_max > 0.0f && focal > e->focal_max ? e->focal_max : focal;   /* a long lens: the eyes move in (slave_list.h) */
    const int64_t dx = pf < focal ? llround((double)e->dx * pf / focal) : e->dx, zc = e->zconv;
    if (zc > 0) {                                     /* the shear: + dx * z / zconv, z = the third column (Q15 rows, plain t), rounded:
                                                       * truncated, it put a flat backdrop a pixel off its labels in each eye */
        for (int r = 0; r < 3; r++) gv->m[r][0] += rdiv((int64_t)gv->m[r][2] * dx, zc);
        gv->t[0] += rdiv((int64_t)gv->t[2] * dx, zc);
    }
    gv->t[0] -= (int32_t)dx;
    e->focal = pf;
}

/* ENG_FOV_PROBE=<k>[:<yaw>] (tests): a full-frame viewport seen through a k times wider lens -- its view-space x and y over k -- and
 * turned yaw degrees (positive = looking right), so the game's own picture shrinks into the middle 1/k (or turns away) and the rest
 * shows what the game sent beyond its view: how much of its world it culls to its own camera (a VR camera that looks around needs
 * the rest). Mono pictures only; sprites and the 2D layers stay where they are. */
static double probe_k = -1, probe_yaw;
static void probe_init(void)
{
    if (probe_k >= 0) return;
    probe_k = 0;
    const char *e = getenv("ENG_FOV_PROBE");
    double k = 1, y = 0;
    if (e && sscanf(e, "%lf:%lf", &k, &y) >= 1 && k >= 1.0 && (k > 1.0 || y != 0.0)) { probe_k = k; probe_yaw = y * M_PI / 180.0; }
}
double eng_fov_probe_k(void) { probe_init(); return probe_k; }
void eng_fov_probe(geo_view *gv)
{
    probe_init();
    if (probe_k <= 0.0) return;
    if (probe_yaw != 0.0) {                          /* the head turned: x' = x cos - z sin, z' = x sin + z cos */
        const double c = cos(probe_yaw), s = sin(probe_yaw);
        for (int r = 0; r < 3; r++) {
            const double x = gv->m[r][0], z = gv->m[r][2];
            gv->m[r][0] = (int32_t)llround(x * c - z * s); gv->m[r][2] = (int32_t)llround(x * s + z * c);
        }
        const double x = gv->t[0], z = gv->t[2];
        gv->t[0] = (int32_t)llround(x * c - z * s); gv->t[2] = (int32_t)llround(x * s + z * c);
    }
    for (int r = 0; r < 3; r++) for (int c = 0; c < 2; c++) gv->m[r][c] = (int32_t)llround(gv->m[r][c] / probe_k);
    gv->t[0] = (int32_t)llround(gv->t[0] / probe_k); gv->t[1] = (int32_t)llround(gv->t[1] / probe_k);
}

/* INSIDE (slave_list.h): the object's view as the eye sees it, then the eye's frustum stretched over the 640 x 480 through the
 * viewport's own lens K and the picture's centre (the centre offset and the clip window go: the picture is the eye's now).
 * A point at eye-space (x, y, z) has to land at 640 (x/z - tl) / (tr - tl) across and 480 (tu - y/z) / (tu - td) down, and the
 * geometry stage puts it at 320 + K x'/z and 240 - K y'/z: so x' = a x + b z and y' = c y + d z, with z kept (the near plane, the
 * fog and the depth order are the eye's own depth). Linear, with a positive determinant: a back face stays one. */
static int32_t sat_round(double v) { return v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)llround(v); }
void eng_inside_view(const eng_inside *in, geo_view *gv, float focal)
{
    memcpy(gv->lm, gv->m, sizeof gv->lm); gv->have_lm = 1;
    const double K = focal > 0.0f ? focal : 1.0, w = in->tr - in->tl, h = in->tu - in->td;
    const double a = 640.0 / (K * w), b = (-640.0 * in->tl / w - 320.0) / K;
    const double c = 480.0 / (K * h), d = (240.0 - 480.0 * in->tu / h) / K;
    for (int k = 0; k < 4; k++) {                    /* rows 0..2: the object's axes (Q15); 3: its place, less the eye's */
        double v[3], e[3];
        for (int j = 0; j < 3; j++) v[j] = k < 3 ? (double)gv->m[k][j] : (double)gv->t[j] - in->p[j];
        for (int j = 0; j < 3; j++) e[j] = in->r[j][0] * v[0] + in->r[j][1] * v[1] + in->r[j][2] * v[2];
        int32_t *o = k < 3 ? gv->m[k] : gv->t;
        o[0] = sat_round(a * e[0] + b * e[2]); o[1] = sat_round(c * e[1] + d * e[2]); o[2] = sat_round(e[2]);
    }
    gv->vx = gv->vy = 0; gv->have_clip = 0;
}

/* STEREO'S ONE EXCEPTION, A BACKDROP: a quad square on to the camera (its four corners at one depth) that covers the whole 4:3
 * picture is a picture the game draws as one polygon (Dirt Dash's stage-select map, at depth 549 -- far in front of the screen), not
 * a thing in the world: it goes back ON the screen's plane, where its captions (the text layer) are. At one depth the eye moved it
 * sideways by one amount, focal * dx * (1/zconv - 1/z) (in 1/16 pixel here), which comes straight off again. "Whole" to a pixel at
 * each edge: Dirt Dash's map spans -0.82 .. 639.81 by 0.39 .. 480.61 (its corners at +-320 / +-240, its lens 550.4 against z 549). */
typedef struct { geo_quad_cb cb; void *user; double shift16, inv_zc; } eye_pass;
static void eye_quad(const geo_quad *q, void *u)
{
    const eye_pass *e = u;
    const int32_t z = q->v[0].z;
    if (z > 0 && !q->behind && q->v[1].z == z && q->v[2].z == z && q->v[3].z == z) {
        const int32_t d = (int32_t)lround(e->shift16 * (e->inv_zc - 1.0 / z));
        int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
        for (int k = 0; k < 4; k++) {
            const int32_t x = q->v[k].sx16 - d, y = q->v[k].sy16;
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
        if (x0 <= 16 && x1 >= 639 * 16 && y0 <= 16 && y1 >= 479 * 16) {
            geo_quad f = *q;
            for (int k = 0; k < 4; k++) f.v[k].sx16 -= d;
            for (int k = 0; k < f.nrv; k++) f.rv[k].sx16 -= d;
            for (int k = 0; k < f.ndv; k++) f.dv[k].sx16 -= d;
            e->cb(&f, e->user);
            return;
        }
    }
    e->cb(q, e->user);
}

int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user)
{
    /* ---- camera state, persistent across records ---- */
    int32_t viewq[3][3] = {{0x7FFF,0,0},{0,0x7FFF,0},{0,0,0x7FFF}};
    int have_view = 0;                 /* oracle: `if not hasattr(self,"viewq")` */
    int32_t zoom_mant = 0x7FFF; int zoom_shift = 15;
    int32_t vx = 0, vy = 0;
    int32_t absolute_priority = 0, objectshift = 0, cz_adjust = 0;
    int reflection = 0, cullflip = 0;
    int32_t amb_fx = 0, pow_fx = 0, light_fx[3] = {0, 0, 0};
    float cl = 0, cr = 0, cu = 0, cd = 0; int have_clip = 0;

    /* Fixed list head (simulate_slavedsp): Super 22 starts at 0x304, System 22
     * at 0x2FF. Deriving it from a pointer word and skipping an FFFE prologue
     * (what this once did) is not what the hardware does. */
    int src = cfg->head;
    int objectflags = 0;

    int guard = 0, prims = 0;
    while (guard++ < 4096) {
        uint32_t code = pw(src) & 0xFFFF;
        uint32_t len  = pw(src + 1) & 0xFFFF;
        const int p = src + 2;                        /* payload base */

        if (len == 0x15) {                            /* bb0003: viewport */
            absolute_priority = (int32_t)((pw(p + 3) >> 16) & 0xffff);
            vx = sext_n((pw(p + 5) >> 16) & 0xffff, 12);
            vy = sext_n( pw(p + 5)        & 0xffff, 12);
            uint32_t z = pw(p + 6);
            zoom_mant = sext_n(z & 0xffff, 16);
            { int e = (int)((z >> 16) & 0x3f); int sh = 0x2e - e;
              zoom_shift = sh < 0 ? 0 : sh; }
            amb_fx = (int32_t)((pw(p + 1) >> 16) & 0xffff);
            pow_fx = (int32_t)( pw(p + 1)        & 0xffff);
            for (int k = 0; k < 3; k++) light_fx[k] = q15v(pw(p + 2 + k));
            {   /* scene clip window, and the reflection swaps that go with it */
                float zf = dspfloatf(z);
                cl = dspfloatf(pw(p + 8)) * zf - 0.5f;   /* vl = src[8] */
                cr = dspfloatf(pw(p + 7)) * zf - 0.5f;   /* vr = src[7] */
                cu = dspfloatf(pw(p + 9)) * zf - 0.5f;
                cd = dspfloatf(pw(p + 10)) * zf - 0.5f;
            }
            reflection = (int)((pw(p + 2) >> 16) & 0x30);
            cullflip   = (reflection == 0x10 || reflection == 0x20);
            if (reflection & 0x10) { float t2 = cl; cl = cr; cr = t2; }
            if (reflection & 0x20) { float t2 = cu; cu = cd; cd = t2; }
            have_clip = 1;
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 0x0c + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
            cz_adjust = objectshift = 0;   /* viewport resets the shift group */
            objectflags = 0;
        } else if (len == 0x10) {                     /* 233002: object shift */
            cz_adjust   = (int32_t)(pw(p + 1) & 0xffffff);
            objectshift = (int32_t)(pw(p + 2) & 0xffffff);
            if (cfg->s22_objectflags) objectflags = (int)((pw(p + 3) >> 21) & 7);
        } else if (len == 0x0a) {                     /* 300000: view xform */
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 1 + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
        } else if (len == 0x0d) {                     /* 200002: primitive */
            if ((code == 0x5 || code >= 0x45) && have_view) {
                int32_t m[3][3], t[3];
                for (int col = 0; col < 3; col++)
                    for (int row = 0; row < 3; row++)
                        m[row][col] = q15v(pw(p + 1 + col * 3 + row));
                for (int k = 0; k < 3; k++)
                    t[k] = sext_n(pw(p + 0x0a + k), 24);

                geo_view gv;
                memset(&gv, 0, sizeof gv);
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        gv.m[r][c] = (int32_t)(((int64_t)m[r][0] * viewq[0][c] +
                                                (int64_t)m[r][1] * viewq[1][c] +
                                                (int64_t)m[r][2] * viewq[2][c]) >> 15);
                for (int c = 0; c < 3; c++)
                    gv.t[c] = (int32_t)(((int64_t)t[0] * viewq[0][c] +
                                         (int64_t)t[1] * viewq[1][c] +
                                         (int64_t)t[2] * viewq[2][c]) >> 15);
                const float focal = (float)zoom_mant / (float)(1ull << zoom_shift);
                int eyed = 0, full = 0;
                if (have_clip) {
                    const int32_t cx = 320 + vx;
                    full = (int32_t)((float)cx + cl) <= 0 && (int32_t)((float)cx - cr - 1.0f) >= 639;   /* a full-frame viewport */
                }
                if (full && !cfg->inside) {
                    if (cfg->eye) {                      /* stereo (slave_list.h): quad_gl.c's test of one */
                        eng_eye_view(cfg->eye, &gv, focal);
                        eyed = 1;
                    } else eng_fov_probe(&gv);
                }
                gv.zoom_mant = zoom_mant; gv.zoom_shift = zoom_shift;
                gv.vx = vx; gv.vy = vy;
                gv.objectshift = objectshift;
                gv.cz_adjust = cz_adjust;
                gv.cl = cl; gv.cr = cr; gv.cu = cu; gv.cd = cd;
                gv.have_clip = have_clip;
                gv.absolute_priority = absolute_priority;
                gv.cullflip = cullflip;
                memcpy(gv.viewq, viewq, sizeof viewq);
                gv.light[0] = light_fx[0]; gv.light[1] = light_fx[1];
                gv.light[2] = light_fx[2];
                gv.ambient = amb_fx; gv.power = pow_fx;
                gv.objectflags = objectflags;
                if (cfg->inside && full) {               /* an Inside eye (slave_list.h): the world from where it is */
                    cfg->inside->focal = focal; cfg->inside->vx = vx; cfg->inside->vy = vy;
                    eng_inside_view(cfg->inside, &gv, focal);
                }
                if (!cfg->inside || full) {              /* (an Inside walk leaves every other viewport to the screen) */
                    geo_hw_set_view(&gv);
                    g_bbox_cur = (int)code;
                    if (eyed) {
                        const eye_pass ep = { cb, user, 16.0 * cfg->eye->dx * cfg->eye->focal,
                                              cfg->eye->zconv > 0 ? 1.0 / cfg->eye->zconv : 0.0 };
                        geo_hw_object((int32_t)code, eye_quad, (void *)&ep);
                    } else geo_hw_object((int32_t)code, cb, user);
                }
                g_bbox_cur = -1;
                objectflags &= ~2;             /* blit_polyobject: per object */
                prims++;
            }
        } else {
            break;                                    /* unknown length */
        }

        /* The next record must follow IMMEDIATELY; anything else ends the
         * list (oracle: `if nxt != index + length + 2: break`). */
        int nxt = (int)(pw(src + len + 3) & 0x7FFF);
        if (nxt != src + (int)len + 4) break;
        src = nxt;
    }
    if (cfg->out_zoom_mant) { *cfg->out_zoom_mant = zoom_mant; *cfg->out_zoom_shift = zoom_shift;
                              *cfg->out_vx = vx; *cfg->out_vy = vy; }
    return prims;
}
