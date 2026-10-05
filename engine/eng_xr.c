/*
 * eng_xr.c -- the game in a VR headset: see eng_xr.h.
 *
 * One OpenXR session on the window's own OpenGL context (XR_KHR_opengl_enable: WGL on Windows, GLX on Linux), one swapchain per
 * eye. The engine draws each eye into an offscreen RGBA8 picture of ours, which is copied byte for byte into the swapchain image
 * (an sRGB one when the runtime offers it: the engine's pixels are display values already, and a runtime takes a UNORM image as
 * linear light and would brighten it). The two pictures go out as two quad layers at the same pose, one per eye.
 */
#ifndef _WIN32
#define _GNU_SOURCE                              /* RTLD_DEFAULT */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#ifdef _WIN32
#include <unknwn.h>                              /* openxr_platform.h's Win32 part names IUnknown */
#define XR_USE_PLATFORM_WIN32
#else
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_NO_PROTOTYPES
#include "../third_party/openxr/openxr.h"
#include "../third_party/openxr/openxr_platform.h"
#include "eng_cfg.h"
#include "eng_display.h"
#include "eng_xr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8        0x8C43
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB     0x8DB9
#endif

/* ---- the OpenXR entry points, from the loader ------------------------------------------------------------------------------- */
#define XR_FUNCS(F) \
    F(xrDestroyInstance) F(xrGetInstanceProperties) F(xrGetSystem) F(xrGetSystemProperties) F(xrPollEvent) F(xrResultToString) \
    F(xrEnumerateEnvironmentBlendModes) F(xrCreateSession) F(xrDestroySession) F(xrBeginSession) F(xrEndSession) \
    F(xrCreateReferenceSpace) F(xrDestroySpace) F(xrLocateSpace) \
    F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrDestroySwapchain) F(xrEnumerateSwapchainImages) \
    F(xrAcquireSwapchainImage) F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage) \
    F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame) F(xrLocateViews) \
    F(xrStringToPath) F(xrCreateActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) F(xrAttachSessionActionSets) \
    F(xrCreateActionSpace) F(xrSyncActions) F(xrGetActionStateFloat) F(xrGetActionStateBoolean) F(xrGetActionStateVector2f) \
    F(xrApplyHapticFeedback) F(xrGetOpenGLGraphicsRequirementsKHR)
#define XRF(n) static PFN_##n n;
XR_FUNCS(XRF)
#undef XRF
static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;

/* ---- the GL past 1.1 (the Windows build resolves everything at run time; engine/gl_dyn.c) --------------------------------- */
typedef void   (APIENTRY *pfn_gen)(GLsizei, GLuint *);
typedef void   (APIENTRY *pfn_del)(GLsizei, const GLuint *);
typedef void   (APIENTRY *pfn_bind)(GLenum, GLuint);
typedef void   (APIENTRY *pfn_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *pfn_status)(GLenum);
typedef void   (APIENTRY *pfn_blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static pfn_gen gen_fb; static pfn_del del_fb; static pfn_bind bind_fb; static pfn_fbtex fb_tex; static pfn_status fb_status; static pfn_blit blit_fb;
static void *glproc(const char *core, const char *ext) { void *p = SDL_GL_GetProcAddress(core); return p ? p : SDL_GL_GetProcAddress(ext); }
static bool gl_load(void)
{
    gen_fb    = (pfn_gen)glproc("glGenFramebuffers", "glGenFramebuffersEXT");
    del_fb    = (pfn_del)glproc("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    bind_fb   = (pfn_bind)glproc("glBindFramebuffer", "glBindFramebufferEXT");
    fb_tex    = (pfn_fbtex)glproc("glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    fb_status = (pfn_status)glproc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    blit_fb   = (pfn_blit)glproc("glBlitFramebuffer", "glBlitFramebufferEXT");
    return gen_fb && del_fb && bind_fb && fb_tex && fb_status && blit_fb;
}

/* ---- state ------------------------------------------------------------------------------------------------------------------- */
static void *lib;
static XrInstance inst;
static XrSystemId sys;
static XrSession sess;
static XrSpace local_space, view_space;
static XrSessionState sstate = XR_SESSION_STATE_UNKNOWN;
static bool running;                         /* xrBeginSession .. xrEndSession */
static XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
static char runtime_name[XR_MAX_RUNTIME_NAME_SIZE], system_name[XR_MAX_SYSTEM_NAME_SIZE];
static int32_t upm;                          /* the game's units in a metre */
static bool gun_game;

typedef struct { XrSwapchain sc; uint32_t n; XrSwapchainImageOpenGLKHR img[8]; } eye_chain;
static eye_chain chain[2];
static int64_t chain_fmt;
static int eye_w, eye_h;                     /* the swapchains' (and the pictures') size */
static GLuint eye_tex[2], eye_fbo[2], copy_fbo;
static bool have_pics;                       /* the eye pictures hold a frame */
/* THE ROOM behind the screen: one small dark image under both eyes (a projection layer). A runtime may composite nothing without a
 * projection layer at all (the OpenXR Simulator's OpenGL path makes its compositor on the first one), and it is a defined backdrop. */
static eye_chain bg;
#define BG_SIZE 16

static XrFrameState fstate;
static XrTime last_time;                     /* the last predicted display time (0 = no frame yet) */
static bool frame_open, frame_render;

/* the screen: in LOCAL space, facing the player at the last recenter */
static int vr_dist_cm = 150, vr_size = 100, vr_depth = 100;
static double anchor_yaw;
static XrVector3f anchor_pos;
static bool recenter_pending = true;

/* the controllers */
static XrActionSet aset;
static XrAction a_aim, a_trigger, a_squeeze, a_coin, a_recenter, a_haptic, a_menu, a_nav;
static XrPath hand[2];
static XrSpace aim_space[2];
static int gun_hand = 1;                     /* the hand that pulled its trigger last (the right one to begin with) */
static bool gun_ok, gun_in;
static float gun_x = 0.5f, gun_y = 0.5f;
static unsigned buttons;
static char mkeys[8]; static int mk_n;       /* the menu's steps from the controllers, for the host (eng_xr_menu_key) */

/* the menu in the headset: the window's menu drawn into this overlay, then over both eyes' pictures */
static GLuint ov_tex, ov_fbo;
static int ov_w, ov_h;
static bool ov_on;                           /* it holds this frame's menu */

static const char *res_str(XrResult r)
{
    static char b[XR_MAX_RESULT_STRING_SIZE];
    if (inst && xrResultToString && xrResultToString(inst, r, b) == XR_SUCCESS) return b;
    snprintf(b, sizeof b, "XrResult %d", (int)r);
    return b;
}
#define XR_OK(call, what) xr_ok((call), what)
static bool xr_ok(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r)) return true;
    fprintf(stderr, "[VR] %s: %s\n", what, res_str(r));
    return false;
}

/* ---- maths: the screen's frame is the anchor's yaw about +y ------------------------------------------------------------------ */
static XrVector3f rot_y(XrVector3f v, double yaw)          /* R_y(yaw) v */
{
    const double c = cos(yaw), s = sin(yaw);
    return (XrVector3f){ (float)(v.x * c + v.z * s), v.y, (float)(-v.x * s + v.z * c) };
}
static XrVector3f quat_rot(XrQuaternionf q, XrVector3f v)  /* q v q* */
{
    const float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return (XrVector3f){ v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx) };
}
static double scr_w43(void) { return 2.0 * (vr_dist_cm / 100.0) * tan(22.5 * M_PI / 180.0) * vr_size / 100.0; }   /* the 4:3 picture's width (m) */
static double scr_h(void)   { return scr_w43() * 0.75; }
static XrPosef screen_pose(void)
{
    XrPosef p;
    const XrVector3f fwd = rot_y((XrVector3f){ 0, 0, -(float)(vr_dist_cm / 100.0) }, anchor_yaw);
    p.position = (XrVector3f){ anchor_pos.x + fwd.x, anchor_pos.y + fwd.y, anchor_pos.z + fwd.z };
    p.orientation = (XrQuaternionf){ 0, (float)sin(anchor_yaw / 2), 0, (float)cos(anchor_yaw / 2) };
    return p;
}

