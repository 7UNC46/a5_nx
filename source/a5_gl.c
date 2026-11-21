/* a5_gl.c -- the GL context Asphalt5GLSurfaceView would have made.
 *
 * On a phone GLSurfaceView makes an EGL window surface and an OpenGL ES 1.x
 * context (the engine links libGLESv1_CM only), then calls into the engine
 * from its render thread; the engine never calls EGL itself. So the port
 * makes them once, on the thread that then runs the engine, before
 * Asphalt5Renderer.nativeInit (importGLInit, the first GL calls).
 *
 * Through the runtime's b_egl* entry points (gl_mesa.c / gl_null.c), as in
 * a6_nx. MIT.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_boost.h"
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.h */
#include "gl_layer.h"
#include "rt_window.h"
#include "util.h"

#include "a5.h"

typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
#define EGL_ALPHA_SIZE             0x3021
#define EGL_BLUE_SIZE              0x3022
#define EGL_GREEN_SIZE             0x3023
#define EGL_RED_SIZE               0x3024
#define EGL_DEPTH_SIZE             0x3025
#define EGL_SURFACE_TYPE           0x3033
#define EGL_NONE                   0x3038
#define EGL_RENDERABLE_TYPE        0x3040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WINDOW_BIT             0x0004
#define EGL_OPENGL_ES_BIT          0x0001

void *b_eglGetDisplay(void *native);
EGLBoolean b_eglInitialize(void *dpy, EGLint *major, EGLint *minor);
EGLint b_eglGetError(void);
EGLBoolean b_eglChooseConfig(void *dpy, const EGLint *attrs, void **configs, EGLint cap, EGLint *num);
void *b_eglCreateWindowSurface(void *dpy, void *cfg, void *win, const EGLint *attrs);
void *b_eglCreateContext(void *dpy, void *cfg, void *share, const EGLint *attrs);
EGLBoolean b_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
EGLBoolean b_eglSwapInterval(void *dpy, EGLint interval);
EGLBoolean b_eglSwapBuffers(void *dpy, void *surface);

static void *s_dpy, *s_surf, *s_ctx;

int a5_gl_init(void) {
  s_dpy = b_eglGetDisplay(NULL);
  if (!s_dpy || !b_eglInitialize(s_dpy, NULL, NULL)) {
    debugPrintf("[egl] no display (0x%x)\n", (unsigned)b_eglGetError());
    return -1;
  }
  /* GLSurfaceView's default for GLES 1: RGB565 + depth 16; RGBA8888 + depth
   * 24 is what the Switch's window configs have and costs nothing. */
  static const EGLint k_cfg[2][16] = {
      {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE},
      {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 16, EGL_NONE},
  };
  void *cfg = NULL;
  EGLint n = 0;
  for (int i = 0; i < 2 && n < 1; i++)
    if (!b_eglChooseConfig(s_dpy, k_cfg[i], &cfg, 1, &n))
      n = 0;
  if (n < 1) {
    debugPrintf("[egl] no GLES 1 config (0x%x)\n", (unsigned)b_eglGetError());
    return -1;
  }
  s_surf = b_eglCreateWindowSurface(s_dpy, cfg, nwindowGetDefault(), NULL);
  if (!s_surf) {
    debugPrintf("[egl] no window surface (0x%x)\n", (unsigned)b_eglGetError());
    return -1;
  }
  static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  s_ctx = b_eglCreateContext(s_dpy, cfg, NULL, ctx_attrs);
  if (!s_ctx || !b_eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
    debugPrintf("[egl] no GLES 1 context (0x%x)\n", (unsigned)b_eglGetError());
    return -1;
  }
  b_eglSwapInterval(s_dpy, g_a5_cfg.fps60 ? 1 : 2);
  int w = 0, h = 0;
  dcr_window_size(&w, &h);
  debugPrintf("[egl] GLES 1.x context, %dx%d, %d fps%s\n", w, h, g_a5_cfg.fps60 ? 60 : 30,
              DCR_GL_MESA ? "" : " (null renderer: nothing is drawn)");
  return 0;
}

void a5_gl_swap(void) {
  static int first = 1;
  if (first) {
    first = 0;
    dcr_boost_first_picture();
  }
  b_eglSwapBuffers(s_dpy, s_surf);
  dcr_boost_launch_tick();
}

/* ------------------------------------------------------------ matrix palette
 * libasphalt5.so draws its animated meshes -- the cars, and by the look of the
 * hardware logs most of the world -- with OES_matrix_palette (GPU skinning on
 * 2010 phones): gxAniGroup::renderDList_v2 switches glMatrixMode to
 * GL_MATRIX_PALETTE_OES, loads one matrix per bone with glLoadMatrixf (each
 * one a full object -> eye matrix that replaces the modelview), and
 * AniObj_v4::SendBuffersToGPU hands GL a bone index and a weight per vertex.
 *
 * Mesa's GLES 1 has no such extension. Its glMatrixMode refused the mode, so
 * every bone matrix landed in the MODELVIEW matrix instead, and the meshes drew
 * with a wrong camera: eye-space z of about -0.02 against a near plane of 32,
 * the whole 3D world clipped away (draw probe, hardware 2026-11-19).
 *
 * Emulated here per the extension's spec: the palette mode, the matrices, the
 * two pointers and the enables are kept on the CPU and never reach Mesa; a draw
 * with GL_MATRIX_PALETTE_OES enabled skins its vertices (and normals) on the
 * CPU into eye space -- v' = sum_i w_i * M[idx_i] * v -- and draws them with an
 * identity modelview. Lights and fog already live in eye space, so nothing
 * else changes. */
#define GL_MATRIX_PALETTE_OES      0x8840
#define GL_MATRIX_INDEX_ARRAY_OES  0x8844
#define GL_WEIGHT_ARRAY_OES        0x86AD
#define GL_MODELVIEW_              0x1700
#define GL_NORMAL_ARRAY_           0x8075
#define PAL_MAX 64

static float g_pal[PAL_MAX][16];
static uint32_t g_pal_cur;
static int g_pal_mode;   /* glMatrixMode(GL_MATRIX_PALETTE_OES) is current */
static int g_pal_on;     /* glEnable(GL_MATRIX_PALETTE_OES) */
static uint32_t g_real_mode = GL_MODELVIEW_; /* the mode Mesa really has */
typedef struct {
  const void *p;
  int32_t size, stride;
  uint32_t type;
} Arr;
static Arr g_mi, g_wt; /* matrix index / weight arrays */
static int g_normals_on;

static int first_time_pal(int k, int32_t size, uint32_t type) {
  static uint32_t seen[16];
  static int n;
  uint32_t key = (uint32_t)k << 28 ^ type << 8 ^ (uint32_t)size;
  for (int i = 0; i < n; i++)
    if (seen[i] == key)
      return 0;
  if (n < 16)
    seen[n++] = key;
  return 1;
}

