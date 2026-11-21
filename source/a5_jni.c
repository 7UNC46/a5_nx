/* a5_jni.c -- the Java side of Asphalt 5 HD, answered in C.
 *
 * The engine looks its Java methods up on the classes it is handed
 * (Asphalt5, GLMediaPlayer, GLResLoader). The names and signatures below are
 * the APK's own (classes.dex), cross-checked against the names in
 * libasphalt5.so. A method missing here falls through to the runtime's
 * default answer and is logged once as "[jni] unhandled ..." in debug.log.
 *
 * Store / billing calls get no purchase flow here: there is no store on the
 * Switch, so LaunchBilling and ReleaseBillingContext do nothing. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include <miniz/miniz.h>

#include "dcr_path.h"
#include "jni.h"
#include "port_config.h"
#include "rt_applet.h"
#include "util.h"

#include "a5.h"

/* ------------------------------------------------------------ helpers */
static jvalue ret_bytes(const void *p, int len) {
  JObj *a = jni_array('B', len);
  if (a && len > 0)
    memcpy(a->a.data, p, len);
  return jv_l(a);
}

static const char *arg_str(const jvalue *a, int i) { return a ? jni_utf(a[i].l) : ""; }

/* A name from the game, absolute or relative to its data folder, to a host
 * path (dcr_path.c + port_path_fixup). */
static const char *data_path(const char *name, char *out, size_t cap) {
  char tmp[DCR_PATH_MAX];
  if (name[0] != '/') {
    snprintf(tmp, sizeof tmp, A5_DATA_ANDROID "/%s", name);
    name = tmp;
  }
  return dcr_translate_path(name, out, cap);
}

static void *read_file(const char *path, long off, long want, long *got) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (want < 0)
    want = size - off;
  if (off < 0 || off > size || want < 0) {
    fclose(f);
    return NULL;
  }
  if (off + want > size)
    want = size - off;
  void *d = malloc(want > 0 ? (size_t)want : 1);
  fseek(f, off, SEEK_SET);
  *got = d ? (long)fread(d, 1, (size_t)want, f) : 0;
  fclose(f);
  return d;
}

/* ------------------------------------------------------------ GLResLoader */
static JNI_H_DECL(h_res_full) {
  char p[DCR_PATH_MAX];
  const char *name = arg_str(a, 0);
  long got = 0;
  void *d = read_file(data_path(name, p, sizeof p), 0, -1, &got);
  static int told;
  if (!d || told++ < 32)
    debugPrintf("[res] getResourceFull(\"%s\"): %s (%ld bytes)\n", name, d ? "ok" : "NOT FOUND", got);
  if (!d)
    return jv_l(NULL);
  jvalue r = ret_bytes(d, (int)got);
  free(d);
  return r;
}

/* getResourceFull(int): the APK's own resource "res_<n>" (drawable). */
static JNI_H_DECL(h_res_full_id) {
  int id = a ? a[0].i : 0;
  char want[32];
  snprintf(want, sizeof want, "/res_%d.", id);
  mz_zip_archive z;
  memset(&z, 0, sizeof z);
  void *d = NULL;
  size_t len = 0;
  if (mz_zip_reader_init_file(&z, dcr_apk_path(), 0)) {
    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n && !d; i++) {
      mz_zip_archive_file_stat st;
      if (mz_zip_reader_file_stat(&z, i, &st) && !strncmp(st.m_filename, "res/", 4) && strstr(st.m_filename, want))
        d = mz_zip_reader_extract_to_heap(&z, i, &len, 0);
    }
    mz_zip_reader_end(&z);
  }
  debugPrintf("[res] getResourceFull(res_%d): %s (%u bytes)\n", id, d ? "ok" : "NOT FOUND", (unsigned)len);
  if (!d)
    return jv_l(NULL);
  jvalue r = ret_bytes(d, (int)len);
  free(d);
  return r;
}

