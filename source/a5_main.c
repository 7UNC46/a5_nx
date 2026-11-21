/* a5_main.c -- Asphalt 5 HD on android32: from the player's APK to the game's
 * frame loop.
 *
 * port_load: the runtime has found the APK (by its package); the setup
 *   unpacks PORT_ABI_DIR/libasphalt5.so next to it (once). Then the library
 *   is mapped, relocated, bound to the runtime's bionic shims and sealed.
 *
 * port_run: what the Java side does on a phone (classes.dex of the APK):
 *     System.loadLibrary("asphalt5")    (no JNI_OnLoad in this build)
 *     GLResLoader.init                  -> GLResLoader.nativeInit(I)
 *     GLMediaPlayer.init                -> GLMediaPlayer.nativeInit(I)
 *     Asphalt5Renderer (GL thread):
 *       onSurfaceCreated -> nativeGetJNIEnv(), Asphalt5.nativeInit(II),
 *                           Asphalt5Renderer.nativeInit(kb, lost, w, h, lang)
 *       onSurfaceChanged -> nativeGetJNIEnv(), nativeResize(w, h)
 *       onDrawFrame      -> nativeRender()
 *     Asphalt5GLSurfaceView.onPause / onResume -> nativePause / nativeResume
 *   Here everything runs on the main thread, which is the GL thread too, as
 *   in a6_nx.
 *
 * Asphalt5Renderer.nativeInit(a, b, c, d, e) (libasphalt5.so): a > 1 sets
 * mHasKeyboard (keys / a gamepad there), b is "the GL context was lost" on a
 * second call, and c, d, e go to GameInit(width, height, language).
 * Asphalt5.nativeInit(II) and the two loaders' (I): the arguments are noted
 * in debug.log; see A5_* below. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_boost.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "jni.h"
#include "port_config.h"
#include "rt_applet.h"
#include "rt_boot.h"
#include "rt_window.h"
#include "so_util.h"
#include "util.h"

#include "a5.h"

so_module g_a5_mod;

typedef void (*fn_v)(void *env, void *obj);
typedef void (*fn_i)(void *env, void *obj, int a);
typedef void (*fn_ii)(void *env, void *obj, int a, int b);
typedef void (*fn_iiiii)(void *env, void *obj, int a, int b, int c, int d, int e);

#define J "Java_com_gameloft_android_GAND_GloftA5HD_"

/* ---------------------------------------------------------------- setup */
static const char *const k_libs[] = {A5_LIB};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 1,
    .apk_requirement = "Asphalt 5 HD (" PORT_PACKAGE ") with " PORT_ABI_DIR A5_LIB,
    .libs_p0 = 0,
    .libs_p1 = 100,
    .steps = NULL,
    .nsteps = 0,
};

const char *port_apk_help(void) {
  return "Copy the APK of your own Asphalt 5 HD to /switch/" PORT_NAME "/\n"
         "(any name). It must contain " PORT_ABI_DIR A5_LIB ".\n"
         "Put the game data folder next to it: /switch/" PORT_NAME "/" A5_DATA_DIR "/\n"
         "(the \"asphalt5\" folder of the data download, with package_general.bar_001.cnk...).";
}

void *a5_native(const char *sym) { return (void *)so_try_find_addr_rx(&g_a5_mod, sym); }