void eng_xr_stereo(int32_t *sep, int32_t *zconv)
{
    *sep   = (int32_t)(0.064 * upm * vr_depth / 100.0 + 0.5);
    *zconv = (int32_t)(vr_dist_cm / 100.0 * upm + 0.5);
}

/* ---- the loader ---------------------------------------------------------------------------------------------------------------- */
static bool load_loader(void)
{
#ifdef _WIN32
    HMODULE m = LoadLibraryA("openxr_loader.dll");
    if (!m) { fprintf(stderr, "[VR] openxr_loader.dll not found (it ships beside the game's .exe)\n"); return false; }
    lib = m;
    xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(m, "xrGetInstanceProcAddr");
#else
    lib = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "[VR] no OpenXR loader (libopenxr_loader.so.1): install your distribution's openxr package\n"); return false; }
    xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr");
#endif
    if (!xrGetInstanceProcAddr) { fprintf(stderr, "[VR] the OpenXR loader has no xrGetInstanceProcAddr\n"); return false; }
    return true;
}
static void unload_loader(void)
{
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
    lib = NULL;
}

/* ---- the GL binding: the window's context, as each platform names it ----------------------------------------------------------- */
#ifdef _WIN32
typedef HDC   (WINAPI *pfn_wgldc)(void);
typedef HGLRC (WINAPI *pfn_wglrc)(void);
static bool gl_binding(XrGraphicsBindingOpenGLWin32KHR *b)
{
    pfn_wgldc dc = (pfn_wgldc)SDL_GL_GetProcAddress("wglGetCurrentDC");
    pfn_wglrc rc = (pfn_wglrc)SDL_GL_GetProcAddress("wglGetCurrentContext");
    memset(b, 0, sizeof *b);
    b->type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
    b->hDC = dc ? dc() : NULL; b->hGLRC = rc ? rc() : NULL;
    if (!b->hDC || !b->hGLRC) { fprintf(stderr, "[VR] no current WGL context\n"); return false; }
    return true;
}
#else
typedef Display    *(*pfn_gxdpy)(void);
typedef GLXDrawable (*pfn_gxdraw)(void);
typedef GLXContext  (*pfn_gxctx)(void);
typedef int         (*pfn_gxqctx)(Display *, GLXContext, int, int *);
typedef GLXFBConfig*(*pfn_gxcfb)(Display *, int, const int *, int *);
typedef int         (*pfn_gxfba)(Display *, GLXFBConfig, int, int *);
static bool gl_binding(XrGraphicsBindingOpenGLXlibKHR *b)
{
    pfn_gxdpy  gdpy  = (pfn_gxdpy)SDL_GL_GetProcAddress("glXGetCurrentDisplay");
    pfn_gxdraw gdraw = (pfn_gxdraw)SDL_GL_GetProcAddress("glXGetCurrentDrawable");
    pfn_gxctx  gctx  = (pfn_gxctx)SDL_GL_GetProcAddress("glXGetCurrentContext");
    pfn_gxqctx qctx  = (pfn_gxqctx)SDL_GL_GetProcAddress("glXQueryContext");
    pfn_gxcfb  cfb   = (pfn_gxcfb)SDL_GL_GetProcAddress("glXChooseFBConfig");
    pfn_gxfba  fba   = (pfn_gxfba)SDL_GL_GetProcAddress("glXGetFBConfigAttrib");
    memset(b, 0, sizeof *b);
    b->type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
    if (!gdpy || !gdraw || !gctx || !qctx || !cfb || !fba || !gdpy() || !gctx()) {
        fprintf(stderr, "[VR] no current GLX context: OpenXR's OpenGL needs X11 (the window is on %s)\n", SDL_GetCurrentVideoDriver());
        return false;
    }
    Display *d = gdpy(); GLXContext c = gctx();
    int id = 0, screen = 0, n = 0;
    qctx(d, c, GLX_FBCONFIG_ID, &id);
    qctx(d, c, GLX_SCREEN, &screen);
    const int attrs[] = { GLX_FBCONFIG_ID, id, None };
    GLXFBConfig *cfgs = cfb(d, screen, attrs, &n);
    if (!cfgs || n < 1) { fprintf(stderr, "[VR] the context's GLX framebuffer config is not found\n"); return false; }
    int vid = 0; fba(d, cfgs[0], GLX_VISUAL_ID, &vid);
    b->xDisplay = d; b->visualid = (uint32_t)vid; b->glxFBConfig = cfgs[0]; b->glxDrawable = gdraw(); b->glxContext = c;
    { int (*xfree)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "XFree"); if (xfree) xfree(cfgs); }
    return true;
}
#endif