static JNI_H_DECL(h_res_bytes) {
  char p[DCR_PATH_MAX];
  long got = 0;
  void *d = read_file(data_path(arg_str(a, 0), p, sizeof p), a ? a[1].i : 0, a ? a[2].i : 0, &got);
  if (!d)
    return jv_l(NULL);
  jvalue r = ret_bytes(d, (int)got);
  free(d);
  return r;
}

static JNI_H_DECL(h_res_length) {
  char p[DCR_PATH_MAX];
  FILE *f = fopen(data_path(arg_str(a, 0), p, sizeof p), "rb");
  if (!f)
    return jv_i(-1);
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fclose(f);
  return jv_i((jint)len);
}

/* ------------------------------------------------------------ GLMediaPlayer
 * Sounds are the data folder's raw_NNN.glsnd (Ogg Vorbis), by number. The
 * engine's BaseSoundManager keeps several instances of a sound (a SoundPool
 * stream each, on a phone) and talks to Java by (sound, instance)
 * (libasphalt5.so, BaseSoundManager::getReadySound):
 *   isSoundLoaded(sound, inst)  -> inst when that instance is loaded, else -1;
 *                                  on -1 the engine makes a new instance and
 *                                  calls loadSound(sound, newInst)
 *   playSound(sound, inst, vol), stopSound / pauseSound / resumeSound(sound,
 *   inst), setVolume / setPitch(sound, inst, value), unloadSound(sound, inst)
 *   isSoundLoadedBig(sound) -> 0 / -1, loadSoundBig(sound), playSoundBig(sound,
 *   vol) ...: the music (MediaPlayer), one instance.
 * (Answering "loaded" for everything left the engine's instances unmade: no
 * sound, and a crash in BaseSoundManager::playEx at the race start, hardware
 * 2026-11-19.) */
#define MAX_IDS  1024
#define MAX_INST 32
static uint32_t g_loaded[MAX_IDS];        /* bit per loaded instance */
static int g_voice[MAX_IDS][MAX_INST];    /* last voice handle + 1 */
static uint8_t g_registered[MAX_IDS], g_big_loaded[MAX_IDS];

static int sound_id(const jvalue *a, int i) {
  int id = a ? a[i].i : -1;
  return id >= 0 && id < MAX_IDS ? id : -1;
}
static int inst_of(const jvalue *a, int i) {
  int n = a ? a[i].i : -1;
  return n >= 0 && n < MAX_INST ? n : -1;
}

static void register_sound(int id) {
  if (g_registered[id])
    return;
  char path[64];
  snprintf(path, sizeof path, A5_DATA_ANDROID "/raw_%03d.glsnd", id);
  a5_sound_register(id, path, 0);
  g_registered[id] = 1;
}

static void snd_log(const char *what, int id, int inst) {
  static int told;
  if (told++ < 24)
    debugPrintf("[snd] %s(%d, %d)\n", what, id, inst);
}

static int voice_for(const jvalue *a) {
  int id = sound_id(a, 0), n = inst_of(a, 1);
  return id >= 0 && n >= 0 ? g_voice[id][n] - 1 : -1;
}

/* isSoundLoaded(sound, instance), as the game's Java has it: one SoundPool
 * sample per sound, the instance argument unused -- 0 if the sound is loaded,
 * -1 if not. BaseSoundManager::getReadySound asks with instance -1 when every
 * instance it has is still marked playing (SoundPool never reports the end of
 * a sound, so that is always): 0 makes it reuse its instance 0. Answering -1
 * there made it create a new instance on every play, until past 32 the
 * effects went silent ("the sound goes after selecting several times",
 * hardware 2026-11-19). Instance 0 is the first one the engine creates and
 * loads, so it exists whenever this says 0. */
