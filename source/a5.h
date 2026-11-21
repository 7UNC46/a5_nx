/* a5.h -- what the port's files share. MIT. */
#ifndef A5_H
#define A5_H
#include <stdint.h>

#include "so_util.h"

extern so_module g_a5_mod;
void *a5_native(const char *sym); /* a native of libasphalt5.so, or NULL */

/* The Java classes the engine is handed (as jclass / jobject). */
#define A5_JPKG       "com/gameloft/android/GAND/GloftA5HD/"
#define A5_CLS_GAME   A5_JPKG "Asphalt5"
#define A5_CLS_REND   A5_JPKG "Asphalt5Renderer"
#define A5_CLS_VIEW   A5_JPKG "Asphalt5GLSurfaceView"
#define A5_CLS_MEDIA  A5_JPKG "GLMediaPlayer"
#define A5_CLS_RES    A5_JPKG "GLResLoader"

/* a5_gl.c: the EGL window context (GLES 1.1), made before the first GL call. */
int a5_gl_init(void);
void a5_gl_swap(void);

/* a5_sound.c: GLMediaPlayer's sounds (SoundPool) and music (MediaPlayer). */
int a5_sound_init(void);
void a5_sound_shutdown(void);
void a5_sound_pause(int paused);
void a5_sound_register(int id, const char *android_path, int flags);
int a5_sound_play(int id, float vol, int loop, float pitch);       /* -> voice */
int a5_music_load(int id);
int a5_music_play(int id, float vol, int loop);
void a5_music_pause(int id, int paused);
void a5_music_stop(int id);
void a5_music_volume(int id, float vol);
int a5_music_playing(int id);
void a5_voice_pitch(int voice, float pitch);
void a5_voice_volume(int voice, float vol);
void a5_voice_pause(int voice, int paused);
void a5_voice_stop(int voice);
int a5_voice_playing(int voice);
int a5_voice_alive(int voice); /* not stopped and not at its end */
void a5_sound_stop_all(void);
void a5_sound_master(float vol);
float a5_sound_get_master(void);
void a5_stream_write(const int16_t *pcm, int frames, int chans, int rate);

/* a5_input.c: controllers + touch -> the engine. */
void a5_input_init(void);
void a5_input_poll(void);
extern volatile int g_a5_dump_request; /* L+R+Minus held 2 s */
/* a5_jni.c -> a5_main.c: a movie was asked for (and skipped); the activity's
 * pause / resume that ends it on a phone is still due. */
extern volatile int g_a5_movie_done;

/* a5_config.c: config.ini. */
typedef struct {
  int fps60;         /* 1: 60 fps, 0: 30 */
  float steer_gain;  /* left stick -> accelerometer tilt */
  int touch;         /* touch screen passed to the game */
  int steer_mode;    /* 0 left stick, 1 motion, 2 both */
  float motion_gain;
  int motion_invert;
  int log_keys;      /* log every key code sent */
  int game_w, game_h; /* the size the engine renders at (scaled to the screen) */
  char gl_off[96];    /* [debug] gl_off: GL states never enabled (bring-up) */
} A5Config;
extern A5Config g_a5_cfg;
/* The engine's language number (GameInstaller's table: 0 English,
 * 1 French, 2 German, 3 Spanish, 4 Italian, 5 Japanese...). */
int a5_language(void);

/* a5_cursor.c: the pointer, drawn before each swap. */
void a5_cursor_set(float x, float y, int shown, int pressed, int crosshair);

#endif /* A5_H */