/* ---- the eyes' pictures and swapchains ------------------------------------------------------------------------------------------ */
static void destroy_chains(void)
{
    for (int e = 0; e < 2; e++) {
        if (chain[e].sc) xrDestroySwapchain(chain[e].sc);
        memset(&chain[e], 0, sizeof chain[e]);
        if (eye_fbo[e]) del_fb(1, &eye_fbo[e]);
        if (eye_tex[e]) glDeleteTextures(1, &eye_tex[e]);
        eye_fbo[e] = eye_tex[e] = 0;
    }
    if (copy_fbo) del_fb(1, &copy_fbo);
    copy_fbo = 0; eye_w = eye_h = 0; have_pics = false;
}
static void want_size(int *w, int *h)
{
    *h = 960;                                                    /* twice the board's lines: the screen spans ~45 degrees */
    *w = g_eng_disp.wide ? (*h * 16 + 4) / 9 : *h * 4 / 3;      /* widescreen: a wider screen, the world wider (Hor+) */
}
static bool make_chains(int w, int h)
{
    destroy_chains();
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = chain_fmt; ci.sampleCount = 1; ci.width = (uint32_t)w; ci.height = (uint32_t)h;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        if (!XR_OK(xrCreateSwapchain(sess, &ci, &chain[e].sc), "xrCreateSwapchain")) { destroy_chains(); return false; }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(chain[e].sc, 0, &n, NULL);
        if (n > 8) n = 8;
        for (uint32_t i = 0; i < n; i++) chain[e].img[i] = (XrSwapchainImageOpenGLKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };
        if (!XR_OK(xrEnumerateSwapchainImages(chain[e].sc, n, &n, (XrSwapchainImageBaseHeader *)chain[e].img), "xrEnumerateSwapchainImages")) { destroy_chains(); return false; }
        chain[e].n = n;
        glGenTextures(1, &eye_tex[e]);
        glBindTexture(GL_TEXTURE_2D, eye_tex[e]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gen_fb(1, &eye_fbo[e]);
        bind_fb(GL_FRAMEBUFFER, eye_fbo[e]);
        fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye_tex[e], 0);
        const GLenum st = fb_status(GL_FRAMEBUFFER);
        bind_fb(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[VR] eye picture %dx%d incomplete (0x%x)\n", w, h, st); destroy_chains(); return false; }
    }
    gen_fb(1, &copy_fbo);
    eye_w = w; eye_h = h;
    fprintf(stderr, "[VR] eye pictures %dx%d, swapchain format 0x%llX\n", w, h, (unsigned long long)chain_fmt);
    return true;
}