static JNI_H_DECL(h_is_loaded) {
  int id = sound_id(a, 0);
  return jv_i(id >= 0 && (g_loaded[id] & 1u) ? 0 : -1);
}
static JNI_H_DECL(h_load_sound) { /* (sound, inst[, context]) */
  int id = sound_id(a, 0), n = inst_of(a, 1);
  snd_log("loadSound", id, n);
  if (id >= 0 && n >= 0) {
    register_sound(id);
    g_loaded[id] |= 1u << n;
  }
  return jv_none();
}
static JNI_H_DECL(h_unload_sound) {
  int id = sound_id(a, 0), n = inst_of(a, 1);
  a5_voice_stop(voice_for(a));
  if (id >= 0 && n >= 0)
    g_loaded[id] &= ~(1u << n);
  return jv_none();
}
static JNI_H_DECL(h_play_sound) { /* (sound, inst, volume) */
  int id = sound_id(a, 0), n = inst_of(a, 1);
  snd_log("playSound", id, n);
  if (id < 0 || n < 0)
    return jv_none();
  register_sound(id);
  a5_voice_stop(g_voice[id][n] - 1); /* an instance plays one sound at a time */
  g_voice[id][n] = a5_sound_play(id, a[2].f, 0, 1.0f) + 1;
  return jv_none();
}
static JNI_H_DECL(h_stop_sound) { a5_voice_stop(voice_for(a)); return jv_none(); }
static JNI_H_DECL(h_pause_sound) { a5_voice_pause(voice_for(a), 1); return jv_none(); }
static JNI_H_DECL(h_resume_sound) { a5_voice_pause(voice_for(a), 0); return jv_none(); }
/* Bring-up (engine sound gone now and then, hardware 2026-11-20): the game
 * sets the pitch / volume of the engine sounds every frame. One it still
 * steers after its voice ended is a sound it believes is playing -- logged
 * (the first 30), with the pitch asked, to see which sounds go quiet and why. */
static void steer_check(const jvalue *a, const char *what) {
  static int told;
  int id = sound_id(a, 0), n = inst_of(a, 1);
  if (id < 0 || n < 0 || told >= 30 || !g_voice[id][n])
    return;
  if (!a5_voice_alive(g_voice[id][n] - 1)) {
    told++;
    debugPrintf("[snd] %s(%d, %d, %g) on a sound that has ended or was stopped\n", what, id, n, (double)a[2].f);
    g_voice[id][n] = 0; /* told once per play */
  }
}
static JNI_H_DECL(h_sound_volume) {
  steer_check(a, "setVolume");
  a5_voice_volume(voice_for(a), a[2].f);
  return jv_none();
}
static JNI_H_DECL(h_sound_pitch) {
  static int told;
  if ((a[2].f > 2.0f || a[2].f < 0.5f) && told++ < 8)
    debugPrintf("[snd] setPitch(%d, %d, %g): outside SoundPool's 0.5 - 2, clamped\n", a[0].i, a[1].i, (double)a[2].f);
  steer_check(a, "setPitch");
  a5_voice_pitch(voice_for(a), a[2].f);
  return jv_none();
}

static void music_forget(int id);
static JNI_H_DECL(h_is_loaded_big) {
  int id = sound_id(a, 0);
  return jv_i(id >= 0 && g_big_loaded[id] ? 0 : -1);
}
static JNI_H_DECL(h_load_big) { /* (sound[, context]) */
  int id = sound_id(a, 0);
  snd_log("loadSoundBig", id, 0);
  if (id >= 0) {
    register_sound(id);
    g_big_loaded[id] = 1;
  }
  return jv_none();
}
static JNI_H_DECL(h_unload_big) {
  int id = sound_id(a, 0);
  a5_music_stop(id);
  if (id >= 0)
    music_forget(id);
  if (id >= 0)
    g_big_loaded[id] = 0;
  return jv_none();
}
/* The end of a music track. On a phone MediaPlayer's OnCompletionListener
 * calls GLMediaPlayer.nativeSetStopOnMusic(currentIndex); the engine marks the
 * track stopped there and starts the next one (or the same one again) -- the
 * main menu's music did not come back once it ended (hardware 2026-11-19).
 * The track playSoundBig started last is watched here; the frame loop asks
 * a5_music_finished() and makes that call. setLoopBig(sound, loop) is the
 * MediaPlayer's setLooping: such a track loops in the mixer and never ends. */