static void st_palette(uint32_t idx) {
  g_pal_cur = idx < PAL_MAX ? idx : PAL_MAX - 1;
  static int told;
  if (!told++)
    debugPrintf("[gl] matrix palette: emulated on the CPU (first glCurrentPaletteMatrixOES(%u))\n", (unsigned)idx);
}
static void st_index_ptr(int32_t size, uint32_t type, int32_t stride, const void *p) {
  g_mi = (Arr){p, size, stride, type};
  if (first_time_pal(1, size, type))
    debugPrintf("[gl] glMatrixIndexPointerOES(size %d, type 0x%x, stride %d)\n", (int)size, (unsigned)type, (int)stride);
}
static void st_weight_ptr(int32_t size, uint32_t type, int32_t stride, const void *p) {
  g_wt = (Arr){p, size, stride, type};
  if (first_time_pal(2, size, type))
    debugPrintf("[gl] glWeightPointerOES(size %d, type 0x%x, stride %d)\n", (int)size, (unsigned)type, (int)stride);
}

static void w_compressed(uint32_t target, int32_t level, uint32_t fmt, int32_t w, int32_t h, int32_t border,
                         int32_t size, const void *data);
static void *r_tex_ptr, *r_ctex_ptr;


/* ------------------------------------------------------------ 3D bring-up
 * The race's 3D world (and the menus' 3D backdrops) do not show while the 2D
 * HUD does (hardware 2026-11-19). These wrappers log, once per distinct
 * combination, the array types the engine hands GL, the caps it enables and
 * the matrix modes it picks, and every GL error a draw or a state call
 * leaves (the first 48). */
typedef uint32_t (*fn_err)(void);
static fn_err g_get_error;
static void *r_vp, *r_np, *r_tp, *r_cp, *r_de, *r_en, *r_mm;

static uint32_t gl_err(void) {
  if (!g_get_error)
    g_get_error = (fn_err)dcr_gl_lookup("glGetError");
  return g_get_error ? g_get_error() : 0;
}

static void report_err(const char *what, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t e = gl_err();
  static int told;
  if (e && told++ < 48)
    debugPrintf("[gl3d] %s(0x%x, 0x%x, 0x%x) -> GL error 0x%x\n", what, (unsigned)a, (unsigned)b, (unsigned)c,
                (unsigned)e);
}

/* "seen once" set of small keys */
static int first_time(uint32_t key) {
  static uint32_t seen[128];
  static int n;
  for (int i = 0; i < n; i++)
    if (seen[i] == key)
      return 0;
  if (n < 128)
    seen[n++] = key;
  return 1;
}

