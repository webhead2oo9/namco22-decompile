/*
 * ss22_gl.c -- a Super System 22 frame through the shared engine (see ss22_gl.h).
 *
 * Order of a frame, as Prop Cycle's renderer does it and MAME's namcos22_v.cpp defines it:
 *   1. clear to the mixer's background colour
 *   2. quads and sprites in ONE far-to-near walk (zsort / sprite z), each quad through the engine's rasteriser with
 *      CZ fog applied after shading and the mixer's poly fade; each sprite rendered to its bounding box and blended
 *   3. the screen fade over everything drawn so far (mixer flag bit 0)
 *   4. the text tilemap over that
 *   5. the mixer's gamma tables over the whole frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "quad_gl.h"
#include "tex_bake.h"
#include "post_gl.h"
#include "fog_hw.h"
#include "text_hw.h"
#include "sprite_hw.h"
#include "ss22_gl.h"
#include "hud_edges.h"

#define NW ENG_SCREEN_W
#define NH ENG_SCREEN_H

/* ------------------------------------------------------------------ the quads */
static geo_quad *qbuf;
static int       qn, qcap, qorder;

/* STEREO (ss22_set_stereo): the right eye's sorted quads; qbuf holds the left eye's (or the one camera's). eye_swap trades the two, so
 * the walk, the sort and the draw use one set of names for either eye. */
static geo_quad *qbuf_r;
static int       qn_r, qcap_r;
static int32_t   st_sep, st_zconv;            /* the next prepare's eyes (0 = the game's camera) */
static float     st_fmax;                     /* their longest full-depth lens (slave_list.h eng_eye.focal_max) */
static bool      st_frame;                    /* the prepared frame has two eyes */
static float     st_focal;                    /* its full-frame viewport's focal length as the parallax sees it (0 = no world this frame) */
static void eye_swap(void)
{
    geo_quad *b = qbuf; qbuf = qbuf_r; qbuf_r = b;
    int n = qn; qn = qn_r; qn_r = n;
    n = qcap; qcap = qcap_r; qcap_r = n;
}
static int32_t eye_dx(int eye) { return eye ? st_sep - st_sep / 2 : -(st_sep / 2); }   /* the pair's two halves add up to st_sep */
void ss22_set_stereo(int32_t sep, int32_t zconv, float focal_max) { st_sep = sep > 0 ? sep : 0; st_zconv = zconv; st_fmax = focal_max; }
bool ss22_stereo_frame(void) { return st_frame; }

/* INSIDE (ss22_set_inside): polygon RAM as this frame's walk saw it, kept for the eyes a headset draws later -- each walks the same
 * list from where it is (and the gun's ray once more). The prepared quads stay the game camera's own: the screen's (ss22_draw_panel). */
static bool      ins_on, ins_frame, ins_world;  /* asked for; the prepared frame has the copy; it has a full-frame viewport (a world) */
static uint32_t *ins_poly;
static uint32_t ins_word(int i) { return ins_poly[i & 0x7FFF]; }
void ss22_set_inside(bool on) { ins_on = on; }

static void push_quad(const geo_quad *q, void *user)
{
    (void)user;
    if (qn == qcap) {
        int nc = qcap ? qcap * 2 : 8192;
        geo_quad *nb = realloc(qbuf, (size_t)nc * sizeof *qbuf);
        if (!nb) return;
        qbuf = nb; qcap = nc;
    }
    qbuf[qn] = *q;
    qbuf[qn].order = qorder++;
    qn++;
}

/* Super 22 fog for one quad: the CZ tables (fog_hw.c), colour = the mixer's fog colour */
static int ss22_fog_quad(const geo_quad *q, eng_fog *f)
{
    const uint8_t *tab = NULL; int sdelta = 0;
    if (!fog_quad(q->color, q->cz_type, &tab, &sdelta)) return 0;
    memcpy(f->rgb, g_fog.fog_rgb, 3);
    f->tab = tab; f->sdelta = sdelta;
    return 1;
}

/* the mixer's poly fade, per channel, after fog */
static void ss22_fade_rgb(float *r, float *g, float *b)
{
    if (g_fog_valid && g_fog.poly_fade_enabled) {
        *r *= g_fog.poly_fade[0] / 256.0f;
        *g *= g_fog.poly_fade[1] / 256.0f;
        *b *= g_fog.poly_fade[2] / 256.0f;
    }
}

static void ss22_qhist_report(void);
static void ss22_cliplog_report(void);

/* ------------------------------------------------------------------ prepared state */
static text_state      tst;
static sprite_state    sst;
static bool            have_spr;
static sprite_item     items[1024];
static int             ni;
static uint8_t         cgbuf[0x20000];
static bool            text_off, spr_off;
static uint8_t         prio_mask[SPR_W * SPR_H];   /* pixels a sprite drawn over the text layer covers this frame */
static bool            prio_any;
static const uint16_t *spot_src;             /* the text layer's spot RAM this frame, for the cache key */
static bool            spot_on;
static int             frames;
static bool            hud_on;               /* the game is showing its race HUD (widescreen moves it to the edges) */
static const eng_hud_cfg *hud_cfg;           /* the game's HUD description, set by its board host (NULL = the HUD stays put) */

void ss22_gl_set_hud(const eng_hud_cfg *h) { hud_cfg = h; }

/* THE GUN SHOT FLASH (Time Crisis): a light-gun cabinet whitens the whole screen for the frame after the trigger so the gun's
 * photodiode can find the beam -- the mixer's screen fade at full strength in white, switched on for ONE frame with no ramp
 * (measured: fade FFFFFF x FF, flags 03, the frames either side 00). Our gun is the pointer and needs no flash; with the option
 * off, such an instant white is not drawn for up to 2 frames. A real fade to white ramps (the factor climbs over frames) and
 * is left alone; one that STARTS at full white shows from its third frame. The game itself is untouched.
 * INSIDE never draws it, whatever the option: there the fade covers the whole world round the player, and a shot would strobe
 * everything they can see white, every shot, at the headset's brightness. */
static bool gun_flash = true;
void ss22_gl_set_gun_flash(bool on) { gun_flash = on; }
static void gun_flash_filter(void)
{
    static int prev_fade, run, env = -1;
    if (env < 0) { const char *e = getenv("ENG_GUN_FLASH"); env = e ? 1 : 0; if (e) gun_flash = atoi(e) != 0; }   /* tests (headless has no settings page) */
    const int fade = (g_fog.mixer_flags & 3) && g_fog.screen_fade_factor;
    const int white = fade && g_fog.screen_fade_factor >= 0xF0 && g_fog.screen_fade[0] >= 0xF0 && g_fog.screen_fade[1] >= 0xF0 && g_fog.screen_fade[2] >= 0xF0;
    if (!white) run = 0;
    else if (run || !prev_fade) run++;                   /* white straight out of no fade: a flash (a ramp reaching white has prev_fade set) */
    prev_fade = fade;
    if ((!gun_flash || ins_on) && run && run <= 2) g_fog.screen_fade_factor = 0;
}