static void *need(const char *sym) {
  void *p = a5_native(sym);
  if (!p)
    fatal_error("%s has no %s: not the expected build (3.4.1)?", A5_LIB, sym);
  return p;
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* dcr_path.c's last word on every path: the data folder the game asks for
 * (/sdcard/gameloft/games/asphalt5/..., which the rules made
 * <root>/gameloft/games/asphalt5/...) is <root>/asphalt5/..., where the
 * player copies it. "//" collapsed on the way. */
const char *port_path_fixup(const char *real, char *out, size_t cap) {
  const char *t = strstr(real, "/gameloft/games/asphalt5");
  if (!t && !strstr(real, "//"))
    return real;
  if (t)
    snprintf(out, cap, "%s/" A5_DATA_DIR "%s", dcr_game_root(), t + strlen("/gameloft/games/asphalt5"));
  else
    snprintf(out, cap, "%s", real);
  size_t n = 0;
  for (size_t i = 0; out[i]; i++)
    if (!(out[i] == '/' && n > 0 && out[n - 1] == '/'))
      out[n++] = out[i];
  out[n] = 0;
  return out;
}

static void check_data(void) {
  char p[DCR_PATH_MAX];
  snprintf(p, sizeof p, "%s/" A5_DATA_DIR "/package_general.bar_001.cnk", dcr_game_root());
  if (!file_exists(p))
    fatal_error("The game data is missing (%s).\n\n%s", p, port_apk_help());
  debugPrintf("[a5] game data: %s/" A5_DATA_DIR "/\n", dcr_game_root());
}

/* ---------------------------------------------------------------- VFP vectors
 * The engine's matrix products (CMatrix::Mult, ::PreMult, ::SetMult) are
 * hand-written ARM11 VFP code that runs in VFP *short-vector mode*: they set
 * FPSCR.LEN = 4 so that one vmul/vmla works on four floats at once. ARMv8's
 * AArch32 (the Switch's Cortex-A57) has no short vectors -- LEN is ignored and
 * each instruction does one float -- so three quarters of every product stayed
 * unwritten: the camera matrix had no y column and an eye-space depth of about
 * -0.02, and the whole 3D world fell in front of the near plane (draw probe,
 * hardware 2026-11-19). The 2D HUD, built without CMatrix products, was fine.
 *
 * Replaced with the same products in C. CMatrix is 16 floats, row-major as the
 * engine keeps it (it transposes before glLoadMatrixf). From the vector code:
 *   Mult(this, m):       this = this * m
 *   PreMult(this, m):    this = m * this
 *   SetMult(d, a, b):    d = a * b
 * where row i of (a * b) = sum_k a[i][k] * row k of b. */
static void mat_mul(float *d, const float *a, const float *b) {
  float t[16];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
  memcpy(d, t, sizeof t);
}
static void cm_mult(float *self, const float *m) { mat_mul(self, self, m); }
static void cm_premult(float *self, const float *m) { mat_mul(self, m, self); }
static void cm_setmult(float *d, const float *a, const float *b) { mat_mul(d, a, b); }

static void fix_vfp_vectors(void) {
  static const struct { const char *sym; void *fn; } k[] = {
      {"_ZN7CMatrix4MultEPS_", (void *)cm_mult},
      {"_ZN7CMatrix7PreMultEPS_", (void *)cm_premult},
      {"_ZN7CMatrix7SetMultEPS_S0_", (void *)cm_setmult},
  };
  int n = 0;
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    uintptr_t a = so_try_find_addr_rx(&g_a5_mod, k[i].sym);
    if (a) {
      hook_arm(a, (uintptr_t)k[i].fn);
      n++;
    } else {
      debugPrintf("[a5] %s not found: its VFP vector code stays\n", k[i].sym);
    }
  }
  debugPrintf("[a5] VFP short-vector matrix code: %d of 3 functions replaced\n", n);
}


int a5_music_finished(void); /* a5_jni.c */
void a5_gl_frame_end(void);  /* a5_gl.c */

int port_load(const char *apk) {
  dcr_setup_from_apk(apk);
  char path[DCR_PATH_MAX];
  rt_root_path(path, sizeof path, A5_LIB);
  if (so_load(&g_a5_mod, path, NULL, PORT_SO_REGION_BYTES) != 0)
    fatal_error("Could not load %s.", path);
  so_relocate(&g_a5_mod);
  so_resolve(&g_a5_mod, NULL, 0, 1);
  so_fix_kuser_helpers(&g_a5_mod);
  so_finalize(&g_a5_mod);
  debugPrintf("[a5] %s mapped at %p\n", A5_LIB, g_a5_mod.load_virtbase);
  fix_vfp_vectors();
  check_data();
  return 0;
}

/* ---------------------------------------------------------------- focus */
static fn_v f_pause, f_resume;
static fn_iiiii f_rend_init;
static fn_ii f_resize;
static void *g_view_obj, *g_rend_obj;

void port_focus_lost(void) {
  if (f_pause && g_view_obj)
    f_pause(g_jni_env, g_view_obj);
  a5_sound_pause(1);
}