#define PTR_WRAP(fn, rp, tag)                                                                         \
  static void fn(int32_t size, uint32_t type, int32_t stride, const void *p) {                       \
    ((void (*)(int32_t, uint32_t, int32_t, const void *))rp)(size, type, stride, p);                  \
    if (first_time((uint32_t)tag << 24 ^ type << 8 ^ (uint32_t)size))                                \
      debugPrintf("[gl3d] " #fn ": size %d, type 0x%x, stride %d\n", (int)size, (unsigned)type, (int)stride); \
    report_err(#fn, size, type, stride);                                                              \
  }
static const void *g_vp;
static int32_t g_vp_size, g_vp_stride;
static uint32_t g_vp_type;
static void w_vertex_ptr(int32_t size, uint32_t type, int32_t stride, const void *p) {
  g_vp = p, g_vp_size = size, g_vp_type = type, g_vp_stride = stride;
  ((void (*)(int32_t, uint32_t, int32_t, const void *))r_vp)(size, type, stride, p);
  if (first_time(1u << 24 ^ type << 8 ^ (uint32_t)size ^ (uint32_t)stride << 4))
    debugPrintf("[gl3d] glVertexPointer: size %d, type 0x%x, stride %d\n", (int)size, (unsigned)type, (int)stride);
  report_err("glVertexPointer", size, type, stride);
}
PTR_WRAP(w_texcoord_ptr, r_tp, 2)
PTR_WRAP(w_color_ptr, r_cp, 3)

static Arr g_np;
static void w_normal_ptr(uint32_t type, int32_t stride, const void *p) {
  g_np = (Arr){p, 3, stride, type};
  ((void (*)(uint32_t, int32_t, const void *))r_np)(type, stride, p);
  if (first_time(4u << 24 ^ type))
    debugPrintf("[gl3d] glNormalPointer: type 0x%x, stride %d\n", (unsigned)type, (int)stride);
  report_err("glNormalPointer", type, stride, 0);
}

/* ------------------------------------------------------------ CPU skinning */
static int type_size(uint32_t t) {
  switch (t) {
  case 0x1400: case 0x1401: return 1; /* (UNSIGNED_)BYTE */
  case 0x1402: case 0x1403: return 2; /* (UNSIGNED_)SHORT */
  default: return 4;                  /* FLOAT, FIXED */
  }
}
static float rd(const uint8_t *p, uint32_t t, int i) {
  switch (t) {
  case 0x1400: return (float)((const int8_t *)p)[i];
  case 0x1401: return (float)p[i];
  case 0x1402: return (float)((const int16_t *)p)[i];
  case 0x1403: return (float)((const uint16_t *)p)[i];
  case 0x140C: return (float)((const int32_t *)p)[i] * (1.0f / 65536.0f); /* FIXED */
  default: return ((const float *)p)[i];
  }
}
static const uint8_t *elem(const Arr *a, uint32_t v) {
  int st = a->stride ? a->stride : a->size * type_size(a->type);
  return (const uint8_t *)a->p + (size_t)v * (size_t)st;
}

static float *g_spos, *g_snrm;
static uint32_t g_scap;

/* Skins vertices lo..hi into g_spos/g_snrm (indexed like the source arrays).
 * Returns 0 if the arrays are not ones this can read. */
static int skin_range(uint32_t lo, uint32_t hi) {
  if (!g_vp || !g_mi.p || !g_wt.p || hi >= (1u << 20))
    return 0;
  if (hi + 1 > g_scap) {
    uint32_t n = (hi + 1) + (hi + 1) / 2;
    float *a = realloc(g_spos, (size_t)n * 3 * sizeof(float)), *b = a ? realloc(g_snrm, (size_t)n * 3 * sizeof(float)) : NULL;
    if (a)
      g_spos = a;
    if (!a || !b)
      return 0;
    g_snrm = b, g_scap = n;
  }
  int nb = g_mi.size < g_wt.size ? g_mi.size : g_wt.size;
  if (nb < 1)
    nb = 1;
  if (nb > 4)
    nb = 4;
  Arr vp = {g_vp, g_vp_size, g_vp_stride, g_vp_type};
  int nrm = g_normals_on && g_np.p;
  for (uint32_t v = lo; v <= hi; v++) {
    const uint8_t *pv = elem(&vp, v), *pi = elem(&g_mi, v), *pw = elem(&g_wt, v);
    float x = rd(pv, vp.type, 0), y = rd(pv, vp.type, 1), z = vp.size > 2 ? rd(pv, vp.type, 2) : 0.0f;
    float nx = 0, ny = 0, nz = 0;
    if (nrm) {
      const uint8_t *pn = elem(&g_np, v);
      nx = rd(pn, g_np.type, 0), ny = rd(pn, g_np.type, 1), nz = rd(pn, g_np.type, 2);
    }
    float o[3] = {0, 0, 0}, on[3] = {0, 0, 0};
    for (int b = 0; b < nb; b++) {
      uint32_t i = (uint32_t)rd(pi, g_mi.type, b);
      float w = rd(pw, g_wt.type, b);
      if (w == 0.0f)
        continue;
      const float *m = g_pal[i < PAL_MAX ? i : 0];
      for (int r = 0; r < 3; r++) {
        o[r] += w * (m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r]);
        if (nrm)
          on[r] += w * (m[r] * nx + m[4 + r] * ny + m[8 + r] * nz);
      }
    }
    float *d = g_spos + (size_t)v * 3;
    d[0] = o[0], d[1] = o[1], d[2] = o[2];
    if (nrm) {
      float l = on[0] * on[0] + on[1] * on[1] + on[2] * on[2];
      l = l > 0.0f ? 1.0f / sqrtf(l) : 0.0f;
      float *e = g_snrm + (size_t)v * 3;
      e[0] = on[0] * l, e[1] = on[1] * l, e[2] = on[2] * l;
    }
  }
  return 1;
}

static void *r_push, *r_pop, *r_li, *r_lm;
static const float k_ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/* Runs draw() with the skinned arrays and an identity modelview, then puts
 * the engine's arrays and matrices back. */
static int skinned_draw(uint32_t lo, uint32_t hi, void (*draw)(const void *), const void *arg) {
  if (!skin_range(lo, hi))
    return 0;
  if (!r_push) {
    r_push = (void *)dcr_gl_lookup("glPushMatrix");
    r_pop = (void *)dcr_gl_lookup("glPopMatrix");
  }
  if (!r_push || !r_pop || !r_lm || !r_mm)
    return 0;
  static int told;
  if (told++ < 3) {
    const float *d = g_spos + (size_t)lo * 3;
    debugPrintf("[gl] skinned draw: vertices %u..%u, first in eye space (%g %g %g)\n", (unsigned)lo, (unsigned)hi,
                (double)d[0], (double)d[1], (double)d[2]);
  }
  void (*mm)(uint32_t) = (void (*)(uint32_t))r_mm;
  if (g_real_mode != GL_MODELVIEW_)
    mm(GL_MODELVIEW_);
  ((void (*)(void))r_push)();
  ((void (*)(const float *))r_lm)(k_ident);
  ((void (*)(int32_t, uint32_t, int32_t, const void *))r_vp)(3, 0x1406, 0, g_spos);
  if (g_normals_on && g_np.p && r_np)
    ((void (*)(uint32_t, int32_t, const void *))r_np)(0x1406, 0, g_snrm);
  draw(arg);
  ((void (*)(int32_t, uint32_t, int32_t, const void *))r_vp)(g_vp_size, g_vp_type, g_vp_stride, g_vp);
  if (g_normals_on && g_np.p && r_np)
    ((void (*)(uint32_t, int32_t, const void *))r_np)(g_np.type, g_np.stride, g_np.p);
  ((void (*)(void))r_pop)();
  if (g_real_mode != GL_MODELVIEW_)
    mm(g_real_mode);
  return 1;
}

/* For big draws (the 3D world's), now and then: what GL really has --
 * client arrays on, a buffer bound, depth bits -- and where the first
 * vertices land: object -> eye (modelview) -> clip (projection) -> screen. */
static void probe_draw(int32_t count, uint32_t type, const void *idx) {
  static int probes;
  static uint64_t big;
  if (count < 30 || probes >= 8 || (big++ % 400) != 0)
    return;
  probes++;
  uint8_t (*is_on)(uint32_t) = (uint8_t(*)(uint32_t))dcr_gl_lookup("glIsEnabled");
  void (*get_iv)(uint32_t, int32_t *) = (void (*)(uint32_t, int32_t *))dcr_gl_lookup("glGetIntegerv");
  void (*get_fv)(uint32_t, float *) = (void (*)(uint32_t, float *))dcr_gl_lookup("glGetFloatv");
  int32_t vbo = -1, depth = -1, vp[4] = {0};
  float mv[16] = {0}, pr[16] = {0}, col[4] = {0};
  if (get_iv) {
    get_iv(0x8894, &vbo);  /* GL_ARRAY_BUFFER_BINDING */
    get_iv(0x0D56, &depth); /* GL_DEPTH_BITS */
    get_iv(0x0BA2, vp);     /* GL_VIEWPORT */
  }
  if (get_fv) {
    get_fv(0x0BA6, mv); /* GL_MODELVIEW_MATRIX */
    get_fv(0x0BA7, pr); /* GL_PROJECTION_MATRIX */
    get_fv(0x0B00, col); /* GL_CURRENT_COLOR */
  }
  debugPrintf("[probe] draw of %d indices (type 0x%x): arrays vertex %d normal %d color %d texcoord %d; buffer %d; "
              "depth bits %d; viewport %d,%d %dx%d; colour %.2f %.2f %.2f %.2f; lighting %d texture %d depth %d\n",
              (int)count, (unsigned)type, is_on ? is_on(0x8074) : -1, is_on ? is_on(0x8075) : -1,
              is_on ? is_on(0x8076) : -1, is_on ? is_on(0x8078) : -1, (int)vbo, (int)depth, (int)vp[0], (int)vp[1],
              (int)vp[2], (int)vp[3], (double)col[0], (double)col[1], (double)col[2], (double)col[3],
              is_on ? is_on(0x0B50) : -1, is_on ? is_on(0x0DE1) : -1, is_on ? is_on(0x0B71) : -1);
  if (!g_vp || vbo > 0 || g_vp_type != 0x1406 || !idx || type != 0x1403)
    return;
  const uint16_t *ix = idx;
  for (int k = 0; k < 3; k++) {
    const float *v = (const float *)((const uint8_t *)g_vp + (size_t)ix[k] * (g_vp_stride ? g_vp_stride : 4 * g_vp_size));
    float o[4] = {v[0], v[1], g_vp_size > 2 ? v[2] : 0.0f, 1.0f}, e[4], c[4];
    for (int r = 0; r < 4; r++)
      e[r] = mv[r] * o[0] + mv[4 + r] * o[1] + mv[8 + r] * o[2] + mv[12 + r] * o[3];
    for (int r = 0; r < 4; r++)
      c[r] = pr[r] * e[0] + pr[4 + r] * e[1] + pr[8 + r] * e[2] + pr[12 + r] * e[3];
    debugPrintf("[probe]   v%d object (%g %g %g) eye (%g %g %g) clip (%g %g %g w %g) -> ndc (%.2f %.2f %.2f)\n", k,
                (double)o[0], (double)o[1], (double)o[2], (double)e[0], (double)e[1], (double)e[2], (double)c[0],
                (double)c[1], (double)c[2], (double)c[3], c[3] ? (double)(c[0] / c[3]) : 0.0,
                c[3] ? (double)(c[1] / c[3]) : 0.0, c[3] ? (double)(c[2] / c[3]) : 0.0);
  }
}

static void tex_check_draw(void *ra);
static void tex_cap(uint32_t cap, int on);

typedef struct {
  uint32_t mode, type;
  int32_t first, count;
  const void *idx;
} DrawArgs;
static void do_elements(const void *a) {
  const DrawArgs *d = a;
  ((void (*)(uint32_t, int32_t, uint32_t, const void *))r_de)(d->mode, d->count, d->type, d->idx);
}
static void *r_da;
static void do_arrays(const void *a) {
  const DrawArgs *d = a;
  ((void (*)(uint32_t, int32_t, int32_t))r_da)(d->mode, d->first, d->count);
}

static void w_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void *idx) {
  tex_check_draw(__builtin_return_address(0));
  if (g_pal_on && idx && count > 0 && (type == 0x1403 || type == 0x1401)) {
    uint32_t lo = 0xffffffffu, hi = 0;
    for (int32_t i = 0; i < count; i++) {
      uint32_t v = type == 0x1403 ? ((const uint16_t *)idx)[i] : ((const uint8_t *)idx)[i];
      lo = v < lo ? v : lo, hi = v > hi ? v : hi;
    }
    DrawArgs d = {mode, type, 0, count, idx};
    if (skinned_draw(lo, hi, do_elements, &d)) {
      report_err("glDrawElements (skinned)", mode, count, type);
      return;
    }
  }
  probe_draw(count, type, idx);
  ((void (*)(uint32_t, int32_t, uint32_t, const void *))r_de)(mode, count, type, idx);
  if (first_time(5u << 24 ^ mode << 16 ^ type))
    debugPrintf("[gl3d] glDrawElements: mode 0x%x, index type 0x%x (count %d)\n", (unsigned)mode, (unsigned)type,
                (int)count);
  report_err("glDrawElements", mode, count, type);
}