/* ---- the controllers ---------------------------------------------------------------------------------------------------------- */
static XrPath path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }
static bool make_action(XrAction *a, const char *name, const char *loc, XrActionType type)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", loc);
    ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = hand;
    return XR_OK(xrCreateAction(aset, &ci, a), name);
}
static bool make_actions(void)
{
    hand[0] = path("/user/hand/left"); hand[1] = path("/user/hand/right");
    XrActionSetCreateInfo ci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(ci.actionSetName, sizeof ci.actionSetName, "cabinet");
    snprintf(ci.localizedActionSetName, sizeof ci.localizedActionSetName, "Cabinet");
    if (!XR_OK(xrCreateActionSet(inst, &ci, &aset), "xrCreateActionSet")) return false;
    if (!make_action(&a_aim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT) ||
        !make_action(&a_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make_action(&a_squeeze, "pedal", "Pedal", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make_action(&a_coin, "coin", "Insert coin", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make_action(&a_recenter, "recenter", "Recenter the screen", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make_action(&a_haptic, "recoil", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT) ||
        !make_action(&a_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make_action(&a_nav, "menu_steps", "Menu steps", XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    /* each profile's components under /user/hand/<side>/ ("<" = the left hand's only: a component a profile lacks on one side
     * fails that profile's whole suggestion); a runtime that does not know a profile says so, and that is no failure */
    struct prof { const char *name; const char *comp[8]; } profs[] = {      /* aim, trigger, pedal, coin, recenter, haptic, menu, menu steps */
        { "/interaction_profiles/oculus/touch_controller", { "input/aim/pose", "input/trigger/value", "input/squeeze/value", "*a|x", "*b|y", "output/haptic",
                                                             "<input/menu/click", "input/thumbstick" } },
        { "/interaction_profiles/valve/index_controller",  { "input/aim/pose", "input/trigger/value", "input/squeeze/value", "input/a/click", "input/b/click", "output/haptic",
                                                             "input/thumbstick/click", "input/thumbstick" } },
        { "/interaction_profiles/htc/vive_controller",     { "input/aim/pose", "input/trigger/value", "input/squeeze/click", "input/menu/click", NULL, "output/haptic",
                                                             "input/trackpad/click", "input/trackpad" } },
        { "/interaction_profiles/microsoft/motion_controller", { "input/aim/pose", "input/trigger/value", "input/squeeze/click", "input/menu/click", NULL, "output/haptic",
                                                                 "input/thumbstick/click", "input/thumbstick" } },
        { "/interaction_profiles/khr/simple_controller",   { "input/aim/pose", "input/select/click", "input/menu/click", NULL, NULL, "output/haptic", NULL, NULL } },
    };
    XrAction *acts[8] = { &a_aim, &a_trigger, &a_squeeze, &a_coin, &a_recenter, &a_haptic, &a_menu, &a_nav };
    for (size_t p = 0; p < sizeof profs / sizeof profs[0]; p++) {
        XrActionSuggestedBinding sb[24]; int k = 0;
        for (int a = 0; a < 8; a++) {
            const char *c = profs[p].comp[a];
            if (!c) continue;
            for (int h = 0; h < 2; h++) {
                char full[96];
                if (c[0] == '<') {                                /* Touch: the menu button is the left controller's (the right one's is the system's) */
                    if (h) continue;
                    snprintf(full, sizeof full, "/user/hand/left/%s", c + 1);
                } else if (c[0] == '*') {                         /* Touch: a/b on the right controller, x/y on the left */
                    const char *pick = h ? c + 1 : strchr(c, '|') + 1;
                    snprintf(full, sizeof full, "/user/hand/%s/input/%c/click", h ? "right" : "left", pick[0]);
                } else snprintf(full, sizeof full, "/user/hand/%s/%s", h ? "right" : "left", c);
                sb[k++] = (XrActionSuggestedBinding){ *acts[a], path(full) };
            }
        }
        XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        s.interactionProfile = path(profs[p].name); s.suggestedBindings = sb; s.countSuggestedBindings = (uint32_t)k;
        const XrResult r = xrSuggestInteractionProfileBindings(inst, &s);
        if (XR_FAILED(r)) fprintf(stderr, "[VR] bindings for %s: %s\n", profs[p].name, res_str(r));
    }
    return true;
}
static bool attach_actions(void)
{
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1; ai.actionSets = &aset;
    if (!XR_OK(xrAttachSessionActionSets(sess, &ai), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        si.action = a_aim; si.subactionPath = hand[h]; si.poseInActionSpace.orientation.w = 1;
        if (!XR_OK(xrCreateActionSpace(sess, &si, &aim_space[h]), "xrCreateActionSpace")) return false;
    }
    return true;
}
static float act_float(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = hand[h];
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat(sess, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static bool act_bool(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = hand[h];
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(sess, &gi, &st)) && st.isActive && st.currentState;
}
static XrVector2f act_vec2(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = hand[h];
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    return XR_SUCCEEDED(xrGetActionStateVector2f(sess, &gi, &st)) && st.isActive ? st.currentState : (XrVector2f){ 0, 0 };
}

/* the aim ray against the screen's plane, in the screen's frame (x right, y up, the player on +z) */
static void aim_update(void)
{
    gun_ok = false;
    if (!last_time || !aim_space[gun_hand]) return;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(aim_space[gun_hand], local_space, last_time, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    const XrPosef sp = screen_pose();
    const XrVector3f o = { loc.pose.position.x - sp.position.x, loc.pose.position.y - sp.position.y, loc.pose.position.z - sp.position.z };
    const XrVector3f d = quat_rot(loc.pose.orientation, (XrVector3f){ 0, 0, -1 });
    const XrVector3f lo = rot_y(o, -anchor_yaw), ld = rot_y(d, -anchor_yaw);
    gun_ok = true; gun_in = false;
    if (ld.z >= -1e-4f) return;                           /* pointing away from the screen: off-screen */
    const float t = -lo.z / ld.z;
    if (t <= 0) return;
    const double u = lo.x + t * ld.x, v = lo.y + t * ld.y, w = scr_w43(), hgt = scr_h();
    gun_x = (float)(0.5 + u / w); gun_y = (float)(0.5 - v / hgt);
    gun_in = gun_x >= 0 && gun_x <= 1 && gun_y >= 0 && gun_y <= 1;
}

bool eng_xr_gun(float *nx, float *ny, bool *inside)
{
    if (!running || !gun_game || !gun_ok) return false;
    *nx = gun_x; *ny = gun_y; *inside = gun_in;
    return true;
}
unsigned eng_xr_buttons(void) { return running ? buttons : 0; }
char eng_xr_menu_key(void)
{
    if (!mk_n) return 0;
    const char c = mkeys[0];
    memmove(mkeys, mkeys + 1, (size_t)--mk_n);
    return c;
}
static void mkey(char c) { if (mk_n < (int)sizeof mkeys) mkeys[mk_n++] = c; }
/* the stick as the menu's arrows: a push past 0.6 is one step, held it repeats (after 0.4 s, every 0.12 s), under 0.35 it is let go */
static void menu_stick(float x, float y)
{
    static char held; static Uint32 next;
    const float ax = fabsf(x), ay = fabsf(y), m = ax > ay ? ax : ay;
    if (m < 0.35f) { held = 0; return; }
    const char dir = ax > ay ? (x > 0 ? 'r' : 'l') : (y > 0 ? 'u' : 'd');
    const Uint32 now = SDL_GetTicks();
    if (dir != held) { if (m >= 0.6f) { held = dir; mkey(dir); next = now + 400; } }
    else if ((int32_t)(now - next) >= 0) { mkey(dir); next = now + 120; }
}
void eng_xr_rumble(float amplitude, uint32_t ms)
{
    if (!running || sstate != XR_SESSION_STATE_FOCUSED) return;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = a_haptic; hi.subactionPath = hand[gun_hand];
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.duration = (XrDuration)ms * 1000000; v.frequency = XR_FREQUENCY_UNSPECIFIED; v.amplitude = amplitude < 0 ? 0 : amplitude > 1 ? 1 : amplitude;
    xrApplyHapticFeedback(sess, &hi, (const XrHapticBaseHeader *)&v);
}

/* ---- start / stop ------------------------------------------------------------------------------------------------------------ */
static int gl_version(void)                                  /* major * 100 + minor of the current context */
{
    const char *v = (const char *)glGetString(GL_VERSION);
    int ma = 0, mi = 0;
    if (v) sscanf(v, "%d.%d", &ma, &mi);
    return ma * 100 + mi;
}

bool eng_xr_start(const char *app, int32_t units_per_m, bool light_gun)
{
    upm = units_per_m > 0 ? units_per_m : 15000;
    gun_game = light_gun;
    vr_dist_cm = eng_cfg_int("vr_distance_cm", 150); if (vr_dist_cm < 50 || vr_dist_cm > 500) vr_dist_cm = 150;
    vr_size    = eng_cfg_int("vr_size", 100);        if (vr_size < 40 || vr_size > 250) vr_size = 100;
    vr_depth   = eng_cfg_int("vr_depth", 100);       if (vr_depth < 0 || vr_depth > 300) vr_depth = 100;
    if (!gl_load()) { fprintf(stderr, "[VR] this OpenGL has no framebuffer objects\n"); return false; }
    if (!load_loader()) return false;
    PFN_xrEnumerateInstanceExtensionProperties enum_ext = NULL; PFN_xrCreateInstance create = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enum_ext);
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
    if (!enum_ext || !create) { fprintf(stderr, "[VR] the OpenXR loader is incomplete\n"); eng_xr_stop(); return false; }
    uint32_t n = 0;
    if (XR_FAILED(enum_ext(NULL, 0, &n, NULL)) || !n) { fprintf(stderr, "[VR] no OpenXR runtime is active (start SteamVR, Monado, ... or set one as the system's OpenXR runtime)\n"); eng_xr_stop(); return false; }
    XrExtensionProperties *ext = calloc(n, sizeof *ext);
    bool have_gl = false;
    if (ext) {
        for (uint32_t i = 0; i < n; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        if (XR_SUCCEEDED(enum_ext(NULL, n, &n, ext)))
            for (uint32_t i = 0; i < n; i++) if (!strcmp(ext[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) have_gl = true;
        free(ext);
    }
    if (!have_gl) { fprintf(stderr, "[VR] the OpenXR runtime has no OpenGL support (XR_KHR_opengl_enable)\n"); eng_xr_stop(); return false; }

    const char *exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "%s", app);
    snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "namco22");
    ici.applicationInfo.applicationVersion = 1; ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    if (!XR_OK(create(&ici, &inst), "xrCreateInstance")) { inst = XR_NULL_HANDLE; eng_xr_stop(); return false; }
#define XRF(f) if (XR_FAILED(xrGetInstanceProcAddr(inst, #f, (PFN_xrVoidFunction *)&f)) || !f) { fprintf(stderr, "[VR] the runtime lacks %s\n", #f); eng_xr_stop(); return false; }
    XR_FUNCS(XRF)
#undef XRF
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(inst, &ip))) snprintf(runtime_name, sizeof runtime_name, "%s", ip.runtimeName);

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult rs = xrGetSystem(inst, &sgi, &sys);
    if (XR_FAILED(rs)) { fprintf(stderr, "[VR] %s: no headset (%s)\n", runtime_name, res_str(rs)); eng_xr_stop(); return false; }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(inst, sys, &sp))) snprintf(system_name, sizeof system_name, "%s", sp.systemName);
    { uint32_t nb = 0; XrEnvironmentBlendMode bm[8];
      if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &nb, bm)) && nb) blend = bm[0]; }

    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!XR_OK(xrGetOpenGLGraphicsRequirementsKHR(inst, sys, &req), "xrGetOpenGLGraphicsRequirementsKHR")) { eng_xr_stop(); return false; }
    const int have = gl_version(), need = (int)XR_VERSION_MAJOR(req.minApiVersionSupported) * 100 + (int)XR_VERSION_MINOR(req.minApiVersionSupported);
    if (have < need) { fprintf(stderr, "[VR] the runtime needs OpenGL %d.%d, this context is %d.%d\n", need / 100, need % 100, have / 100, have % 100); eng_xr_stop(); return false; }

#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR gb;
#else
    XrGraphicsBindingOpenGLXlibKHR gb;
#endif
    if (!gl_binding(&gb)) { eng_xr_stop(); return false; }
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = sys;
    if (!XR_OK(xrCreateSession(inst, &sci, &sess), "xrCreateSession")) { sess = XR_NULL_HANDLE; eng_xr_stop(); return false; }
    XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rci.poseInReferenceSpace.orientation.w = 1;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XR_OK(xrCreateReferenceSpace(sess, &rci, &local_space), "LOCAL space")) { eng_xr_stop(); return false; }
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XR_OK(xrCreateReferenceSpace(sess, &rci, &view_space), "VIEW space")) { eng_xr_stop(); return false; }

    int64_t fmts[64]; uint32_t nf = 0;
    if (!XR_OK(xrEnumerateSwapchainFormats(sess, 64, &nf, fmts), "xrEnumerateSwapchainFormats") || !nf) { eng_xr_stop(); return false; }
    chain_fmt = 0;
    for (uint32_t i = 0; i < nf && !chain_fmt; i++) if (fmts[i] == GL_SRGB8_ALPHA8) chain_fmt = fmts[i];
    for (uint32_t i = 0; i < nf && !chain_fmt; i++) if (fmts[i] == GL_RGBA8) chain_fmt = fmts[i];
    if (!chain_fmt) chain_fmt = fmts[0];
    if (chain_fmt != GL_SRGB8_ALPHA8) fprintf(stderr, "[VR] no sRGB swapchain: the picture may look brighter than in the window\n");
    int w, h; want_size(&w, &h);
    if (!make_chains(w, h)) { eng_xr_stop(); return false; }
    {   XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; ci.format = chain_fmt; ci.sampleCount = 1;
        ci.width = ci.height = BG_SIZE; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        uint32_t n = 0;
        if (XR_OK(xrCreateSwapchain(sess, &ci, &bg.sc), "the backdrop's swapchain")) {
            xrEnumerateSwapchainImages(bg.sc, 0, &n, NULL);
            if (n > 8) n = 8;
            for (uint32_t i = 0; i < n; i++) bg.img[i] = (XrSwapchainImageOpenGLKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR };
            if (XR_SUCCEEDED(xrEnumerateSwapchainImages(bg.sc, n, &n, (XrSwapchainImageBaseHeader *)bg.img))) bg.n = n;
        } }
    if (!make_actions() || !attach_actions()) { eng_xr_stop(); return false; }
    fprintf(stderr, "[VR] %s, %s: a %.2f m screen at %.2f m, 3D depth %d %% (%d units a metre)\n", runtime_name, system_name,
            scr_w43() * eye_w / (eye_h * 4.0 / 3.0), vr_dist_cm / 100.0, vr_depth, upm);
    return true;
}