void ss22_prepare(const ss22_regs *r)
{
    if (!frames) {
        const char *e = getenv("ENG_NO_TEXT");    text_off = e && *e != '0';
        e = getenv("ENG_NO_SPRITES");             spr_off  = e && *e != '0';
    }
    frames++;
    g_eng_frame++;
    text_set_spot(r->spotram, r->spot_enabled);
    spot_src = r->spotram; spot_on = r->spotram && r->spot_enabled;

    fog_load_regs(r->mixer, r->czattr, r->czram);
    gun_flash_filter();
    eng_palette_from_planar(r->pal, 0x8000);

    /* the text model indexes cg[0x1E000:0x20000] as textram, and a board keeps the two regions apart */
    memcpy(cgbuf, r->cgram, 0x1E000);
    memcpy(cgbuf + 0x1E000, r->textram, 0x2000);
    text_load_regs(&tst, cgbuf, r->pal, r->tilemapattr);

    { static int hold;                                                /* widescreen: is the race HUD up? */
      hud_on = hud_cfg && hud_cfg->marks && eng_hud_marks_up(r->textram, hud_cfg->marks, hud_cfg->n_marks, &hold); }

    ni = 0; have_spr = false;
    if (!spr_off && g_sprite_tiles && r->spriteram &&
        sprite_load_regs(&sst, r->spriteram, r->spriteram_size, r->vics, r->vics_size, r->vics_ctl, r->pal)) {
        have_spr = true;
        ni = sprite_collect(&sst, &g_fog, items, (int)(sizeof items / sizeof items[0]));
        static int sprlog = -1;                      /* ENG_SPRLOG=<n>: the sprite list of update n, one line each (finding a HUD's parts) */
        if (sprlog < 0) { const char *e = getenv("ENG_SPRLOG"); sprlog = e ? atoi(e) : 0; }
        if (sprlog && frames == sprlog)
            for (int i = 0; i < ni; i++)
                fprintf(stderr, "[SPR] %3d z %06X layer %u  x %4d y %4d  %3d x %3d  idx %d  tile %d%s\n", i, items[i].z, items[i].z >> 21, items[i].x0, items[i].y0,
                        items[i].w, items[i].h, items[i].idx, items[i].tile, items[i].prioverchar ? "  prio" : "");
    }

    qn = 0; qorder = 0; qn_r = 0;
    st_frame = st_sep > 0; st_focal = 0;
    if (r->walk) {
        eng_eye eye[2] = { { eye_dx(0), st_zconv, st_fmax, 0 }, { eye_dx(1), st_zconv, st_fmax, 0 } };
        eng_list_cfg cfg = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL, st_frame ? &eye[0] : NULL };
        eng_walk_list(r->poly_word, &cfg, push_quad, NULL);
        eng_quad_sort(qbuf, qn, 0);
        if (st_frame) {                          /* the right eye: the same list again (the walk only reads polygon RAM and the point ROM) */
            eye_swap();
            qn = 0; qorder = 0; cfg.eye = &eye[1];
            eng_walk_list(r->poly_word, &cfg, push_quad, NULL);
            eng_quad_sort(qbuf, qn, 0);
            eye_swap();
            st_focal = eye[0].focal;
        }
    }
    ins_frame = ins_world = false;
    if (ins_on && r->walk && (ins_poly || (ins_poly = malloc(0x8000 * sizeof *ins_poly)))) {
        for (int i = 0; i < 0x8000; i++) ins_poly[i] = r->poly_word(i);
        ins_frame = true;
        for (int i = 0; i < qn && !ins_world; i++) ins_world = qbuf[i].clip[0] <= 0 && qbuf[i].clip[1] >= NW - 1;
    }
    ss22_qhist_report();
    ss22_cliplog_report();
}

int ss22_quads(void)   { return qn; }

/* ENG_QHIST=<n>: where the game's polygons land horizontally in update n (screen x of each quad's left edge and right edge, 128 px bins from
 * -1024 to 1664) -- how far past the 4:3 window the game's own culling lets the picture go */
static void ss22_cliplog_report(void)                /* ENG_CLIPLOG=<n>: the distinct viewport clip windows (l r t b) of update n and their quad counts */
{
    static int want = -1;
    if (want < 0) { const char *e = getenv("ENG_CLIPLOG"); want = e ? atoi(e) : 0; }
    if (!want || frames != want) return;
    int32_t seen[64][4]; int cnt[64], ns = 0;
    for (int i = 0; i < qn; i++) {
        int k;
        for (k = 0; k < ns; k++) if (!memcmp(seen[k], qbuf[i].clip, sizeof seen[k])) break;
        if (k == ns && ns < 64) { memcpy(seen[ns], qbuf[i].clip, sizeof seen[0]); cnt[ns++] = 0; }
        if (k < 64) cnt[k]++;
    }
    for (int k = 0; k < ns; k++) fprintf(stderr, "[CLIP] %4d..%4d x %4d..%4d  %d quads\n", seen[k][0], seen[k][1], seen[k][2], seen[k][3], cnt[k]);
}