static void w_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
  tex_check_draw(__builtin_return_address(0));
  if (g_pal_on && count > 0 && first >= 0) {
    DrawArgs d = {mode, 0, first, count, NULL};
    if (skinned_draw((uint32_t)first, (uint32_t)(first + count - 1), do_arrays, &d))
      return;
  }
  ((void (*)(uint32_t, int32_t, int32_t))r_da)(mode, first, count);
}

/* config [debug] gl_off: GL states never switched on, to find the one that
 * hides the 3D world (fog, scissor, cull, alpha, depth, lighting, blend). */
static int cap_blocked(uint32_t cap) {
  const char *o = g_a5_cfg.gl_off;
  struct { const char *n; uint32_t c; } k[] = {{"fog", 0xb60},  {"scissor", 0xc11}, {"cull", 0xb44},
                                                {"alpha", 0xbc0}, {"depth", 0xb71},   {"lighting", 0xb50},
                                                {"blend", 0xbe2}};
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (k[i].c == cap && strstr(o, k[i].n))
      return 1;
  return 0;
}

static void w_enable(uint32_t cap) {
  tex_cap(cap, 1);
  if (cap == GL_MATRIX_PALETTE_OES) {
    g_pal_on = 1;
    return;
  }
  if (cap_blocked(cap)) {
    if (first_time(8u << 24 ^ cap))
      debugPrintf("[gl3d] glEnable(0x%x) blocked by [debug] gl_off\n", (unsigned)cap);
    return;
  }
  ((void (*)(uint32_t))r_en)(cap);
  if (first_time(6u << 24 ^ cap))
    debugPrintf("[gl3d] glEnable(0x%x)\n", (unsigned)cap);
  report_err("glEnable", cap, 0, 0);
}

static void *r_dis, *r_ecs, *r_dcs;
static void w_disable(uint32_t cap) {
  tex_cap(cap, 0);
  if (cap == GL_MATRIX_PALETTE_OES) {
    g_pal_on = 0;
    return;
  }
  ((void (*)(uint32_t))r_dis)(cap);
}
static void w_enable_cs(uint32_t a) {
  if (a == GL_MATRIX_INDEX_ARRAY_OES || a == GL_WEIGHT_ARRAY_OES)
    return;
  if (a == GL_NORMAL_ARRAY_)
    g_normals_on = 1;
  ((void (*)(uint32_t))r_ecs)(a);
}
static void w_disable_cs(uint32_t a) {
  if (a == GL_MATRIX_INDEX_ARRAY_OES || a == GL_WEIGHT_ARRAY_OES)
    return;
  if (a == GL_NORMAL_ARRAY_)
    g_normals_on = 0;
  ((void (*)(uint32_t))r_dcs)(a);
}

static uint32_t g_mode = 0x1700;
static void *r_vpt, *r_sc, *r_fogf, *r_fogfv, *r_cc, *r_df, *r_clr;