void eng_xr_stop(void)
{
    if (sess) {
        if (running) xrEndSession(sess);
        destroy_chains();
        if (bg.sc) xrDestroySwapchain(bg.sc);
        memset(&bg, 0, sizeof bg);
        for (int h = 0; h < 2; h++) if (aim_space[h]) xrDestroySpace(aim_space[h]);
        if (view_space) xrDestroySpace(view_space);
        if (local_space) xrDestroySpace(local_space);
        xrDestroySession(sess);                      /* the action set goes with the instance */
    }
    if (inst) xrDestroyInstance(inst);
    sess = XR_NULL_HANDLE; inst = XR_NULL_HANDLE; local_space = view_space = XR_NULL_HANDLE;
    aim_space[0] = aim_space[1] = XR_NULL_HANDLE; aset = XR_NULL_HANDLE;
    running = false; frame_open = false; last_time = 0; gun_ok = false; buttons = 0; mk_n = 0;
    if (ov_fbo) del_fb(1, &ov_fbo);
    if (ov_tex) glDeleteTextures(1, &ov_tex);
    ov_fbo = ov_tex = 0; ov_w = ov_h = 0; ov_on = false;
    unload_loader();
}

bool eng_xr_running(void) { return sess && running; }

/* ---- the runtime's events, the controllers ------------------------------------------------------------------------------------ */
static const char *state_name(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "idle"; case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized"; case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused"; case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "lost"; case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "starting";
    }
}