static void ss22_qhist_report(void)
{
    static int want = -1;
    if (want < 0) { const char *e = getenv("ENG_QHIST"); want = e ? atoi(e) : 0; }
    if (!want || frames != want) return;
    int lo[24] = { 0 }, hi[24] = { 0 };
    for (int i = 0; i < qn; i++) {
        int mn = 1 << 30, mx = -(1 << 30);
        for (int k = 0; k < qbuf[i].nrv; k++) { int x = qbuf[i].rv[k].sx16 >> 4; if (x < mn) mn = x; if (x > mx) mx = x; }
        int a = (mn + 1024) / 128, b = (mx + 1024) / 128;
        if (a < 0) a = 0; if (a > 23) a = 23; if (b < 0) b = 0; if (b > 23) b = 23;
        lo[a]++; hi[b]++;
    }
    for (int i = 0; i < qn; i++) {                       /* few quads (a menu screen): each one in full; else the wide screen-space ones (one depth, 300+ px) */
        int mn = 1 << 30, mx = -(1 << 30);
        for (int k = 0; k < qbuf[i].nrv; k++) { const int x = qbuf[i].rv[k].sx16 >> 4; if (x < mn) mn = x; if (x > mx) mx = x; }
        const bool ss = qbuf[i].nrv == 4 && qbuf[i].rv[0].z == qbuf[i].rv[1].z && qbuf[i].rv[1].z == qbuf[i].rv[2].z && qbuf[i].rv[2].z == qbuf[i].rv[3].z && mx - mn >= 300;
        if (qn > 12 && !ss) continue;
        {
            int mn = 1 << 30, mx = -(1 << 30), my0 = 1 << 30, my1 = -(1 << 30);
            for (int k = 0; k < qbuf[i].nrv; k++) {
                const int x = qbuf[i].rv[k].sx16 >> 4, y = qbuf[i].rv[k].sy16 >> 4;
                if (x < mn) mn = x; if (x > mx) mx = x; if (y < my0) my0 = y; if (y > my1) my1 = y;
            }
            fprintf(stderr, "[QUAD] %d  x %d..%d  y %d..%d  zsort %06X  clip %d..%d  nrv %d  z0..z3 %d %d %d %d  uv %d..%d x %d..%d  bank %d pal %d\n", i, mn, mx, my0, my1,
                    qbuf[i].zsort & 0xffffff, qbuf[i].clip[0], qbuf[i].clip[1], qbuf[i].nrv, (int)qbuf[i].v[0].z, (int)qbuf[i].v[1].z, (int)qbuf[i].v[2].z, (int)qbuf[i].v[3].z,
                    qbuf[i].uvbox[0], qbuf[i].uvbox[1], qbuf[i].uvbox[2], qbuf[i].uvbox[3], qbuf[i].texbank, (qbuf[i].color >> 8) & 0x7F);
            for (int k = 0; k < qbuf[i].nrv; k++)
                fprintf(stderr, "[QUAD]    v%d  sx %d sy %d  u %u v %u  bri %d\n", k, qbuf[i].rv[k].sx16 >> 4, qbuf[i].rv[k].sy16 >> 4, qbuf[i].rv[k].u, qbuf[i].rv[k].v, qbuf[i].rv[k].bri);
        }
    }
    fprintf(stderr, "[QHIST] %d quads; bins of 128 px from -1024 (left edge | right edge)\n", qn);
    for (int i = 0; i < 24; i++) fprintf(stderr, "[QHIST] %5d..%5d  %5d %5d\n", i * 128 - 1024, i * 128 - 897, lo[i], hi[i]);
}
int ss22_sprites(void) { return ni; }

/* ------------------------------------------------------------------ drawing */
static bool    gl_ready;
static GLuint  spr_tex, txt_tex;
static uint8_t *spr_buf, *txt_buf;