static void w_viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
  ((void (*)(int32_t, int32_t, int32_t, int32_t))r_vpt)(x, y, w, h);
  if (first_time(9u << 24 ^ (uint32_t)(x * 7 + y * 131 + w * 3 + h * 17)))
    debugPrintf("[gl3d] glViewport(%d, %d, %d, %d)\n", (int)x, (int)y, (int)w, (int)h);
}
static void w_scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
  ((void (*)(int32_t, int32_t, int32_t, int32_t))r_sc)(x, y, w, h);
  if (first_time(10u << 24 ^ (uint32_t)(x * 7 + y * 131 + w * 3 + h * 17)))
    debugPrintf("[gl3d] glScissor(%d, %d, %d, %d)\n", (int)x, (int)y, (int)w, (int)h);
}
static void w_fogf(uint32_t pname, float v) {
  ((void (*)(uint32_t, float))r_fogf)(pname, v);
  static int told;
  if (told++ < 16)
    debugPrintf("[gl3d] glFogf(0x%x, %g)\n", (unsigned)pname, (double)v);
  report_err("glFogf", pname, (uint32_t)v, 0);
}
static void w_fogfv(uint32_t pname, const float *v) {
  ((void (*)(uint32_t, const float *))r_fogfv)(pname, v);
  static int told;
  if (told++ < 8 && v)
    debugPrintf("[gl3d] glFogfv(0x%x, %g %g %g %g)\n", (unsigned)pname, (double)v[0], (double)v[1], (double)v[2],
                (double)v[3]);
  report_err("glFogfv", pname, 0, 0);
}
static void w_load_matrix(const float *m) {
  if (g_pal_mode) {
    if (m)
      memcpy(g_pal[g_pal_cur], m, sizeof g_pal[0]);
    return;
  }
  ((void (*)(const float *))r_lm)(m);
  static int told;
  if (g_mode == 0x1701 && told++ < 4 && m)
    debugPrintf("[gl3d] projection: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n", (double)m[0],
                (double)m[1], (double)m[2], (double)m[3], (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                (double)m[8], (double)m[9], (double)m[10], (double)m[11], (double)m[12], (double)m[13], (double)m[14],
                (double)m[15]);
}
static void w_clear_color(float r, float g, float b, float a) {
  ((void (*)(float, float, float, float))r_cc)(r, g, b, a);
  if (first_time(11u << 24 ^ (uint32_t)(r * 255) << 16 ^ (uint32_t)(g * 255) << 8 ^ (uint32_t)(b * 255)))
    debugPrintf("[gl3d] glClearColor(%g, %g, %g, %g)\n", (double)r, (double)g, (double)b, (double)a);
}
static void w_depth_func(uint32_t f) {
  ((void (*)(uint32_t))r_df)(f);
  if (first_time(12u << 24 ^ f))
    debugPrintf("[gl3d] glDepthFunc(0x%x)\n", (unsigned)f);
}
static void w_clear(uint32_t mask) {
  ((void (*)(uint32_t))r_clr)(mask);
  if (first_time(13u << 24 ^ mask))
    debugPrintf("[gl3d] glClear(0x%x)\n", (unsigned)mask);
}

/* ------------------------------------------------------------ mipmaps
 * A texture whose minification filter wants mipmaps but that has no complete
 * chain of smaller levels is "incomplete" in GLES 1: it draws as the plain
 * vertex colour -- white -- with no GL error. The phones' drivers were laxer
 * about it; full-screen 2D backdrops (loading screens, options, co-driver
 * choice) came out white on the Switch (hardware 2026-11-19), while textures
 * the engine uploads with all levels were fine.
 *
 * So the filter the engine asks for is kept per texture, and GL is handed a
 * filter without mipmaps (LINEAR) for as long as the texture's level chain is
 * not complete -- the engine's own filter as soon as it is. config [debug]
 * gl_off "mipmap" still forces LINEAR everywhere. */
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_LINEAR_ 0x2601
#define TEX_MAX 8192
typedef struct {
  uint16_t w, h;     /* level 0 */
  uint32_t levels;   /* bit per uploaded level */
  int32_t minf;      /* the engine's filter; 0 = GL's default */
  int32_t applied;   /* what GL has; 0 = untouched */
} TexInfo;
static TexInfo g_tex[TEX_MAX];
static uint32_t g_bound[8], g_unit;
static void *r_tpi, *r_tpf, *r_bind, *r_atex, *r_deltex;

static int is_mip(int32_t f) { return f >= 0x2700 && f <= 0x2703; }
static int chain_complete(const TexInfo *t) {
  if (!(t->levels & 1))
    return 0;
  int n = 0, m = t->w > t->h ? t->w : t->h;
  while (m > 1)
    m >>= 1, n++;
  uint32_t need = n >= 31 ? 0xffffffffu : (2u << n) - 1;
  return (t->levels & need) == need;
}
static void real_min_filter(int32_t v) {
  if (!r_tpi)
    dcr_gl_lookup("glTexParameteri"); /* sets r_tpi to Mesa's */
  if (r_tpi)
    ((void (*)(uint32_t, uint32_t, int32_t))r_tpi)(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER, v);
}
/* Hands GL the filter the currently bound texture should have now. */
static void tex_apply(void) {
  uint32_t id = g_bound[g_unit & 7];
  if (!id || id >= TEX_MAX)
    return;
  TexInfo *t = &g_tex[id];
  int32_t want = t->minf ? t->minf : 0x2702; /* GL_NEAREST_MIPMAP_LINEAR */
  if (is_mip(want) && (!chain_complete(t) || strstr(g_a5_cfg.gl_off, "mipmap")))
    want = GL_LINEAR_;
  if (want != t->applied) {
    real_min_filter(want);
    t->applied = want;
    static int told;
    if (want == GL_LINEAR_ && is_mip(t->minf ? t->minf : 0x2702) && (t->levels & 1) && told++ < 12)
      debugPrintf("[gl] texture %u (%ux%u, levels 0x%x) has no full mipmap chain: drawn with GL_LINEAR\n",
                  (unsigned)id, (unsigned)t->w, (unsigned)t->h, (unsigned)t->levels);
  }
}
static void tex_uploaded(uint32_t target, int32_t level, int32_t w, int32_t h) {
  uint32_t id = g_bound[g_unit & 7];
  if (target != GL_TEXTURE_2D_ || !id || id >= TEX_MAX || level < 0 || level > 15)
    return;
  TexInfo *t = &g_tex[id];
  if (level == 0) {
    if (t->w != (uint16_t)w || t->h != (uint16_t)h)
      t->levels = 0;
    t->w = (uint16_t)w, t->h = (uint16_t)h;
  }
  t->levels |= 1u << level;
  tex_apply();
}
/* Bring-up (white loading / options / co-driver screens, hardware
 * 2026-11-19): a draw with texturing on whose bound texture never got any
 * pixels draws as plain white. Logged once per texture, with the engine
 * function that drew. */
static uint8_t g_tex_on[8];
static void tex_cap(uint32_t cap, int on) {
  if (cap == GL_TEXTURE_2D_)
    g_tex_on[g_unit & 7] = (uint8_t)on;
}
static void tex0_flush2d(void);
static void tex_check_draw(void *ra) {
  uint32_t u = g_unit & 7, id = g_bound[u];
  if (!g_tex_on[u])
    return;
  if (id < TEX_MAX && (g_tex[id].levels & 1))
    return;
  if (id == 0)
    tex0_flush2d();
  static uint32_t seen[64];
  static int n;
  for (int i = 0; i < n; i++)
    if (seen[i] == id)
      return;
  if (n >= 64)
    return;
  seen[n++] = id;
  char who[96];
  debugPrintf("[tex] draw with texture %u that has no pixels (unit %u) from %s\n", (unsigned)id, (unsigned)u,
              dcr_addr_name((uint32_t)(uintptr_t)ra, who, sizeof who));
}

/* Bring-up: which engine Texture a 2D draw (Lib3D::Flush2D) with GL texture 0
 * meant. Flush2D takes the Texture from Lib3D+4844 or, failing that, from the
 * TexturesLibrary by the key at Lib3D+4816 (index << 8 | palette) and binds
 * Texture+0x44[palette] -- 0 when that array is NULL. Logged once per key,
 * with the Texture's first fields. */
/* ------------------------------------------------------------ menu backdrops
 * The menus' backdrop -- loading screens, options, the co-driver gallery --
 * is texture 60 of the general library, and it drew as the library's empty
 * placeholder: never loaded (w 0, h 0, no GL name; hardware 2026-11-19).
 * A texture the engine draws but never loaded is loaded here once, after
 * the frame, with the engine's own TexturesLibrary::LoadTexture, after
 * marking it needed (SetTexFlags 1) as the sprite refresh would. Its entry
 * is logged before, the result after. */
static struct {
  uint32_t id, lib;
  int tries;
} g_want[16];
static int g_nwant;
static void tex0_want(uint32_t id, uint32_t lib) {
  for (int i = 0; i < g_nwant; i++)
    if (g_want[i].id == id && g_want[i].lib == lib)
      return;
  if (g_nwant < 16)
    g_want[g_nwant++] = (typeof(g_want[0])){id, lib, 0};
}
void a5_gl_frame_end(void) {
  static uintptr_t a_texlib, a_idx, a_flags, a_load;
  static int looked;
  if (!g_nwant)
    return;
  if (!looked++) {
    a_texlib = so_try_find_addr_rx(&g_a5_mod, "g_pTexLib");
    a_idx = so_try_find_addr_rx(&g_a5_mod, "_ZN15TexturesLibrary9GetTexIdxEii");
    a_flags = so_try_find_addr_rx(&g_a5_mod, "_ZN15TexturesLibrary11SetTexFlagsEthi");
    a_load = so_try_find_addr_rx(&g_a5_mod, "_ZN15TexturesLibrary11LoadTextureEiib");
  }
  uint8_t *texlib = a_texlib ? *(uint8_t **)a_texlib : NULL;
  if (!texlib || !a_idx || !a_flags || !a_load)
    return;
  for (int i = 0; i < g_nwant; i++) {
    if (g_want[i].tries >= 2)
      continue;
    g_want[i].tries++;
    uint32_t id = g_want[i].id, lib = g_want[i].lib;
    int idx = ((int (*)(void *, int, int))a_idx)(texlib, (int)id, (int)lib);
    if (idx < 0) {
      debugPrintf("[tex0] texture %u (lib %u): not in the library\n", (unsigned)id, (unsigned)lib);
      continue;
    }
    uint8_t *list = (*(uint8_t ***)(texlib + 8))[lib];
    uint8_t *e = *(uint8_t **)(list + 4) + idx * 20;
    debugPrintf("[tex0] texture %u (lib %u) = entry %d: Texture %p, id %u, flags 0x%02x, dirty %u -- loading it\n",
                (unsigned)id, (unsigned)lib, idx, *(void **)e, (unsigned)*(uint16_t *)(e + 12), (unsigned)e[14],
                (unsigned)e[16]);
    /* LoadTexture loads an entry only when (flags & 3) == 1; texture 60 came
     * with 0xf3 -- bit 1 set as well, so it stayed the placeholder. Try it
     * with bit 1 cleared. */
    e[14] = (uint8_t)((e[14] & ~3u) | 1u);
    int r = ((int (*)(void *, int, int, int))a_load)(texlib, idx, (int)lib, 0);
    uint32_t *t = *(uint32_t **)e;
    debugPrintf("[tex0] texture %u: LoadTexture -> %d; flags now 0x%02x, Texture %p w %u h %u fmt 0x%x ids %p\n",
                (unsigned)id, r, (unsigned)e[14], (void *)t, t ? t[1] : 0, t ? t[2] : 0, t ? t[3] : 0,
                t ? (void *)(uintptr_t)t[17] : NULL);
  }
}

static void tex0_flush2d(void) {
  static uintptr_t a_lib3d, a_texlib, a_get;
  static int looked;
  if (!looked++) {
    a_lib3d = so_try_find_addr_rx(&g_a5_mod, "g_pLib3D");
    a_texlib = so_try_find_addr_rx(&g_a5_mod, "g_pTexLib");
    a_get = so_try_find_addr_rx(&g_a5_mod, "_ZN15TexturesLibrary10GetTextureEti");
  }
  if (!a_lib3d || !a_texlib || !a_get)
    return;
  uint8_t *lib3d = *(uint8_t **)a_lib3d;
  void *texlib = *(void **)a_texlib;
  if (!lib3d || !texlib)
    return;
  uint32_t direct = *(uint32_t *)(lib3d + 4844), key = *(uint32_t *)(lib3d + 4816);
  uint32_t lib = *(uint32_t *)(lib3d + 4860);
  uint32_t mark = direct ? direct : key;
  /* once per key every 5 s, so a screen shown later with the same key shows
   * up again */
  static uint32_t seen[48];
  static uint64_t when[48];
  static int n, total;
  uint64_t now = armGetSystemTick();
  int slot = -1;
  for (int i = 0; i < n; i++)
    if (seen[i] == mark)
      slot = i;
  if (slot >= 0 && now - when[slot] < armNsToTicks(5000000000ull))
    return;
  if (slot < 0) {
    if (n >= 48)
      return;
    slot = n++;
    seen[slot] = mark;
  }
  when[slot] = now;
  if (total++ >= 200)
    return;
  uint32_t *tex = NULL;
  if (direct) {
    tex = *(uint32_t **)(uintptr_t)direct;
  } else if (key != 0xffffffffu) {
    void **slot = ((void **(*)(void *, uint32_t, int))a_get)(texlib, (key >> 8) & 0xffff, (int)lib);
    tex = slot ? (uint32_t *)*slot : NULL;
  }
  if (!tex) {
    debugPrintf("[tex0] Flush2D key 0x%x lib %u direct 0x%x: no Texture\n", (unsigned)key, (unsigned)lib,
                (unsigned)direct);
    return;
  }
  if (!direct && !tex[1] && !tex[2])
    tex0_want((key >> 8) & 0xffff, lib);
  debugPrintf("[tex0] Flush2D key 0x%x (index %u pal %u) lib %u direct 0x%x: Texture %p w %u h %u flags 0x%x "
              "+10 0x%x +14 0x%x +18 0x%x +1c 0x%x npal %u +24 0x%x +28 0x%x +3c 0x%x +40 0x%x ids %p +48 0x%x\n",
              (unsigned)key, (unsigned)((key >> 8) & 0xffff), (unsigned)(key & 0xff), (unsigned)lib,
              (unsigned)direct, (void *)tex, tex[1], tex[2], tex[3], tex[4], tex[5], tex[6], tex[7], tex[8], tex[9],
              tex[10], tex[15], tex[16], (void *)(uintptr_t)tex[17], tex[18]);
}

/* Texture environment (how a texture combines with the vertex colour): the
 * values the engine sets, once each, and any GL error -- a combiner setting
 * Mesa refuses leaves the old one, which can wash a sprite out to white. */
static void *r_tenvi, *r_tenvfv;
static void w_tex_env_i(uint32_t target, uint32_t pname, int32_t v) {
  ((void (*)(uint32_t, uint32_t, int32_t))r_tenvi)(target, pname, v);
  if (first_time(16u << 24 ^ pname << 12 ^ (uint32_t)v))
    debugPrintf("[gl3d] glTexEnvi(0x%x, 0x%x, 0x%x)\n", (unsigned)target, (unsigned)pname, (unsigned)v);
  report_err("glTexEnvi", target, pname, (uint32_t)v);
}
static void w_tex_env_fv(uint32_t target, uint32_t pname, const float *v) {
  ((void (*)(uint32_t, uint32_t, const float *))r_tenvfv)(target, pname, v);
  if (v && first_time(17u << 24 ^ pname << 12 ^ (uint32_t)(v[0] * 255)))
    debugPrintf("[gl3d] glTexEnvfv(0x%x, 0x%x, %g %g %g %g)\n", (unsigned)target, (unsigned)pname, (double)v[0],
                (double)v[1], (double)v[2], (double)v[3]);
  report_err("glTexEnvfv", target, pname, 0);
}

static void w_bind_texture(uint32_t target, uint32_t id) {
  ((void (*)(uint32_t, uint32_t))r_bind)(target, id);
  if (target == GL_TEXTURE_2D_)
    g_bound[g_unit & 7] = id;
}
static void w_active_texture(uint32_t unit) {
  ((void (*)(uint32_t))r_atex)(unit);
  g_unit = unit - 0x84C0; /* GL_TEXTURE0 */
}
static void w_delete_textures(int32_t n, const uint32_t *ids) {
  for (int32_t i = 0; ids && i < n; i++)
    if (ids[i] && ids[i] < TEX_MAX)
      memset(&g_tex[ids[i]], 0, sizeof g_tex[0]);
  ((void (*)(int32_t, const uint32_t *))r_deltex)(n, ids);
}

static void w_tex_param_i(uint32_t target, uint32_t pname, int32_t v) {
  if (target == GL_TEXTURE_2D_ && pname == GL_TEXTURE_MIN_FILTER) {
    uint32_t id = g_bound[g_unit & 7];
    if (id && id < TEX_MAX) {
      g_tex[id].minf = v;
      tex_apply();
      return;
    }
  }
  ((void (*)(uint32_t, uint32_t, int32_t))r_tpi)(target, pname, v);
  if (first_time(14u << 24 ^ pname << 12 ^ (uint32_t)v))
    debugPrintf("[gl3d] glTexParameteri(0x%x, 0x%x)\n", (unsigned)pname, (unsigned)v);
}
static void w_tex_param_f(uint32_t target, uint32_t pname, float v) {
  if (target == GL_TEXTURE_2D_ && pname == GL_TEXTURE_MIN_FILTER) {
    uint32_t id = g_bound[g_unit & 7];
    if (id && id < TEX_MAX) {
      g_tex[id].minf = (int32_t)v;
      tex_apply();
      return;
    }
  }
  ((void (*)(uint32_t, uint32_t, float))r_tpf)(target, pname, v);
  if (first_time(15u << 24 ^ pname << 12 ^ (uint32_t)v))
    debugPrintf("[gl3d] glTexParameterf(0x%x, %g)\n", (unsigned)pname, (double)v);
}

static void w_load_identity(void) {
  if (g_pal_mode) {
    memcpy(g_pal[g_pal_cur], k_ident, sizeof k_ident);
    return;
  }
  ((void (*)(void))r_li)();
}

static void w_matrix_mode(uint32_t mode) {
  g_mode = mode;
  g_pal_mode = mode == GL_MATRIX_PALETTE_OES;
  if (g_pal_mode)
    return; /* Mesa keeps its mode; the palette lives here */
  g_real_mode = mode;
  ((void (*)(uint32_t))r_mm)(mode);
  if (first_time(7u << 24 ^ mode))
    debugPrintf("[gl3d] glMatrixMode(0x%x)\n", (unsigned)mode);
  report_err("glMatrixMode", mode, 0, 0);
}

static uintptr_t diag_wrap(const char *name, uintptr_t real) {
  static const struct { const char *n; void **r; void *w; } k[] = {
      {"glVertexPointer", &r_vp, (void *)w_vertex_ptr},     {"glTexCoordPointer", &r_tp, (void *)w_texcoord_ptr},
      {"glColorPointer", &r_cp, (void *)w_color_ptr},       {"glNormalPointer", &r_np, (void *)w_normal_ptr},
      {"glDrawElements", &r_de, (void *)w_draw_elements},   {"glEnable", &r_en, (void *)w_enable},
      {"glMatrixMode", &r_mm, (void *)w_matrix_mode},       {"glViewport", &r_vpt, (void *)w_viewport},
      {"glScissor", &r_sc, (void *)w_scissor},              {"glFogf", &r_fogf, (void *)w_fogf},
      {"glFogfv", &r_fogfv, (void *)w_fogfv},               {"glLoadMatrixf", &r_lm, (void *)w_load_matrix},
      {"glClearColor", &r_cc, (void *)w_clear_color},       {"glDepthFunc", &r_df, (void *)w_depth_func},
      {"glClear", &r_clr, (void *)w_clear},                 {"glDrawArrays", &r_da, (void *)w_draw_arrays},
      {"glDisable", &r_dis, (void *)w_disable},             {"glLoadIdentity", &r_li, (void *)w_load_identity},
      {"glEnableClientState", &r_ecs, (void *)w_enable_cs}, {"glDisableClientState", &r_dcs, (void *)w_disable_cs},
      {"glTexParameteri", &r_tpi, (void *)w_tex_param_i},   {"glTexParameterf", &r_tpf, (void *)w_tex_param_f},
      {"glBindTexture", &r_bind, (void *)w_bind_texture},   {"glActiveTexture", &r_atex, (void *)w_active_texture},
      {"glDeleteTextures", &r_deltex, (void *)w_delete_textures},
      {"glTexEnvi", &r_tenvi, (void *)w_tex_env_i},         {"glTexEnvfv", &r_tenvfv, (void *)w_tex_env_fv},
  };
  if (!real)
    return 0;
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(name, k[i].n)) {
      *k[i].r = (void *)real;
      return (uintptr_t)k[i].w;
    }
  return 0;
}