void eng_xr_poll(void)
{
    if (!inst) return;
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    bool lost = false;
    while (inst && xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            sstate = sc->state;
            fprintf(stderr, "[VR] session %s\n", state_name(sstate));
            if (sstate == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running = XR_OK(xrBeginSession(sess, &bi), "xrBeginSession");
                recenter_pending = true;
            } else if (sstate == XR_SESSION_STATE_STOPPING && running) {
                xrEndSession(sess); running = false;
            } else if (sstate == XR_SESSION_STATE_EXITING || sstate == XR_SESSION_STATE_LOSS_PENDING) lost = true;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) lost = true;
        else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) recenter_pending = true;
        ev = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
    }
    if (lost) { fprintf(stderr, "[VR] the headset session ended: the game goes on in the window\n"); eng_xr_stop(); return; }

    buttons = 0;
    if (!running || sstate != XR_SESSION_STATE_FOCUSED) { gun_ok = false; return; }
    XrActiveActionSet as = { aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(sess, &si))) { gun_ok = false; return; }
    static bool trig_prev[2], cen_prev, menu_prev, ok_prev;
    bool cen = false, menu = false;
    float sx = 0, sy = 0;
    for (int h = 0; h < 2; h++) {
        const bool t = act_float(a_trigger, h) > 0.5f;
        if (t && !trig_prev[h]) gun_hand = h;           /* the gun is in the hand that fired last */
        trig_prev[h] = t;
        if (t) buttons |= ENG_XR_TRIGGER;
        if (act_float(a_squeeze, h) > 0.5f) buttons |= ENG_XR_PEDAL;
        if (act_bool(a_coin, h)) buttons |= ENG_XR_COIN;
        cen = cen || act_bool(a_recenter, h);
        menu = menu || act_bool(a_menu, h);
        const XrVector2f s = act_vec2(a_nav, h);
        if (s.x * s.x + s.y * s.y > sx * sx + sy * sy) { sx = s.x; sy = s.y; }
    }
    /* THE MENU: its button opens it (the host does, 'm'); while it is open the stick steps, trigger / A is OK, B / Y and the menu
     * button go back, and none of them reaches the game (the host does not read the controls while the menu is open) */
    const bool open = eng_ui_is_open(), ok = (buttons & (ENG_XR_TRIGGER | ENG_XR_COIN)) != 0;
    if (menu && !menu_prev) mkey(open ? 'b' : 'm');
    if (open) {
        if (ok && !ok_prev) mkey('o');
        if (cen && !cen_prev) mkey('b');
        menu_stick(sx, sy);
    } else {
        if (cen && !cen_prev) recenter_pending = true;
        menu_stick(0, 0);
    }
    cen_prev = cen; menu_prev = menu; ok_prev = ok;
    aim_update();
}