static void tex_alloc(GLuint *t)
{
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPR_W, SPR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

/* INSIDE's SCREEN (ss22_draw_panel) is drawn over a clear picture and goes out as a layer: its alpha has to be what covers, so a blend
 * there adds alpha as "over" does (the colour, blended as ever into black, comes out premultiplied) */
#ifndef APIENTRY
#define APIENTRY
#endif
typedef void (APIENTRY *pfn_blend_sep)(GLenum, GLenum, GLenum, GLenum);
static pfn_blend_sep p_blend_sep;
static bool panel_pass;
static void blend_over(void)
{
    if (panel_pass && p_blend_sep) p_blend_sep(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* one sprite in its turn of the merged z order: rendered into the corner of one 640x480 texture, sub-imaged, blended; par = its stereo
 * parallax (scene units, 0 = flat on the screen's plane); xyz = an Inside eye's own corners for it (x, y, the eye's depth), clockwise
 * from the top left, or NULL */
static void draw_sprite(const sprite_item *it, float par, const float (*xyz)[3])
{
    if (!spr_buf && !(spr_buf = malloc((size_t)SPR_W * SPR_H * 4))) return;
    sprite_render_item(&sst, &g_fog, it, spr_buf, 1);
    if (it->prioverchar) {                    /* the text layer shows nothing under this sprite's pixels */
        for (int y = 0; y < it->h; y++)
            for (int x = 0; x < it->w; x++)
                if (spr_buf[((size_t)y * it->w + x) * 4 + 3]) prio_mask[(size_t)(it->y0 + y) * SPR_W + it->x0 + x] = 1;
        prio_any = true;
    }
    if (!spr_tex) tex_alloc(&spr_tex); else glBindTexture(GL_TEXTURE_2D, spr_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, it->w, it->h, GL_RGBA, GL_UNSIGNED_BYTE, spr_buf);
    const float su = (float)it->w / SPR_W, sv = (float)it->h / SPR_H;
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    blend_over();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    if (xyz) {                                /* (a billboard square on to the game's camera: a plane, so its texture is the eye's 1/z) */
        const float s[4] = { 0, su, su, 0 }, t[4] = { 0, 0, sv, sv };
        for (int k = 0; k < 4; k++) { const float w = 1.0f / xyz[k][2]; glTexCoord4f(s[k] * w, t[k] * w, 0, w); glVertex2f(xyz[k][0], xyz[k][1]); }
    } else {
        /* widescreen: a HUD sprite goes out to its side (a sprite that is deep in the world, a billboard, stays where the 3D puts it) */
        const int dx = (g_eng_hud_e && hud_cfg && (int)(it->z & 0x1FFFFF) <= hud_cfg->sprite_zmax) ? eng_hud_dx(it->x0 + it->w / 2.0) : 0;
        const float x0 = (float)(it->x0 + dx) + par, x1 = x0 + (float)it->w;
        glTexCoord2f(0,  0);  glVertex2f(x0, (float)it->y0);
        glTexCoord2f(su, 0);  glVertex2f(x1, (float)it->y0);
        glTexCoord2f(su, sv); glVertex2f(x1, (float)(it->y0 + it->h));
        glTexCoord2f(0,  sv); glVertex2f(x0, (float)(it->y0 + it->h));
    }
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

/* the text tilemap, drawn last: transparent where it draws nothing, its alpha left for GL to blend. text_render is a
 * pure function of the RAM it reads, so it re-renders only when a hash of those inputs changes. */
/* WIDESCREEN: a full-screen SHADE in the text layer (Dirt Dash's jungle: black, translucent, darkest at the top and bottom -- MAME's text spot/alpha) is
 * laid out for 640 px and, drawn as it is, stops at the 4:3 edges: a dark rectangle in the middle of a wider picture. Its outermost columns -- the first
 * from each side where most rows are that kind of pixel (translucent and dark; HUD text is opaque) -- are repeated outward to the picture's edges, row by
 * row, so the vignette carries on. Nothing else in the layer is ever repeated. ENG_TEXT_EXTEND=0 turns it off. */
/* A pixel of a full-screen shade: translucent and dark (HUD text is opaque). */
static bool is_shade(const uint8_t *q) { return q[3] && q[3] < 255 && q[0] <= 24 && q[1] <= 24 && q[2] <= 24; }

/* WIDESCREEN, a shade under a moving HUD. The HUD's text is moved out piece by piece, and a piece is a connected run of drawn pixels -- a full-screen
 * shade is drawn pixels too, and joined every label of the layer into one screen-wide "banner" that (rightly) stays put: the sprites' digits went to the
 * corners and their labels stayed in the middle. So while the HUD moves, a layer with a shade is drawn in two passes: the SHADE alone, where it was (with
 * the holes under the text filled from the shade beside them, or the text's old places would show as bright silhouettes), then the TEXT alone, piece
 * by piece, at its side. The piece scan sees the text only. */
static uint8_t *shd_buf, *only_buf;                 /* the shade (holes filled), and everything else */
static GLuint   shd_tex;
static bool     shade_split;                        /* this layer holds a full-screen shade and the HUD may move */

static void text_hud_scan(const uint8_t *rgba)
{
    static uint8_t occ[SPR_W * SPR_H];
    long nshade = 0;
    for (long i = 0; i < (long)SPR_W * SPR_H; i++) { const uint8_t *q = rgba + i * 4; const bool sh = is_shade(q); nshade += sh; occ[i] = q[3] && !sh; }
    eng_hud_text_scan(occ);
    shade_split = nshade > (long)SPR_W * SPR_H / 8;
    if (!shade_split) return;
    if (!shd_buf) shd_buf = malloc((size_t)SPR_W * SPR_H * 4);
    if (!only_buf) only_buf = malloc((size_t)SPR_W * SPR_H * 4);
    if (!shd_buf || !only_buf) { shade_split = false; return; }
    for (int y = 0; y < SPR_H; y++) {
        const uint8_t *row = rgba + (size_t)y * SPR_W * 4;
        uint8_t *sr = shd_buf + (size_t)y * SPR_W * 4, *tr = only_buf + (size_t)y * SPR_W * 4;
        for (int x = 0; x < SPR_W; x++) {
            const uint8_t *q = row + (size_t)x * 4;
            if (is_shade(q)) { memcpy(sr + (size_t)x * 4, q, 4); memset(tr + (size_t)x * 4, 0, 4); }
            else             { memcpy(tr + (size_t)x * 4, q, 4); memset(sr + (size_t)x * 4, 0, 4); }
        }
        for (int x = 0; x < SPR_W; x++) {               /* the shade behind a text pixel: what its neighbours in the row have */
            const uint8_t *q = row + (size_t)x * 4;
            if (!q[3] || is_shade(q)) continue;
            for (int d = 1; d <= 48; d++) {
                const uint8_t *l = x - d >= 0 ? row + (size_t)(x - d) * 4 : NULL, *r = x + d < SPR_W ? row + (size_t)(x + d) * 4 : NULL;
                if (l && is_shade(l)) { memcpy(sr + (size_t)x * 4, l, 4); break; }
                if (r && is_shade(r)) { memcpy(sr + (size_t)x * 4, r, 4); break; }
            }
        }
    }
}

static uint8_t ext_pix[2 * SPR_H * 4];        /* texel 0 = the left shade column, texel 1 = the right one, one row each */
static int     ext_xl = -1, ext_xr = -1;      /* the source columns (-1 = that side has no shade) */
static GLuint  ext_tex;
static bool    ext_dirty;

static void text_shade_scan(const uint8_t *rgba)
{
    static int cov[SPR_W];
    memset(cov, 0, sizeof cov);
    for (int y = 0; y < SPR_H; y++) {
        const uint8_t *row = rgba + (size_t)y * SPR_W * 4;
        for (int x = 0; x < SPR_W; x++) {
            const uint8_t *q = row + (size_t)x * 4;
            if (is_shade(q)) cov[x]++;
        }
    }
    ext_xl = ext_xr = -1;
    for (int x = 0; x < SPR_W / 2; x++) if (cov[x] >= SPR_H / 2) { ext_xl = x; break; }
    for (int x = SPR_W - 1; x >= SPR_W / 2; x--) if (cov[x] >= SPR_H / 2) { ext_xr = x; break; }
    memset(ext_pix, 0, sizeof ext_pix);
    for (int y = 0; y < SPR_H; y++)
        for (int side = 0; side < 2; side++) {
            const int x = side ? ext_xr : ext_xl;
            if (x < 0) continue;
            const uint8_t *q = rgba + ((size_t)y * SPR_W + (size_t)x) * 4;
            if (is_shade(q)) memcpy(ext_pix + ((size_t)y * 2 + (size_t)side) * 4, q, 4);
        }
    ext_dirty = true;
}

static void text_shade_draw(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("ENG_TEXT_EXTEND"); on = !(e && *e == '0'); }
    if (!on || g_scene_x0 > -0.5f || (ext_xl < 0 && ext_xr < 0)) return;
    if (!ext_tex) { tex_alloc(&ext_tex); ext_dirty = true; }
    glBindTexture(GL_TEXTURE_2D, ext_tex);
    if (ext_dirty) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, SPR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, ext_pix); ext_dirty = false; }
    glBegin(GL_QUADS);
    if (ext_xl >= 0) {                                   /* from the picture's left edge to the source column: the column's own row, at the texel's centre */
        glTexCoord2f(0.25f, 0); glVertex2f(g_scene_x0, 0);  glTexCoord2f(0.25f, 0); glVertex2f((float)ext_xl, 0);
        glTexCoord2f(0.25f, 1); glVertex2f((float)ext_xl, NH); glTexCoord2f(0.25f, 1); glVertex2f(g_scene_x0, NH);
    }
    if (ext_xr >= 0) {
        glTexCoord2f(0.75f, 0); glVertex2f((float)(ext_xr + 1), 0); glTexCoord2f(0.75f, 0); glVertex2f(g_scene_x1, 0);
        glTexCoord2f(0.75f, 1); glVertex2f(g_scene_x1, NH); glTexCoord2f(0.75f, 1); glVertex2f((float)(ext_xr + 1), NH);
    }
    glEnd();
    glBindTexture(GL_TEXTURE_2D, txt_tex);
}

static void draw_text(void)
{
    if (text_off || !tst.valid) return;
    if (!txt_buf && !(txt_buf = malloc((size_t)SPR_W * SPR_H * 4))) return;
    static uint64_t last_hash; static bool have_last, txt_dirty, tex_masked; static long last_px;
    uint64_t h = 1469598103934665603ULL;
#define MIX(p, n) do { const uint8_t *_b = (const uint8_t *)(p); for (size_t _i = 0; _i < (size_t)(n); _i++) { h ^= _b[_i]; h *= 1099511628211ULL; } } while (0)
    MIX(tst.cgram, 0x20000); MIX(tst.pal, 0x18000); MIX(tst.attr, sizeof tst.attr);
    MIX(g_fog.poly_fade, sizeof g_fog.poly_fade); MIX(g_fog.gamma, sizeof g_fog.gamma);
    MIX(&g_fog.mixer_flags, 1); MIX(&g_fog.text_palbase, 1); MIX(&g_fog.text_alpha, 1); MIX(&g_fog.text_alpha_lo, 1);
    MIX(&g_fog.text_alpha_hi, 1); MIX(&g_fog.text_alpha_mask, 1); MIX(&g_fog.spot_factor, sizeof g_fog.spot_factor);
    MIX(&spot_on, sizeof spot_on); if (spot_on && spot_src) MIX(spot_src, 0x1000);
    { int fade_en = ((g_fog.mixer_flags & 2) != 0) && g_fog.screen_fade_factor;   /* text_hw.c gates the fade the same way */
      MIX(&fade_en, sizeof fade_en);
      if (fade_en) { MIX(g_fog.screen_fade, 3); MIX(&g_fog.screen_fade_factor, 1); } }
#undef MIX
    long px;
    if (have_last && h == last_hash) px = last_px;
    else {
        text_render(&tst, &g_fog, txt_buf, 0);
        px = 0;
        for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (txt_buf[i * 4 + 3]) px++;
        last_hash = h; have_last = true; last_px = px; txt_dirty = true;
        if (hud_cfg && hud_cfg->marks) text_hud_scan(txt_buf);                 /* the layer's pieces (the text; a shade apart), for widescreen */
        else shade_split = false;
        text_shade_scan(txt_buf);                                              /* a full-screen shade in the layer, for widescreen */
    }
    { static int want = -1;                              /* ENG_TXTEDGE=<n>: the text layer's outermost pixel columns of update n (what a full-screen shade looks like) */
      if (want < 0) { const char *e = getenv("ENG_TXTEDGE"); want = e ? atoi(e) : 0; }
      if (want && frames == want) {
          for (int y = 0; y < SPR_H; y += 40) {
              const uint8_t *l = txt_buf + ((size_t)y * SPR_W) * 4, *m = txt_buf + ((size_t)y * SPR_W + SPR_W / 2) * 4, *r = txt_buf + ((size_t)y * SPR_W + SPR_W - 1) * 4;
              fprintf(stderr, "[TXTEDGE] y %3d  left %3d %3d %3d a%3d   mid %3d %3d %3d a%3d   right %3d %3d %3d a%3d\n", y, l[0], l[1], l[2], l[3], m[0], m[1], m[2], m[3], r[0], r[1], r[2], r[3]);
          }
          long opaque = 0, shade = 0;
          for (int y = 0; y < SPR_H; y++) for (int x = 0; x < SPR_W; x += SPR_W - 1) { const uint8_t a8 = txt_buf[((size_t)y * SPR_W + x) * 4 + 3]; if (a8 == 255) opaque++; else if (a8) shade++; }
          fprintf(stderr, "[TXTEDGE] edge columns: %ld opaque, %ld translucent of %d\n", opaque, shade, 2 * SPR_H);
      } }
    if (!px) return;
    if (!txt_tex) { tex_alloc(&txt_tex); txt_dirty = true; }
    glBindTexture(GL_TEXTURE_2D, txt_tex);
    const bool split = shade_split && g_eng_hud_e != 0;             /* a moving HUD over a shade: the text alone here, the shade in its own texture */
    static bool tex_split;
    /* the texture already holds this text under this mask: a race's HUD sprites mask it every frame, and re-sending the same 1.2 MB each
     * time cost ~2 ms a frame on Windows */
    static uint8_t last_mask[SPR_W * SPR_H];
    const bool mask_changed = prio_any != tex_masked || (prio_any && memcmp(prio_mask, last_mask, sizeof last_mask) != 0);
    if (txt_dirty || mask_changed || split != tex_split) {          /* upload: the text as rendered, less what a sprite over the text layer covers */
        static uint8_t *masked;
        const uint8_t *src = split ? only_buf : txt_buf, *src2 = split ? shd_buf : NULL;
        if (prio_any) {
            if (!masked && !(masked = malloc((size_t)SPR_W * SPR_H * 4))) masked = NULL;
            if (masked) {
                memcpy(masked, src, (size_t)SPR_W * SPR_H * 4);
                for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (prio_mask[i]) masked[i * 4 + 3] = 0;
                src = masked;
            }
            /* the shade gets NO such hole while the HUD moves: it stays put, and the sprite (and the text piece its hole belongs to) has gone out to its side --
             * a hole left at the old place shows the unshaded picture through it (the tacho's hub, as a bright disc) */
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, src);
        if (src2) {
            if (!shd_tex) tex_alloc(&shd_tex);
            glBindTexture(GL_TEXTURE_2D, shd_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, src2);
            glBindTexture(GL_TEXTURE_2D, txt_tex);
        }
        if (prio_any) memcpy(last_mask, prio_mask, sizeof last_mask);
        tex_masked = prio_any; tex_split = split; txt_dirty = false;
    }
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    blend_over();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    if (g_eng_hud_e) {
        if (split) {                                      /* the shade where it was (and out to the picture's edges), then the text piece by piece */
            glBindTexture(GL_TEXTURE_2D, shd_tex);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(1, 0); glVertex2f(NW, 0);
            glTexCoord2f(1, 1); glVertex2f(NW, NH);
            glTexCoord2f(0, 1); glVertex2f(0, NH);
            glEnd();
            text_shade_draw();                            /* leaves txt_tex bound */
        }
        eng_hud_text_draw();                              /* widescreen HUD: each piece of the layer at its side */
    } else {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(NW, 0);
    glTexCoord2f(1, 1); glVertex2f(NW, NH);
    glTexCoord2f(0, 1); glVertex2f(0, NH);
    glEnd();
    }
    if (!split) text_shade_draw();                       /* widescreen: a full-screen shade reaches the picture's edges */
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

/* STEREO: a sprite deeper than the HUD's is a billboard in the world: it takes the parallax a point at its depth gets from the eye's
 * frustum (slave_list.h), dx * f * (1/zconv - 1/z). The HUD's sprites, and every sprite of a frame with no world, stay on the screen. */
static float sprite_parallax(const sprite_item *it, int eye)
{
    const int32_t z = (int32_t)(it->z & 0x1FFFFF);
    if (!st_frame || st_focal <= 0.0f || z <= (hud_cfg ? hud_cfg->sprite_zmax : 0) || z <= 0) return 0.0f;
    const double inv_zc = st_zconv > 0 ? 1.0 / st_zconv : 0.0;
    return (float)(eye_dx(eye) * (double)st_focal * (inv_zc - 1.0 / z));
}

static void draw_frame(int vw, int vh, int eye);
void ss22_draw(int vw, int vh) { draw_frame(vw, vh, 0); }
void ss22_draw_eye(int eye, int vw, int vh)
{
    const bool right = st_frame && eye == 1;
    if (right) eye_swap();
    draw_frame(vw, vh, right ? 1 : 0);
    if (right) eye_swap();
}

/* every pass's start: nothing to draw with yet = false */
static bool pass_ready(void)
{
    if (!g_eng_pointrom) return false;
    if (!gl_ready) { renderer_texture_init(); gl_ready = true; }
    g_tex_opaque = 1;                       /* the polygon path has no transparent pen */
    if (prio_any) { memset(prio_mask, 0, sizeof prio_mask); prio_any = false; }
    return true;
}
/* GL's state for the 2D scene: the viewport, a top-down ortho over g_scene_x0 .. x1 by 480, no depth test, no blend */
static void pass_gl(int vw, int vh)
{
    glViewport(0, 0, vw, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(g_scene_x0, g_scene_x1, NH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}
static void quad_cfg(eng_draw_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->shade = 1;
    cfg->fog = g_fog_valid;
    cfg->fog_before_shade = 0;               /* Super 22 */
    cfg->fog_quad = ss22_fog_quad;
    cfg->fade_rgb = ss22_fade_rgb;
    cfg->wide_backdrop = 1;                   /* a menu's screen-sized backdrop reaches the wide picture's edges (engine/quad_gl.c) */
    { static int tc = -1; if (tc < 0) { const char *e = getenv("ENG_TEXEL_CENTRE"); tc = e ? atoi(e) : 1; } cfg->texel_centre = tc; }
}
/* the screen fade: blend(rgb, fade, 0xff - factor) over every pixel, background included, i.e. the fade colour at alpha (factor + 1) / 256.
 * Inside's screen (panel): over what is drawn there only, its alpha kept -- the world's eyes take the whole fade themselves. */
static void screen_fade(bool panel)
{
    if (!(g_fog_valid && (g_fog.mixer_flags & 1) && g_fog.screen_fade_factor) || (panel && !p_blend_sep)) return;
    const float a = (g_fog.screen_fade_factor + 1) / 256.0f, k = panel ? a : 1.0f;
    glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    if (panel) p_blend_sep(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   /* fade x a x what covers + rgb x (1 - a) */
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(g_fog.screen_fade[0] / 255.0f * k, g_fog.screen_fade[1] / 255.0f * k, g_fog.screen_fade[2] / 255.0f * k, a);
    glBegin(GL_QUADS);
    glVertex2f(g_scene_x0, 0); glVertex2f(g_scene_x1, 0); glVertex2f(g_scene_x1, NH); glVertex2f(g_scene_x0, NH);
    glEnd();
    glDisable(GL_BLEND); glEnable(GL_ALPHA_TEST);
}

static void draw_frame(int vw, int vh, int eye)
{
    if (!pass_ready()) return;

    /* the scene's width: wider than 4:3 widens full-frame viewports (Hor+) */
    const double aspect = (double)vw / vh;
    if (aspect > 4.0 / 3.0 + 1e-3) {
        const float E = (float)((NH * aspect - NW) / 2.0);
        g_scene_x0 = -E; g_scene_x1 = NW + E;
    } else { g_scene_x0 = 0.0f; g_scene_x1 = NW; }

    eng_hud_begin(hud_on);                  /* widescreen: how far the HUD goes out (0 = it stays) */
    g_eng_quad_dx = eng_hud_quad_dx;
    eng_hud_quads_scan(qbuf, qn, hud_cfg ? hud_cfg->quad_band : -1, hud_cfg && hud_cfg->quad_zero_depth);

    pass_gl(vw, vh);

    { static int ml = -1; if (ml < 0) ml = getenv("ENG_MIXLOG") != NULL;       /* ENG_MIXLOG=1: the mixer state of every drawn frame (finding what makes a frame white) */
      if (ml) fprintf(stderr, "[MIX] bg %02X%02X%02X fade %02X%02X%02X x%02X flags %02X quads %d sprites %d\n", g_fog.bg[0], g_fog.bg[1], g_fog.bg[2],
                      g_fog.screen_fade[0], g_fog.screen_fade[1], g_fog.screen_fade[2], g_fog.screen_fade_factor, g_fog.mixer_flags, qn, ni); }
    /* the mixer's background, before the gamma; what no layer covers */
    glClearColor(g_fog.bg[0] / 255.0f, g_fog.bg[1] / 255.0f, g_fog.bg[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    eng_draw_cfg cfg;
    quad_cfg(&cfg);

    /* MERGED Z ORDER, not layers: MAME queues sprites and polygons into the same radix tree keyed on a 24-bit z and
     * walks it far to near, so a sprite can sit behind a polygon (the HUD plate behind the score digits) */
    int qi = 0, si = 0;
    eng_draw_begin();
    while (qi < qn || si < ni) {
        const uint32_t qz = qi < qn ? (uint32_t)(qbuf[qi].zsort & 0xFFFFFF) : 0;
        const uint32_t sz = si < ni ? items[si].z : 0;
        if (si < ni && (qi >= qn || sz >= qz)) { eng_draw_end(); draw_sprite(&items[si], sprite_parallax(&items[si], eye), NULL); si++; eng_draw_resume(); }
        else eng_draw_quad(&qbuf[qi++], &cfg);
    }
    eng_draw_end();

    screen_fade(false);

    draw_text();

    /* the mixer's gamma over the whole frame, once, as the hardware does at scanout */
    if (g_fog_valid && g_fog.have_gamma) eng_post_lut(g_fog.gamma, vw, vh);
}

/* ------------------------------------------------------------------ INSIDE (engine/eng_xr.h View: Inside) */
/* The world is the full-frame viewports; a sprite in it is one deeper than the HUD's, in a frame that has a world (sprite_parallax's
 * rule). Everything else -- the HUD's sprites, the text layer, sub-window viewports, a frame with no world -- is the screen's. */
static bool full_frame(const geo_quad *q) { return q->clip[0] <= 0 && q->clip[1] >= NW - 1; }
static bool world_sprite(const sprite_item *it)
{
    const int32_t z = (int32_t)(it->z & 0x1FFFFF);
    return ins_world && z > (hud_cfg ? hud_cfg->sprite_zmax : 0);
}
/* a billboard as the eye sees it: its rectangle on the game's picture at its depth, back through the game's lens into the camera's
 * space, then into the eye and its frustum over the 640 x 480 (x, y, the eye's depth); false = a corner is behind the eye */
static bool sprite_eye(const sprite_item *it, const eng_inside *v, float xyz[4][3])
{
    const double K = v->focal, cx = 320.0 + v->vx, cy = 240.0 + v->vy, z = (double)(it->z & 0x1FFFFF);
    const double sx[4] = { it->x0, it->x0 + it->w, it->x0 + it->w, it->x0 }, sy[4] = { it->y0, it->y0, it->y0 + it->h, it->y0 + it->h };
    for (int k = 0; k < 4; k++) {
        const double P[3] = { (sx[k] - cx) * z / K - v->p[0], (cy - sy[k]) * z / K - v->p[1], z - v->p[2] };
        double e[3];
        for (int j = 0; j < 3; j++) e[j] = v->r[j][0] * P[0] + v->r[j][1] * P[1] + v->r[j][2] * P[2];
        if (e[2] < 1.0) return false;
        xyz[k][0] = (float)(NW * (e[0] / e[2] - v->tl) / (v->tr - v->tl));
        xyz[k][1] = (float)(NH * (v->tu - e[1] / e[2]) / (v->tu - v->td));
        xyz[k][2] = (float)e[2];
    }
    return true;
}

/* WHERE THE GUN CANNOT SHOOT: the game pins every shot to its own 640 x 480 picture (0x9338, the gun's reader, clamps x to 0..639
 * and y to 0..479 after its calibration), so an enemy the eye sees outside the camera's picture cannot be hit -- yet. That part of
 * the eye is drawn at `keep` of its brightness. The camera's picture is four planes through its centre (sx = 320 + vx + focal x / z,
 * sy = 240 + vy - focal y / z, as ss22_inside_ray's); seen from the eye each is a line across its picture (the eye's place left
 * out: a head's few centimetres against the world's metres), the eye's tangents u, v meeting m . (u, v, 1) >= 0 on the camera's
 * side. What lies past any line is darkened piece by piece -- past the first, then short of the first but past the second, and so
 * on -- so no piece overlaps another and none is darkened twice. A camera's picture all behind the eye leaves nothing short of
 * all four: the eye is darkened whole. */
static int clip_half(const float (*in)[2], int n, const double m[3], double side, float (*out)[2])
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const float *a = in[i], *b = in[(i + 1) % n];
        const double fa = side * (m[0] * a[0] + m[1] * a[1] + m[2]), fb = side * (m[0] * b[0] + m[1] * b[1] + m[2]);
        if (fa >= 0) { out[k][0] = a[0]; out[k][1] = a[1]; k++; }
        if ((fa >= 0) != (fb >= 0)) {
            const double t = fa / (fa - fb);
            out[k][0] = (float)(a[0] + t * (b[0] - a[0])); out[k][1] = (float)(a[1] + t * (b[1] - a[1])); k++;
        }
    }
    return k;
}
static void dim_outside(const eng_inside *v, float keep)
{
    if (keep >= 1.0f || v->focal <= 0.0f) return;
    const double f = v->focal, L = (-320.0 - v->vx) / f, R = (320.0 - v->vx) / f, B = (v->vy - 240.0) / f, T = (240.0 + v->vy) / f;
    const double n[4][3] = { { 1, 0, -L }, { -1, 0, R }, { 0, 1, -B }, { 0, -1, T } };   /* the camera's side of each, its space */
    float cur[12][2] = { { (float)v->tl, (float)v->td }, { (float)v->tr, (float)v->td }, { (float)v->tr, (float)v->tu }, { (float)v->tl, (float)v->tu } }, piece[12][2], next[12][2];
    int nc = 4;
    if (!p_blend_sep) p_blend_sep = (pfn_blend_sep)SDL_GL_GetProcAddress("glBlendFuncSeparate");
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(v->tl, v->tr, v->td, v->tu, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    if (p_blend_sep) p_blend_sep(GL_ZERO, GL_SRC_ALPHA, GL_ZERO, GL_ONE);   /* colour x keep, the alpha kept */
    else glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
    glColor4f(0, 0, 0, keep);
    for (int i = 0; i < 4 && nc >= 3; i++) {
        double m[3];
        for (int j = 0; j < 3; j++) m[j] = v->r[j][0] * n[i][0] + v->r[j][1] * n[i][1] + v->r[j][2] * n[i][2];
        const int np = clip_half((const float (*)[2])cur, nc, m, -1.0, piece);
        if (np >= 3) { glBegin(GL_POLYGON); for (int k = 0; k < np; k++) glVertex2f(piece[k][0], piece[k][1]); glEnd(); }
        nc = clip_half((const float (*)[2])cur, nc, m, 1.0, next);
        memcpy(cur, next, sizeof cur);
    }
    glDisable(GL_BLEND);
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
}

void ss22_draw_inside(eng_inside *v, int vw, int vh, float outside)
{
    v->focal = 0.0f; v->vx = v->vy = 0;
    if (!pass_ready()) return;
    g_scene_x0 = 0.0f; g_scene_x1 = NW;      /* the eye's frustum is the whole 640 x 480 */
    eng_hud_begin(false); g_eng_quad_dx = NULL;
    eye_swap();                              /* its quads go where a right eye's would (an Inside frame has no stereo pair) */
    qn = 0; qorder = 0;
    if (ins_frame) {
        eng_list_cfg lc = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL, NULL, v };
        eng_walk_list(ins_word, &lc, push_quad, NULL);
        eng_quad_sort(qbuf, qn, 0);
    }
    pass_gl(vw, vh);
    glClearColor(g_fog.bg[0] / 255.0f, g_fog.bg[1] / 255.0f, g_fog.bg[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eng_draw_cfg cfg;
    quad_cfg(&cfg);
    int qi = 0, si = 0;
    eng_draw_begin();
    for (;;) {                               /* the merged z order, the world's part (its quads are the eye's own depth) */
        while (si < ni && !world_sprite(&items[si])) si++;
        if (qi >= qn && si >= ni) break;
        const uint32_t qz = qi < qn ? (uint32_t)(qbuf[qi].zsort & 0xFFFFFF) : 0;
        const uint32_t sz = si < ni ? items[si].z : 0;
        if (si < ni && (qi >= qn || sz >= qz)) {
            float xyz[4][3];
            if (v->focal > 0.0f && sprite_eye(&items[si], v, xyz)) { eng_draw_end(); draw_sprite(&items[si], 0.0f, (const float (*)[3])xyz); eng_draw_resume(); }
            si++;
        } else eng_draw_quad(&qbuf[qi++], &cfg);
    }
    eng_draw_end();
    screen_fade(false);
    dim_outside(v, outside);
    if (g_fog_valid && g_fog.have_gamma) eng_post_lut(g_fog.gamma, vw, vh);
    eye_swap();
}

void ss22_draw_panel(int vw, int vh)
{
    if (!pass_ready()) return;
    if (!p_blend_sep) p_blend_sep = (pfn_blend_sep)SDL_GL_GetProcAddress("glBlendFuncSeparate");
    g_scene_x0 = 0.0f; g_scene_x1 = NW;
    eng_hud_begin(false); g_eng_quad_dx = NULL;
    pass_gl(vw, vh);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    panel_pass = true;
    eng_draw_cfg cfg;
    quad_cfg(&cfg);
    int qi = 0, si = 0;
    eng_draw_begin();
    for (;;) {                               /* the merged z order, the screen's part: the game's own camera */
        while (qi < qn && full_frame(&qbuf[qi])) qi++;
        while (si < ni && world_sprite(&items[si])) si++;
        if (qi >= qn && si >= ni) break;
        const uint32_t qz = qi < qn ? (uint32_t)(qbuf[qi].zsort & 0xFFFFFF) : 0;
        const uint32_t sz = si < ni ? items[si].z : 0;
        if (si < ni && (qi >= qn || sz >= qz)) { eng_draw_end(); draw_sprite(&items[si], 0.0f, NULL); si++; eng_draw_resume(); }
        else eng_draw_quad(&qbuf[qi++], &cfg);
    }
    eng_draw_end();
    screen_fade(true);
    draw_text();
    if (g_fog_valid && g_fog.have_gamma) eng_post_lut_premul(g_fog.gamma, vw, vh);
    panel_pass = false;
}

/* THE GUN'S RAY: a narrow eye looking along it (eng_xr.c builds it) sees whatever the ray meets at its picture's centre; the nearest
 * such polygon's depth there -- 1/z is linear across a polygon on screen, so the triangle's barycentric weights interpolate it. */
static void ray_quad(const geo_quad *q, void *u)
{
    double *best = u;
    const geo_vert *v = q->ndv ? q->dv : q->rv;
    const int n = q->ndv ? q->ndv : q->nrv;
    const double px = 320.0 * 16, py = 240.0 * 16;
    for (int i = 1; i + 1 < n; i++) {
        const double x0 = v[0].sx16, y0 = v[0].sy16, x1 = v[i].sx16, y1 = v[i].sy16, x2 = v[i + 1].sx16, y2 = v[i + 1].sy16;
        const double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (den == 0.0) continue;
        const double b0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
        const double b1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
        const double b2 = 1.0 - b0 - b1;
        if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0 || v[0].z <= 0 || v[i].z <= 0 || v[i + 1].z <= 0) continue;
        const double z = 1.0 / (b0 / v[0].z + b1 / v[i].z + b2 / v[i + 1].z);
        if (*best <= 0.0 || z < *best) *best = z;
        return;
    }
}
bool ss22_inside_ray(eng_inside *ray, double hit[3], float *sx, float *sy)
{
    ray->focal = 0.0f; ray->vx = ray->vy = 0;
    if (!ins_frame || !ins_world || !g_eng_pointrom) return false;
    double best = 0.0;
    eng_list_cfg lc = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL, NULL, ray };
    eng_walk_list(ins_word, &lc, ray_quad, &best);
    if (ray->focal <= 0.0f) return false;
    const double d = best > 0.0 ? best : (double)0x1FFFFF;   /* nothing met: as far as the game's depth goes */
    for (int k = 0; k < 3; k++) hit[k] = ray->p[k] + d * ray->r[2][k];
    if (hit[2] < 1.0) { *sx = *sy = -1.0f; return true; }    /* behind the game's camera: off its picture */
    *sx = (float)(320.0 + ray->vx + ray->focal * hit[0] / hit[2]);
    *sy = (float)(240.0 + ray->vy - ray->focal * hit[1] / hit[2]);
    return true;
}

/* ------------------------------------------------------------------ GL context helpers */
void eng_gl_context_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

static SDL_Window   *hl_win;
static SDL_GLContext hl_ctx;

bool eng_gl_open_headless(int w, int h)
{
#ifndef _WIN32                                          /* Windows: SDL's offscreen driver makes OpenGL only through EGL, which Windows lacks -- the hidden window below does */
    SDL_SetHint("SDL_VIDEODRIVER", "offscreen");
    setenv("SDL_VIDEODRIVER", "offscreen", 1);         /* SDL < 2.0.22 reads only the environment */
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_SetHint("SDL_VIDEODRIVER", ""); setenv("SDL_VIDEODRIVER", "", 1);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[GL] no video: %s\n", SDL_GetError()); return false; }
    }
    eng_gl_context_attributes();
    hl_win = SDL_CreateWindow("engine", 0, 0, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!hl_win) { fprintf(stderr, "[GL] no window: %s\n", SDL_GetError()); return false; }
#ifdef _WIN32
    { extern SDL_GLContext eng_gl_create_win(SDL_Window **, const char **); const char *miss = NULL;
      hl_ctx = eng_gl_create_win(&hl_win, &miss); }
#else
    hl_ctx = SDL_GL_CreateContext(hl_win);
#endif
    if (!hl_ctx) { fprintf(stderr, "[GL] no context: %s\n", SDL_GetError()); return false; }
    fprintf(stderr, "[GL] headless: %s\n", (const char *)glGetString(GL_RENDERER));
    return true;
}

bool eng_gl_write_ppm(const char *path, int vw, int vh)
{
    uint8_t *px = malloc((size_t)vw * vh * 3);
    if (!px) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return false; }
    fprintf(f, "P6\n%d %d\n255\n", vw, vh);
    for (int y = vh - 1; y >= 0; y--) fwrite(px + (size_t)y * vw * 3, 1, (size_t)vw * 3, f);
    fclose(f);
    free(px);
    return true;
}