/* Bring-up: the first texture uploads, with the GL error each left (the
 * menus' backgrounds came out white, hardware 2026-11-19). */
static void w_teximage(uint32_t target, int32_t level, int32_t ifmt, int32_t w, int32_t h, int32_t border,
                       uint32_t fmt, uint32_t type, const void *data) {
  static int told;
  static uint32_t (*get_error)(void);
  if (!get_error)
    get_error = (uint32_t(*)(void))dcr_gl_lookup("glGetError");
  /* an error an earlier call left (the engine's glTexParameterf of
   * GL_GENERATE_MIPMAP leaves 0x500) is not this upload's */
  for (int i = 0; get_error && i < 8 && get_error(); i++)
    ;
  ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))r_tex_ptr)(
      target, level, ifmt, w, h, border, fmt, type, data);
  uint32_t err = get_error ? get_error() : 0;
  if (!err)
    tex_uploaded(target, level, w, h);
  if (told < 40 || (err && told < 80)) {
    told++;
    debugPrintf("[gl] glTexImage2D(level %d, internal 0x%x, %dx%d, format 0x%x, type 0x%x, data %s) -> error 0x%x\n",
                (int)level, (unsigned)ifmt, (int)w, (int)h, (unsigned)fmt, (unsigned)type, data ? "yes" : "NULL",
                (unsigned)err);
  }
  /* Bring-up (white loading / options / co-driver backdrops, hardware
   * 2026-11-19): every big upload, who made it, and what its pixels are --
   * white in the data means the decoding went wrong, not GL. */
  static int big;
  if (level == 0 && (int64_t)w * h >= 128 * 128 && big < 80) {
    big++;
    int n = 0, white = 0, clear = 0, black = 0;
    if (data && type == 0x1401 && (fmt == 0x1908 || fmt == 0x1907)) {
      int bpp = fmt == 0x1908 ? 4 : 3;
      const uint8_t *px = data;
      for (int64_t i = 0; i < (int64_t)w * h; i += 97, n++) {
        const uint8_t *q = px + i * bpp;
        white += q[0] > 245 && q[1] > 245 && q[2] > 245;
        black += q[0] < 10 && q[1] < 10 && q[2] < 10;
        clear += bpp == 4 && q[3] == 0;
      }
    } else if (data && (type == 0x8033 || type == 0x8363 || type == 0x8034)) {
      const uint16_t *px = data;
      for (int64_t i = 0; i < (int64_t)w * h; i += 97, n++) {
        uint16_t v = px[i];
        if (type == 0x8033) {
          white += (v & 0xfff0) == 0xfff0, black += (v & 0xfff0) == 0, clear += (v & 0xf) == 0;
        } else if (type == 0x8363) {
          white += v == 0xffff, black += v == 0;
        } else {
          white += (v & 0xfffe) == 0xfffe, black += (v & 0xfffe) == 0, clear += !(v & 1);
        }
      }
    }
    char who[96];
    debugPrintf("[tex] %dx%d fmt 0x%x type 0x%x err 0x%x from %s: %d samples, white %d%%, black %d%%, alpha 0 %d%%\n",
                (int)w, (int)h, (unsigned)fmt, (unsigned)type, (unsigned)err,
                dcr_addr_name((uint32_t)(uintptr_t)__builtin_return_address(0), who, sizeof who), n,
                n ? white * 100 / n : 0, n ? black * 100 / n : 0, n ? clear * 100 / n : 0);
  }
}

