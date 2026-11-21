/* a5_config.c -- config.ini for Asphalt 5 HD (the runtime's INI engine,
 * rt_cfg.c, reads and writes it; this is the table). MIT. */
#include <string.h>
#include <switch.h>

#include "rt_cfg.h"
#include "util.h"

#include "a5.h"

A5Config g_a5_cfg = {.fps60 = 0, .steer_gain = 0.6f, .touch = 1, .motion_gain = 1.5f, .log_keys = 1};

int g_a5_cooperative = 0;
int g_a5_dump_combo = 1;
int g_a5_trace_files = 0;
static int g_boost = 1;
static int g_lang_choice = 0;
static int g_fps_choice = 1; /* "60,30" */
static int g_size_choice = 0; /* "854x480,800x480,1280x720" */

/* GameInstaller's language table (classes.dex): eng, fra, deu, spa, ita,
 * jpn, ... in that order. */
#define A5_LANGS "auto,en,fr,de,es,it,ja"

static const CfgOpt k_opts[] = {
    CFG_ROW_RESOLUTION("auto",
                       "Rendering resolution: auto (1080 if docked when the game starts, 720 in\n"
                       "# handheld), 1080 or 720."),
    {"display", "frame_rate", "30", "60 or 30 frames a second. The game was made for 30 on 2010 phones.",
     CFG_CHOICE, "60,30", &g_fps_choice, 0, 0, 0, 0},
    {"display", "game_size", "854x480",
     "The size the game draws at, scaled to the screen. The game only lays its\n"
     "# menus out for phone screens: 854x480 (default) or 800x480. 1280x720 puts\n"
     "# the menus in a corner.",
     CFG_CHOICE, "854x480,800x480,1280x720", &g_size_choice, 0, 0, 0, 0},
    {"controls", "steer_gain", "0.6", "How far the left stick tilts the 'phone' (0.2 - 1.5).", CFG_FLOAT, NULL,
     &g_a5_cfg.steer_gain, 0.2f, 1.5f, 0, 0},
    {"controls", "steering", "stick",
     "stick: the left stick steers. motion: tilt the Switch (handheld), the Pro\n"
     "# Controller or the Joy-Con grip like a steering wheel. both: either.",
     CFG_CHOICE, "stick,motion,both", &g_a5_cfg.steer_mode, 0, 0, 0, 0},
    {"controls", "motion_gain", "1.5", "Motion steering: how strongly a tilt steers (0.5 - 4).", CFG_FLOAT, NULL,
     &g_a5_cfg.motion_gain, 0.5f, 4.0f, 0, 0},
    {"controls", "motion_invert", "false", "Motion steering goes the wrong way: set this to true.", CFG_BOOL, NULL,
     &g_a5_cfg.motion_invert, 0, 0, 0, 0},
    {"controls", "touch_screen", "true", "The touch screen works in handheld.", CFG_BOOL, NULL, &g_a5_cfg.touch, 0,
     0, 0, 0},
    {"game", "language", "auto", "Game language: auto (the Switch's), en, fr, de, es, it or ja.", CFG_CHOICE,
     A5_LANGS, &g_lang_choice, 0, 0, 0, 0},
    CFG_ROW_BOOST("CPU at 1785 MHz while the game loads (frames over 50 ms), normal otherwise.", &g_boost),
    CFG_ROW_GL_SELFTEST(NULL),
    {"debug", "cooperative", "false",
     "false: the game's threads are time-sliced every 10 ms. true: they take\n"
     "# turns only when one waits.",
     CFG_BOOL, NULL, &g_a5_cooperative, 0, 0, 0, 0},
    {"debug", "gl_off", "",
     "Bring-up: GL states the game may not switch on, to find the one that hides\n"
     "# the 3D world. A list of: fog scissor cull alpha depth lighting blend\n"
     "# mipmap (mipmap: textures filtered without their smaller levels)\n"
     "# (e.g. gl_off = mipmap). Empty: none.",
     CFG_TEXT, NULL, g_a5_cfg.gl_off, 0, 0, sizeof g_a5_cfg.gl_off, 0},
    {"debug", "log_keys", "true", "Write every button the game is sent (Android key code) to debug.log.", CFG_BOOL,
     NULL, &g_a5_cfg.log_keys, 0, 0, 0, 0},
    {"debug", "trace_files", "false", "Write every file the game opens to debug.log, with the result (slow).",
     CFG_BOOL, NULL, &g_a5_trace_files, 0, 0, 0, 0},
    {"debug", "hang_dump", "true",
     "Hold L + R + Minus for 2 s: the game pauses 12 s and debug.log gets\n"
     "# where every thread is (for bug reports of hangs).",
     CFG_BOOL, NULL, &g_a5_dump_combo, 0, 0, 0, 0},
    CFG_ROW_BOOT_LOG("Show the start-up log on screen.", NULL),
    CFG_ROW_LOG_JNI("Write every Java call the game makes to debug.log (slow; for bring-up).", NULL),
};

static void apply(void) {
  g_a5_cfg.fps60 = g_fps_choice == 0;
  static const int k_sizes[3][2] = {{854, 480}, {800, 480}, {1280, 720}};
  g_a5_cfg.game_w = k_sizes[g_size_choice % 3][0];
  g_a5_cfg.game_h = k_sizes[g_size_choice % 3][1];
  if (g_a5_cfg.gl_off[0])
    debugPrintf("[config] GL states kept off (gl_off): %s\n", g_a5_cfg.gl_off);
  debugPrintf("[config] resolution %s, %d fps, steer %.2f, touch %s, boost %s\n",
              rt_config_get("display", "resolution"), g_a5_cfg.fps60 ? 60 : 30, (double)g_a5_cfg.steer_gain,
              g_a5_cfg.touch ? "on" : "off", g_boost ? "on" : "off");
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .version = 1,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }

int a5_language(void) {
  if (g_lang_choice > 0)
    return g_lang_choice - 1;
  int lang = 0;
  u64 code = 0;
  SetLanguage sl;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
      switch (sl) {
      case SetLanguage_FR: case SetLanguage_FRCA: lang = 1; break;
      case SetLanguage_DE: lang = 2; break;
      case SetLanguage_ES: case SetLanguage_ES419: lang = 3; break;
      case SetLanguage_IT: lang = 4; break;
      case SetLanguage_JA: lang = 5; break;
      default: lang = 0; break;
      }
    }
    setExit();
  }
  debugPrintf("[config] language %d (the console's)\n", lang);
  return lang;
}

int dcr_path_traced(const char *p) {
  if (!p)
    return 0;
  return g_a5_trace_files || strstr(p, ".apk") != NULL;
}