static uint8_t g_big_loop[MAX_IDS];
static int g_music_cur = -1, g_music_paused, g_music_seen;
static void music_forget(int id) {
  if (id < 0 || id == g_music_cur)
    g_music_cur = -1;
}
int a5_music_finished(void) {
  if (g_music_cur < 0 || g_music_paused)
    return -1;
  if (a5_music_playing(g_music_cur)) {
    g_music_seen = 1;
    return -1;
  }
  if (!g_music_seen) /* never got going: no completion to report */
    return -1;
  int id = g_music_cur;
  g_music_cur = -1;
  return id;
}
static JNI_H_DECL(h_loop_big) { /* (sound, loop) */
  int id = sound_id(a, 0);
  if (id >= 0)
    g_big_loop[id] = a[1].z ? 1 : 0;
  return jv_none();
}
static JNI_H_DECL(h_play_big) { /* (sound, volume) */
  int id = sound_id(a, 0);
  snd_log("playSoundBig", id, 0);
  if (id >= 0) {
    register_sound(id);
    /* Java calls MediaPlayer.start(), which leaves a track that is already
     * playing alone (it only resumes a paused one): the menus call
     * playSoundBig on every screen change, and the music restarted each
     * time (hardware 2026-11-19). */
    if (id == g_music_cur || g_big_loop[id]) {
      if (a5_music_playing(id) || g_music_paused) {
        a5_music_pause(id, 0);
        a5_music_volume(id, a[1].f);
        g_music_paused = 0;
        return jv_none();
      }
    }
    a5_music_play(id, a[1].f, g_big_loop[id]);
    g_music_cur = g_big_loop[id] ? -1 : id;
    g_music_paused = 0, g_music_seen = 0;
  }
  return jv_none();
}
static JNI_H_DECL(h_stop_big) {
  int id = sound_id(a, 0);
  a5_music_stop(id);
  if (id >= 0)
    music_forget(id);
  return jv_none();
}
static JNI_H_DECL(h_pause_big) {
  int id = sound_id(a, 0);
  a5_music_pause(id, 1);
  if (id == g_music_cur)
    g_music_paused = 1;
  return jv_none();
}
static JNI_H_DECL(h_resume_big) {
  int id = sound_id(a, 0);
  a5_music_pause(id, 0);
  if (id == g_music_cur)
    g_music_paused = 0;
  return jv_none();
}
static JNI_H_DECL(h_volume_big) { a5_music_volume(sound_id(a, 0), a[1].f); return jv_none(); }
static JNI_H_DECL(h_media_playing) { return jv_z(a5_music_playing(sound_id(a, 0))); }
static JNI_H_DECL(h_stop_all) { a5_sound_stop_all(); return jv_none(); }
static JNI_H_DECL(h_stop_all_big) {
  a5_music_stop(-1);
  music_forget(-1);
  return jv_none();
}

/* ------------------------------------------------------------ Asphalt5 */
static JNI_H_DECL(h_exit) {
  debugPrintf("[a5] Asphalt5.Exit: leaving\n");
  rt_request_exit();
  return jv_none();
}
static JNI_H_DECL(h_empty_bytes) { return ret_bytes("", 0); }
/* loadMovie: on a phone Java plays the video in its own activity (MyVideoView)
 * while the game's pauses; when it ends the game's activity resumes, and
 * Game::Resume clears the "movie playing" flag GS_TrailerMovie waits on
 * (libasphalt5.so: set in GS_TrailerMovie::Create, byte +7538 of the Game).
 * No video here: the frame loop plays that pause / resume (a5_main.c). */
volatile int g_a5_movie_done;
static JNI_H_DECL(h_movie) {
  debugPrintf("[a5] loadMovie(\"%s\"): no video playback here, skipped\n", arg_str(a, 0));
  g_a5_movie_done = 1;
  return jv_none();
}
static JNI_H_DECL(h_loading) {
  static int last = -1;
  if (a && a[0].i != last && (a[0].i % 25 == 0))
    debugPrintf("[a5] loading %d\n", a[0].i);
  if (a)
    last = a[0].i;
  return jv_none();
}