uintptr_t port_gl_wrap(const char *name, uintptr_t real) {
  if (!strcmp(name, "glTexImage2D")) {
    r_tex_ptr = (void *)real;
    return (uintptr_t)w_teximage;
  }
  if (!strcmp(name, "glCompressedTexImage2D")) {
    r_ctex_ptr = (void *)real;
    return (uintptr_t)w_compressed;
  }
  if (real)
    return diag_wrap(name, real); /* Mesa has it: a bring-up logger or nothing */
  if (!strcmp(name, "glCurrentPaletteMatrixOES"))
    return (uintptr_t)st_palette;
  if (!strcmp(name, "glMatrixIndexPointerOES"))
    return (uintptr_t)st_index_ptr;
  if (!strcmp(name, "glWeightPointerOES"))
    return (uintptr_t)st_weight_ptr;
  return 0;
}

/* ------------------------------------------------------------ ATC textures
 * Gameloft shipped Asphalt 5's data once per GPU family; the "adreno" data
 * has its textures in AMD/Qualcomm ATC (GL_AMD_compressed_ATC_texture), which
 * the Switch's GPU does not have: every glCompressedTexImage2D of them failed
 * and the screen stayed white (hardware 2026-11-19). They are decoded here to
 * RGBA8888 and uploaded with glTexImage2D instead.
 *
 * ATC blocks are 4x4 texels, like DXT:
 *   colour (8 bytes): c0 (RGB555, bit 15 = mode), c1 (RGB565), 2-bit indices
 *     mode 0: c0, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1, c1
 *     mode 1: black, c0 - c1/4, c0, c1
 *   GL_ATC_RGBA_EXPLICIT_ALPHA_AMD:      8 bytes of 4-bit alpha first
 *   GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD:  a DXT5 alpha block first
 * (If colours come out slightly off, the interpolation weights are the place
 * to look.) */