/* ---- frames ------------------------------------------------------------------------------------------------------------------- */
static void recenter(XrTime t)
{
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(view_space, local_space, t, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    const XrVector3f f = quat_rot(loc.pose.orientation, (XrVector3f){ 0, 0, -1 });
    anchor_yaw = atan2(-f.x, -f.z);
    anchor_pos = loc.pose.position;
    recenter_pending = false;
    fprintf(stderr, "[VR] screen in front of you: yaw %.0f deg, eyes at %.2f %.2f %.2f m\n", anchor_yaw * 180 / M_PI, anchor_pos.x, anchor_pos.y, anchor_pos.z);
}

bool eng_xr_frame_begin(int *w, int *h)
{
    frame_open = frame_render = ov_on = false;
    if (!running) return false;
    fstate = (XrFrameState){ XR_TYPE_FRAME_STATE };
    if (!XR_OK(xrWaitFrame(sess, NULL, &fstate), "xrWaitFrame")) return false;
    if (!XR_OK(xrBeginFrame(sess, NULL), "xrBeginFrame")) return false;
    frame_open = true;
    last_time = fstate.predictedDisplayTime;
    if (recenter_pending) recenter(fstate.predictedDisplayTime);
    int ww, wh; want_size(&ww, &wh);
    if ((ww != eye_w || wh != eye_h) && !make_chains(ww, wh)) return false;   /* widescreen switched: new pictures */
    frame_render = fstate.shouldRender && eye_w > 0;
    *w = eye_w; *h = eye_h;
    return frame_render;
}

void eng_xr_eye_target(int eye)
{
    bind_fb(GL_FRAMEBUFFER, eye_fbo[eye & 1]);
    glViewport(0, 0, eye_w, eye_h);
}

bool eng_xr_overlay_begin(int w, int h)
{
    ov_on = false;
    if (!frame_render || w < 1 || h < 1) return false;
    if (w != ov_w || h != ov_h) {                    /* the window's size: the menu lays itself out in it */
        if (!ov_tex) glGenTextures(1, &ov_tex);
        glBindTexture(GL_TEXTURE_2D, ov_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (!ov_fbo) gen_fb(1, &ov_fbo);
        bind_fb(GL_FRAMEBUFFER, ov_fbo);
        fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ov_tex, 0);
        const GLenum st = fb_status(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[VR] menu overlay %dx%d incomplete (0x%x)\n", w, h, st); bind_fb(GL_FRAMEBUFFER, 0); ov_w = ov_h = 0; return false; }
        ov_w = w; ov_h = h;
    }
    bind_fb(GL_FRAMEBUFFER, ov_fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return ov_on = true;
}
void eng_xr_overlay_end(void) { bind_fb(GL_FRAMEBUFFER, 0); }

/* The menu draws itself with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) into the clear overlay, so its colour is premultiplied: ONE over the
 * picture. (Its alpha is squared on a translucent pixel, so a see-through panel shows a little more of the game than in the window.) */
void eng_xr_overlay_draw(void)
{
    if (!ov_on) return;
    glViewport(0, 0, eye_w, eye_h);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT);
    glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, ov_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

/* the room: the eyes' views this frame, both looking at the dark image */
static bool backdrop(XrCompositionLayerProjection *pl, XrCompositionLayerProjectionView pv[2])
{
    if (!bg.sc || !bg.n) return false;
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fstate.predictedDisplayTime; li.space = local_space;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(sess, &li, &vs, 2, &n, v)) || n != 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(bg.sc, &ai, &idx))) return false;
    const bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(bg.sc, &wi)) && idx < bg.n;
    if (ok) {
        bind_fb(GL_FRAMEBUFFER, copy_fbo);
        fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bg.img[idx].image, 0);
        glViewport(0, 0, BG_SIZE, BG_SIZE);
        glClearColor(10 / 255.0f, 10 / 255.0f, 12 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        bind_fb(GL_FRAMEBUFFER, 0);
    }
    xrReleaseSwapchainImage(bg.sc, &ri);
    if (!ok) return false;
    for (int e = 0; e < 2; e++) {
        pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        pv[e].pose = v[e].pose; pv[e].fov = v[e].fov;
        pv[e].subImage.swapchain = bg.sc;
        pv[e].subImage.imageRect.extent.width = BG_SIZE; pv[e].subImage.imageRect.extent.height = BG_SIZE;
    }
    *pl = (XrCompositionLayerProjection){ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    pl->space = local_space; pl->viewCount = 2; pl->views = pv;
    return true;
}