/* ------------------------------------------------------------ the tables */
#define G  A5_CLS_GAME
#define MP A5_CLS_MEDIA
#define RL A5_CLS_RES
#define S  "Ljava/lang/String;"

const JMethodDef jni_method_defs[] = {
    /* Asphalt5 */
    {G, "Exit", "()V", h_exit},
    {G, "sendAppToBackground", "()V", jni_h_void},
    {G, "NotifyTrophy", "(I)V", jni_h_void},
    {G, "OpenGLive", "(I)V", jni_h_void},
    {G, "launchGetGames", "(I)V", jni_h_void},
    {G, "SetLoadingValuable", "(I)V", h_loading},
    {G, "loadMovie", "(" S ")V", h_movie},
    {G, "IsDoubleOption", "()I", jni_h_zero},
    {G, "GetDoubleOptionText1", "()[B", h_empty_bytes},
    {G, "GetDoubleOptionText2", "()[B", h_empty_bytes},
    {G, "GetDoubleOptionText3", "()[B", h_empty_bytes},
    {G, "LaunchBilling", "()V", jni_h_void},          /* no store here */
    {G, "ReleaseBillingContext", "()V", jni_h_void},

    /* GLResLoader */
    {RL, "getResourceFull", "(" S ")[B", h_res_full},
    {RL, "getResourceFull", "(I)[B", h_res_full_id},
    {RL, "getResourceBytes", "(" S "II)[B", h_res_bytes},
    {RL, "getResourceLength", "(" S ")I", h_res_length},

    /* GLMediaPlayer */
    {MP, "initSoundPoolArray", "()V", jni_h_void},
    {MP, "destroySoundPool", "()V", jni_h_void},
    {MP, "loadSound", NULL, h_load_sound},
    {MP, "loadSoundBig", NULL, h_load_big},
    {MP, "isSoundLoaded", "(II)I", h_is_loaded},
    {MP, "isSoundLoadedBig", "(I)I", h_is_loaded_big},
    {MP, "unloadSound", "(II)V", h_unload_sound},
    {MP, "unloadSoundBig", "(I)V", h_unload_big},
    {MP, "playSound", "(IIF)V", h_play_sound},
    {MP, "stopSound", "(II)V", h_stop_sound},
    {MP, "pauseSound", "(II)V", h_pause_sound},
    {MP, "resumeSound", "(II)V", h_resume_sound},
    {MP, "setVolume", "(IIF)V", h_sound_volume},
    {MP, "setPitch", "(IIF)V", h_sound_pitch},
    {MP, "resetSound", "(I)V", jni_h_void},
    {MP, "stopAllPool", "(I)V", h_stop_all},
    {MP, "stopAllSounds", "()V", h_stop_all},
    {MP, "playSoundBig", "(IF)V", h_play_big},
    {MP, "stopSoundBig", "(I)V", h_stop_big},
    {MP, "setLoopBig", NULL, h_loop_big},
    {MP, "pauseSoundBig", "(I)V", h_pause_big},
    {MP, "resumeSoundBig", "(I)V", h_resume_big},
    {MP, "setVolumeBig", "(IF)V", h_volume_big},
    {MP, "stopAllBig", "(I)V", h_stop_all_big},
    {MP, "isMediaPlaying", "(I)Z", h_media_playing},
    {MP, "loadMovie", "(" S ")V", h_movie},

    {NULL, NULL, NULL, NULL},
};

const JFieldDef jni_field_defs[] = {
    {"android/os/Build$VERSION", "SDK_INT", NULL, 8, NULL}, /* the game is from 2.2 days */
    {NULL, NULL, NULL, 0, NULL},
};

const char *const jni_class_supers[][2] = {{NULL, NULL}};
const char *const jni_missing_classes[] = {NULL};