#include <stdlib.h>

#define GL_RGBA                               0x1908
#define GL_UNSIGNED_BYTE                      0x1401
#define GL_ATC_RGB_AMD                        0x8C92
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD        0x8C93
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD    0x87EE

typedef void (*fn_tex)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
typedef void (*fn_ctex)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, const void *);
#define r_ctex ((fn_ctex)r_ctex_ptr)
#define r_tex ((fn_tex)r_tex_ptr)

static void atc_colours(const uint8_t *b, uint8_t pal[4][3]) {
  unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  int r0 = (c0 >> 10) & 31, g0 = (c0 >> 5) & 31, b0 = c0 & 31;
  int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
  int C0[3] = {r0 << 3 | r0 >> 2, g0 << 3 | g0 >> 2, b0 << 3 | b0 >> 2};
  int C1[3] = {r1 << 3 | r1 >> 2, g1 << 2 | g1 >> 4, b1 << 3 | b1 >> 2};
  for (int k = 0; k < 3; k++) {
    if (!(c0 & 0x8000)) {
      pal[0][k] = (uint8_t)C0[k];
      pal[1][k] = (uint8_t)((2 * C0[k] + C1[k]) / 3);
      pal[2][k] = (uint8_t)((C0[k] + 2 * C1[k]) / 3);
      pal[3][k] = (uint8_t)C1[k];
    } else {
      int v = C0[k] - C1[k] / 4;
      pal[0][k] = 0;
      pal[1][k] = (uint8_t)(v < 0 ? 0 : v);
      pal[2][k] = (uint8_t)C0[k];
      pal[3][k] = (uint8_t)C1[k];
    }
  }
}

static void dxt5_alpha(const uint8_t *b, uint8_t out[16]) {
  int a0 = b[0], a1 = b[1], pal[8];
  pal[0] = a0, pal[1] = a1;
  if (a0 > a1)
    for (int i = 1; i < 7; i++)
      pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
  else {
    for (int i = 1; i < 5; i++)
      pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    pal[6] = 0, pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++)
    bits |= (uint64_t)b[2 + i] << (8 * i);
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}

static uint8_t *atc_decode(uint32_t fmt, int w, int h, const uint8_t *src, int size) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == GL_ATC_RGB_AMD ? 8 : 16;
  if (size < bw * bh * bs)
    return NULL;
  uint8_t *out = malloc((size_t)w * h * 4);
  if (!out)
    return NULL;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      const uint8_t *b = src + (by * bw + bx) * bs;
      uint8_t alpha[16];
      if (fmt == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD)
        for (int i = 0; i < 16; i++) {
          int n = (b[i / 2] >> (4 * (i & 1))) & 15;
          alpha[i] = (uint8_t)(n << 4 | n);
        }
      else if (fmt == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD)
        dxt5_alpha(b, alpha);
      else
        for (int i = 0; i < 16; i++)
          alpha[i] = 255;
      const uint8_t *c = b + bs - 8;
      uint8_t pal[4][3];
      atc_colours(c, pal);
      uint32_t idx = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
      for (int i = 0; i < 16; i++) {
        int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= w || y >= h)
          continue;
        uint8_t *p = out + ((size_t)y * w + x) * 4;
        const uint8_t *col = pal[(idx >> (2 * i)) & 3];
        p[0] = col[0], p[1] = col[1], p[2] = col[2], p[3] = alpha[i];
      }
    }
  return out;
}

static void w_compressed(uint32_t target, int32_t level, uint32_t fmt, int32_t w, int32_t h, int32_t border,
                         int32_t size, const void *data) {
  static int told;
  int atc = fmt == GL_ATC_RGB_AMD || fmt == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD || fmt == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD;
  if (told < 12) {
    told++;
    debugPrintf("[gl] glCompressedTexImage2D(level %d, format 0x%x, %dx%d, %d bytes)%s\n", (int)level, (unsigned)fmt,
                (int)w, (int)h, (int)size, atc ? ": ATC, decoded in software" : "");
  }
  if (atc && !r_tex_ptr)
    r_tex_ptr = (void *)dcr_gl_lookup("glTexImage2D");
  if (!atc || !r_tex) {
    if (r_ctex) {
      for (int i = 0; i < 8 && gl_err(); i++)
        ;
      r_ctex(target, level, fmt, w, h, border, size, data);
      if (!gl_err())
        tex_uploaded(target, level, w, h);
    }
    return;
  }
  uint8_t *rgba = data ? atc_decode(fmt, w, h, data, size) : NULL;
  if (!rgba) {
    static int bad;
    if (bad++ < 4)
      debugPrintf("[gl] ATC texture %dx%d (%d bytes) could not be decoded\n", (int)w, (int)h, (int)size);
    return;
  }
  r_tex(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  tex_uploaded(target, level, w, h);
  free(rgba);
}