void port_focus_gained(void) {
  a5_sound_pause(0);
  if (f_resume && g_view_obj)
    f_resume(g_jni_env, g_view_obj);
}

/* ---------------------------------------------------------------- run */
static void *cls(const char *name) { return jni_class(name)->obj; }

/* The arguments the Java side passes (to confirm on hardware):
 * GLResLoader / GLMediaPlayer.nativeInit(I) and Asphalt5.nativeInit(II). */
#define A5_RES_INIT_ARG   0
#define A5_MEDIA_INIT_ARG 0
#define A5_KEYBOARD       2 /* > 1: mHasKeyboard */

void port_run(void) {
  jni_init();
  so_execute_init_array(&g_a5_mod);
  if (a5_native("JNI_OnLoad"))
    debugPrintf("[a5] JNI_OnLoad -> 0x%x\n", ((int (*)(void *, void *))a5_native("JNI_OnLoad"))(g_jni_vm, NULL));

  g_view_obj = jni_singleton(A5_CLS_VIEW);
  void *rend_obj = g_rend_obj = jni_singleton(A5_CLS_REND);
  void *game_obj = jni_singleton(A5_CLS_GAME);

  if (a5_sound_init() != 0)
    debugPrintf("[a5] no sound (audout)\n");

  /* The engine keeps ONE JNIEnv in a global, set only by
   * Asphalt5Renderer.nativeGetJNIEnv, and every other native reads it from
   * there (hardware 2026-11-19: GLResLoader.nativeInit+0x20, a NULL env, as
   * in a6_nx). On a phone the loaders' inits run later, from the GL thread's
   * onSurfaceCreated, when it is set: so it is set first here. */
  ((fn_v)need(J "Asphalt5Renderer_nativeGetJNIEnv"))(g_jni_env, rend_obj);
  debugPrintf("[a5] Asphalt5Renderer.nativeGetJNIEnv done (the engine's env global)\n");

  ((fn_i)need(J "GLResLoader_nativeInit"))(g_jni_env, cls(A5_CLS_RES), A5_RES_INIT_ARG);
  debugPrintf("[a5] GLResLoader.nativeInit done\n");
  ((fn_i)need(J "GLMediaPlayer_nativeInit"))(g_jni_env, cls(A5_CLS_MEDIA), A5_MEDIA_INIT_ARG);
  debugPrintf("[a5] GLMediaPlayer.nativeInit done\n");

  /* The engine lays its menus out for phone screens (at 1280x720 they sat
   * 480 pixels tall in the bottom of the screen, hardware 2026-11-19): it
   * draws at a phone's size and the vi layer scales that to the screen. */
  dcr_window_set_size(g_a5_cfg.game_w, g_a5_cfg.game_h);
  debugPrintf("[a5] the game draws at %dx%d, scaled to the screen\n", g_a5_cfg.game_w, g_a5_cfg.game_h);
  if (a5_gl_init() != 0)
    fatal_error("The graphics could not start (EGL). See debug.log.");
  int w = 1280, h = 720;
  dcr_window_size(&w, &h);
  int lang = a5_language();

  /* onSurfaceCreated */
  ((fn_v)need(J "Asphalt5Renderer_nativeGetJNIEnv"))(g_jni_env, rend_obj);
  ((fn_ii)need(J "Asphalt5_nativeInit"))(g_jni_env, cls(A5_CLS_GAME), w, h);
  debugPrintf("[a5] Asphalt5.nativeInit(%d, %d) done\n", w, h);
  f_rend_init = (fn_iiiii)need(J "Asphalt5Renderer_nativeInit");
  f_rend_init(g_jni_env, rend_obj, A5_KEYBOARD, 0, w, h, lang);
  debugPrintf("[a5] Asphalt5Renderer.nativeInit(keyboard %d, %dx%d, language %d) done\n", A5_KEYBOARD, w, h,
              lang);
  /* onSurfaceChanged */
  ((fn_v)need(J "Asphalt5Renderer_nativeGetJNIEnv"))(g_jni_env, rend_obj);
  f_resize = (fn_ii)need(J "Asphalt5Renderer_nativeResize");
  f_resize(g_jni_env, cls(A5_CLS_REND), w, h);
  debugPrintf("[a5] nativeResize %dx%d\n", w, h);

  fn_v render = (fn_v)need(J "Asphalt5Renderer_nativeRender");
  f_pause = (fn_v)a5_native(J "Asphalt5GLSurfaceView_nativePause");
  f_resume = (fn_v)a5_native(J "Asphalt5GLSurfaceView_nativeResume");
  a5_input_init();
  (void)game_obj;

  debugPrintf("[a5] frame loop\n");
  log_flush_ring();
  uint64_t frame = 0;
  while (!rt_exit_requested() && appletMainLoop()) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000LL);
      continue;
    }
    dcr_boost_frame_begin();
    a5_input_poll();
    if (g_a5_dump_request) {
      debugPrintf("[a5] hang dump requested (L+R+Minus): pausing 12 s for the watchdog\n");
      log_flush_ring();
      svcSleepThread(12000000000ll);
      g_a5_dump_request = 0;
      log_flush_ring();
    }
    render(g_jni_env, cls(A5_CLS_REND));
    a5_gl_frame_end(); /* textures the engine drew but never loaded */
    a5_gl_swap(); /* GLSurfaceView swaps after every onDrawFrame */
    if (g_a5_movie_done) {
      /* The video activity has "ended". GS_TrailerMovie waits for the Game's
       * "movie playing" byte (+7538, set in GS_TrailerMovie::Create) to go
       * back to 0; on a phone Game::Resume clears it when the activity comes
       * back with its GL context lost.
       *
       * The byte is now cleared directly. Faking the lost context instead
       * (pause -> Asphalt5Renderer.nativeInit(..., 1, ...) -> resume) made the
       * engine drop the GL names of the 2D textures it had already loaded --
       * the loading screens', the options' and the co-driver menu's sprites
       * then drew with texture 0, plain white (hardware 2026-11-19: "draw with
       * texture 0 ... from Lib3D::Flush2D"). The old way stays as the
       * fallback if the Game object cannot be found. */
      g_a5_movie_done = 0;
      static uintptr_t game_ptr_addr;
      if (!game_ptr_addr)
        game_ptr_addr = so_try_find_addr_rx(&g_a5_mod, "g_pMainGameClass");
      uint8_t *game = game_ptr_addr ? *(uint8_t **)game_ptr_addr : NULL;
      if (game) {
        game[7538] = 0;
        debugPrintf("[a5] movie skipped: the game's movie flag cleared (no context loss)\n");
      } else if (f_pause && f_resume && g_view_obj) {
        f_pause(g_jni_env, g_view_obj);
        f_rend_init(g_jni_env, g_rend_obj, A5_KEYBOARD, 1, w, h, lang);
        f_resize(g_jni_env, cls(A5_CLS_REND), w, h);
        f_resume(g_jni_env, g_view_obj);
        debugPrintf("[a5] movie skipped: surface re-created (context lost) and the activity resumed\n");
      }
    }
    {
      /* a music track ended: tell the engine, as MediaPlayer's completion
       * listener does on a phone (a5_jni.c, a5_music_finished) */
      static fn_i f_music_stop;
      static int looked;
      if (!looked++)
        f_music_stop = (fn_i)a5_native(J "GLMediaPlayer_nativeSetStopOnMusic");
      int done = a5_music_finished();
      if (done >= 0 && f_music_stop) {
        f_music_stop(g_jni_env, cls(A5_CLS_MEDIA), done);
        static int told;
        if (told++ < 8)
          debugPrintf("[snd] music %d ended: nativeSetStopOnMusic\n", done);
      }
    }
    if (frame < 3)
      debugPrintf("[a5] frame %llu rendered\n", (unsigned long long)frame);
    dcr_boost_frame_end(++frame);
    if (frame % 600 == 0) {
      debugPrintf("[a5] %llu frames, %d Java objects\n", (unsigned long long)frame, jni_live_objects());
      log_flush_ring();
    }
  }
  rt_applet_stop();
  if (f_pause && g_view_obj)
    f_pause(g_jni_env, g_view_obj); /* the game saves on onPause */
  a5_sound_shutdown();
  debugPrintf("[a5] exit after %llu frames\n", (unsigned long long)frame);
  log_flush_ring();
}