void eng_xr_frame_end(void)
{
    if (!frame_open) return;
    frame_open = false;
    XrCompositionLayerQuad q[2];
    XrCompositionLayerProjection room;
    XrCompositionLayerProjectionView room_v[2];
    const XrCompositionLayerBaseHeader *layers[3];
    uint32_t nl = 0;
    if (frame_render) {
        have_pics = true;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);              /* a byte copy: the pictures are display values already */
        bool ok = true;
        for (int e = 0; e < 2 && ok; e++) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            if (!XR_OK(xrAcquireSwapchainImage(chain[e].sc, &ai, &idx), "xrAcquireSwapchainImage")) { ok = false; break; }
            if (XR_OK(xrWaitSwapchainImage(chain[e].sc, &wi), "xrWaitSwapchainImage") && idx < chain[e].n) {
                bind_fb(GL_READ_FRAMEBUFFER, eye_fbo[e]);
                bind_fb(GL_DRAW_FRAMEBUFFER, copy_fbo);
                fb_tex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, chain[e].img[idx].image, 0);
                blit_fb(0, 0, eye_w, eye_h, 0, 0, eye_w, eye_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                fb_tex(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            } else ok = false;
            xrReleaseSwapchainImage(chain[e].sc, &ri);
        }
        bind_fb(GL_FRAMEBUFFER, 0);
        if (ok && backdrop(&room, room_v)) layers[nl++] = (const XrCompositionLayerBaseHeader *)&room;
        if (ok) {
            const XrPosef pose = screen_pose();
            const float hgt = (float)scr_h(), wid = hgt * (float)eye_w / (float)eye_h;
            for (int e = 0; e < 2; e++) {
                q[e] = (XrCompositionLayerQuad){ XR_TYPE_COMPOSITION_LAYER_QUAD };
                q[e].space = local_space;
                q[e].eyeVisibility = e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                q[e].subImage.swapchain = chain[e].sc;
                q[e].subImage.imageRect.extent.width = eye_w; q[e].subImage.imageRect.extent.height = eye_h;
                q[e].pose = pose;
                q[e].size = (XrExtent2Df){ wid, hgt };
                layers[nl++] = (const XrCompositionLayerBaseHeader *)&q[e];
            }
        }
    }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fstate.predictedDisplayTime; ei.environmentBlendMode = blend;
    ei.layerCount = nl; ei.layers = nl ? layers : NULL;
    XR_OK(xrEndFrame(sess, &ei), "xrEndFrame");
}

void eng_xr_mirror(int x, int y, int w, int h, bool sharp)
{
    int dw, dh; SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &dw, &dh);
    bind_fb(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!have_pics) return;
    bind_fb(GL_READ_FRAMEBUFFER, eye_fbo[0]);
    bind_fb(GL_DRAW_FRAMEBUFFER, 0);
    const int gy = dh - (y + h);                      /* GL's origin is bottom-left */
    blit_fb(0, 0, eye_w, eye_h, x, gy, x + w, gy + h, GL_COLOR_BUFFER_BIT, sharp ? GL_NEAREST : GL_LINEAR);
    bind_fb(GL_FRAMEBUFFER, 0);
}

bool eng_xr_read_eye(int eye)
{
    if (!have_pics) return false;
    bind_fb(GL_READ_FRAMEBUFFER, eye_fbo[eye & 1]);
    return true;
}
void eng_xr_read_done(void) { bind_fb(GL_FRAMEBUFFER, 0); }

/* ---- the menu's VR page ------------------------------------------------------------------------------------------------------- */
enum { V_DIST, V_SIZE, V_DEPTH, V_CENTER, V_N };
static int vr_n(void) { return V_N; }
static bool vr_val(int r) { return r != V_CENTER; }
static void vr_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0;
    switch (r) {
    case V_DIST:   snprintf(l, ln, "Screen distance"); snprintf(v, vn, "%.2f m", vr_dist_cm / 100.0); break;
    case V_SIZE:   snprintf(l, ln, "Screen size"); snprintf(v, vn, "%d %%  (%.2f m wide)", vr_size, scr_h() * (eye_h ? (double)eye_w / eye_h : 4.0 / 3.0)); break;
    case V_DEPTH:  snprintf(l, ln, "3D depth"); if (vr_depth) snprintf(v, vn, "%d %%", vr_depth); else snprintf(v, vn, "OFF (flat)"); break;
    case V_CENTER: snprintf(l, ln, "Screen"); snprintf(v, vn, "Recenter in front of you"); break;
    }
}
static void vr_change(int r, int dir)
{
    const int d = dir ? dir : 1;
    switch (r) {
    case V_DIST:   vr_dist_cm += 25 * d; if (vr_dist_cm < 50) vr_dist_cm = 50; if (vr_dist_cm > 500) vr_dist_cm = 500; eng_cfg_set_int("vr_distance_cm", vr_dist_cm); break;
    case V_SIZE:   vr_size += 10 * d; if (vr_size < 40) vr_size = 40; if (vr_size > 250) vr_size = 250; eng_cfg_set_int("vr_size", vr_size); break;
    case V_DEPTH:  vr_depth += 10 * d; if (vr_depth < 0) vr_depth = 0; if (vr_depth > 300) vr_depth = 300; eng_cfg_set_int("vr_depth", vr_depth); break;
    case V_CENTER: if (dir == 0) recenter_pending = true; break;
    }
}
static void vr_notes(void (*line)(const char *fmt, ...))
{
    line("%s%s%s: %s", runtime_name[0] ? runtime_name : "no OpenXR runtime", system_name[0] ? ", " : "", system_name,
         sess ? state_name(sstate) : "off (start with --vr)");
    line("%s", gun_game ? "Trigger: shoot  Grip: pedal  A/X: coin  B/Y: recenter" : "A/X: coin  B/Y: recenter the screen");
    line("Menu: menu button (Index/WMR: stick click, Vive: pad)");   /* in it: the stick, trigger / A = OK, B / Y = back */
}
static const eng_ui_page vr_page = { "VR", 520, 150, 0, vr_n, vr_val, NULL, vr_text, vr_change, vr_notes };
const eng_ui_page *eng_xr_page(void) { return &vr_page; }
