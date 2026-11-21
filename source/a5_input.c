/* a5_input.c -- controllers and the touch screen -> Asphalt 5 HD.
 *
 * Asphalt 5 HD takes Android key codes (Asphalt5.nativeSetOnKeyDown / Up),
 * touches (Asphalt5.nativeTouchPressed / Moved / Released(x, y, pointer))
 * and the accelerometer (Asphalt5.nativeAccelerometer(x, y, z), on the
 * activity object). Asphalt5Renderer.nativeInit is told the phone has keys
 * (mHasKeyboard).
 *
 * The Android build's menus are touch only: no menu reads the game pad (its
 * key queue feeds GS_Run alone; hardware 2026-11-19, d-pad and A ignored).
 * So in the menus the controller works their touch areas (menu focus below):
 *   d-pad / left stick  the focus jumps from button to button, A touches it
 *   right stick         a free pointer instead, A / ZR / R3 touch with it
 *   L                   focus <-> free pointer
 *   B / -  4 (BACK)   +  82 (MENU)
 * Race (GS_Run): left stick X steers (accelerometer), A fires the nitro and
 * ZL brakes -- touches on the HUD's nitro button and brake pedal; the
 * pointer is off.
 * Which keys the game answers in a race is its choice; [debug] log_keys
 * lists every key sent, to tune the layout on hardware. MIT.
 */
#include <math.h>
#include <switch.h>

#include "jni.h"
#include "rt_pad.h"
#include "rt_window.h"
#include "util.h"

#include "a5.h"

const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.h */

/* Android key codes */
#define AK_BACK        4
#define AK_DPAD_UP     19
#define AK_DPAD_DOWN   20
#define AK_DPAD_LEFT   21
#define AK_DPAD_RIGHT  22
#define AK_DPAD_CENTER 23
#define AK_BUTTON_X    99
#define AK_BUTTON_Y    100
#define AK_BUTTON_L1   102
#define AK_BUTTON_R1   103
#define AK_BUTTON_L2   104
#define AK_BUTTON_R2   105
#define AK_START       108
#define AK_SELECT      109
#define AK_MENU        82

/* GL2JNILib.touchEvent actions */
#define TOUCH_UP   0
#define TOUCH_DOWN 1
#define TOUCH_MOVE 2

typedef void (*key_fn)(void *env, void *cls, int code);
typedef void (*touch_fn)(void *env, void *cls, int x, int y, int id);
typedef void (*accel_fn)(void *env, void *obj, float x, float y, float z);

static key_fn f_down, f_up;
static touch_fn f_press, f_move, f_release;
static accel_fn f_accel;
static int g_race; /* the current state is a GS_Run (a5_input_poll) */
static void nitro_request(const char *why);
static void nitro_hook(void);
static void *g_game_cls, *g_game_obj;
static PadState g_pad;
static int g_w = 1280, g_h = 720;
volatile int g_a5_dump_request;

/* ------------------------------------------------------------ motion */
/* config steering = motion / both: the accelerometer of the console
 * (handheld), the Pro Controller or the left Joy-Con. Gravity along the
 * controller's X axis is how far it is turned like a wheel. */
static HidSixAxisSensorHandle g_sx_handheld, g_sx_full, g_sx_dual[2];
static int g_sx_ok;

static void motion_init(void) {
  if (g_a5_cfg.steer_mode == 0)
    return;
  int ok = 0;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sx_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_handheld)))
    ok |= 1;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sx_full, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_full)))
    ok |= 2;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(g_sx_dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(g_sx_dual[0])))
    ok |= 4;
  g_sx_ok = ok;
  debugPrintf("[input] motion sensors: handheld %s, Pro Controller %s, Joy-Con %s\n", ok & 1 ? "ok" : "no",
              ok & 2 ? "ok" : "no", ok & 4 ? "ok" : "no");
}

/* -1 .. 1 (0: none / level), from the controller in use. */
static float motion_steer(void) {
  u32 style = padGetStyleSet(&g_pad);
  HidSixAxisSensorHandle h;
  if ((style & HidNpadStyleTag_NpadFullKey) && (g_sx_ok & 2))
    h = g_sx_full;
  else if ((style & HidNpadStyleTag_NpadJoyDual) && (g_sx_ok & 4))
    h = g_sx_dual[0];
  else if (g_sx_ok & 1)
    h = g_sx_handheld;
  else
    return 0.0f;
  HidSixAxisSensorState st;
  if (!hidGetSixAxisSensorStates(h, &st, 1))
    return 0.0f;
  float x = st.acceleration.x * g_a5_cfg.motion_gain; /* in g */
  if (g_a5_cfg.motion_invert)
    x = -x;
  return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

/* ------------------------------------------------------------ setup */
void a5_input_init(void) {
#define J "Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_"
  f_down = (key_fn)a5_native(J "nativeSetOnKeyDown");
  f_up = (key_fn)a5_native(J "nativeSetOnKeyUp");
  f_press = (touch_fn)a5_native(J "nativeTouchPressed");
  f_move = (touch_fn)a5_native(J "nativeTouchMoved");
  f_release = (touch_fn)a5_native(J "nativeTouchReleased");
  f_accel = (accel_fn)a5_native(J "nativeAccelerometer");
  g_game_cls = jni_class(A5_CLS_GAME)->obj;
  g_game_obj = jni_singleton(A5_CLS_GAME);
  dcr_window_size(&g_w, &g_h);
  rt_pad_setup(1, 1);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  motion_init();
  nitro_hook();
  debugPrintf("[input] keys %s, touch %s, accelerometer %s\n", f_down && f_up ? "ok" : "MISSING",
              f_press && f_move && f_release ? "ok" : "MISSING", f_accel ? "ok" : "MISSING");
}

/* ------------------------------------------------------------ keys */
static void key(int code, int down) {
  key_fn f = down ? f_down : f_up;
  if (!f)
    return;
  if (g_a5_cfg.log_keys)
    debugPrintf("[input] key %d %s\n", code, down ? "down" : "up");
  f(g_jni_env, g_game_cls, code);
}

static void touch(int action, int x, int y, int id) {
  touch_fn f = action == TOUCH_DOWN ? f_press : action == TOUCH_MOVE ? f_move : f_release;
  if (f)
    f(g_jni_env, g_game_cls, x, y, id);
}

static const struct {
  u64 btn;
  int code;
} k_map[] = {
    {HidNpadButton_B, AK_BACK},        {HidNpadButton_X, AK_BUTTON_Y},
    {HidNpadButton_Y, AK_BUTTON_X},    {HidNpadButton_R, AK_BUTTON_R1},
    {HidNpadButton_Plus, AK_MENU},     {HidNpadButton_Minus, AK_BACK},
};

/* The d-pad and, as a d-pad, the left stick (pushed past half) -- but the
 * stick only while it is not steering (a race: see a5_input_poll). */
#define DIR_UP    1u
#define DIR_DOWN  2u
#define DIR_LEFT  4u
#define DIR_RIGHT 8u
static const int k_dir_code[4] = {AK_DPAD_UP, AK_DPAD_DOWN, AK_DPAD_LEFT, AK_DPAD_RIGHT};
static unsigned g_dirs;

static void dirs_set(unsigned want) {
  for (int i = 0; i < 4; i++) {
    unsigned b = 1u << i;
    if ((want & b) && !(g_dirs & b))
      key(k_dir_code[i], 1);
    else if (!(want & b) && (g_dirs & b))
      key(k_dir_code[i], 0);
  }
  g_dirs = want;
}

/* ------------------------------------------------------------ touch */
/* The engine takes pointer ids 0 and 1 only (notifyTouchPress & co. assert
 * id <= 1, app.cpp:156, hardware 2026-11-19); the panel's finger ids keep
 * counting up. Each finger gets one of the two slots while it is down; the
 * right-stick pointer is slot 1. */
#define VF_POINTER 1
static int g_slot_finger[2] = {-1, -1};

static int slot_of(int finger, int take) {
  for (int s = 0; s < 2; s++)
    if (g_slot_finger[s] == finger)
      return s;
  if (!take)
    return -1;
  for (int s = 0; s < 2; s++)
    if (g_slot_finger[s] < 0) {
      g_slot_finger[s] = finger;
      return s;
    }
  return -1; /* a third finger: ignored */
}

static void poll_touch(void) {
  static HidTouchScreenState old;
  HidTouchScreenState st;
  if (!hidGetTouchScreenStates(&st, 1))
    return;
  if (!g_a5_cfg.touch)
    st.count = 0;
  /* The panel is 1280x720 whatever the rendering size. */
  float sx = g_w / 1280.0f, sy = g_h / 720.0f;
  for (int i = 0; i < st.count; i++) {
    int x = (int)(st.touches[i].x * sx), y = (int)(st.touches[i].y * sy), seen = 0;
    for (int j = 0; j < old.count; j++)
      seen |= old.touches[j].finger_id == st.touches[i].finger_id;
    int id = slot_of((int)st.touches[i].finger_id, 1);
    if (id < 0)
      continue;
    static int told;
    if (!seen && told++ < 40)
      debugPrintf("[touch] finger %d down at %d,%d (%.2f, %.2f of the screen)\n", id, x, y, (double)x / g_w,
                  (double)y / g_h);
    touch(seen ? TOUCH_MOVE : TOUCH_DOWN, x, y, id);
    /* a finger landing on the race HUD's nitro button: hand the press over */
    float fx = (float)x / g_w, fy = (float)y / g_h;
    if (!seen && g_race && fabsf(fx - 0.879f) < 0.09f && fabsf(fy - 0.917f) < 0.12f)
      nitro_request("finger");
  }
  for (int j = 0; j < old.count; j++) {
    int gone = 1;
    for (int i = 0; i < st.count; i++)
      gone &= st.touches[i].finger_id != old.touches[j].finger_id;
    int id = gone ? slot_of((int)old.touches[j].finger_id, 0) : -1;
    if (id >= 0) {
      touch(TOUCH_UP, (int)(old.touches[j].x * sx), (int)(old.touches[j].y * sy), id);
      g_slot_finger[id] = -1;
    }
  }
  old = st;
}

/* ------------------------------------------------------------ race */
/* In a race the current game state is a GS_Run: Game::CurrentState() on
 * g_pMainGameClass, its vtable against GS_Run's. */
static int in_race(void) {
  static uintptr_t a_game, a_cur, a_vt;
  static int looked;
  if (!looked++) {
    a_game = so_try_find_addr_rx(&g_a5_mod, "g_pMainGameClass");
    a_cur = so_try_find_addr_rx(&g_a5_mod, "_ZN4Game12CurrentStateEv");
    a_vt = so_try_find_addr_rx(&g_a5_mod, "_ZTV6GS_Run");
  }
  void *game = a_game ? *(void **)a_game : NULL;
  if (!game || !a_cur || !a_vt)
    return 0;
  uint32_t *st = ((uint32_t * (*)(void *)) a_cur)(game);
  return st && st[0] == (uint32_t)a_vt + 8;
}

/* ------------------------------------------------------------ nitro
 * The race reads its pedals from the GamePadManager (Game+7104): +8 keys
 * pressed this frame, +0xc released, +0x10 held. Scene::UpdateCars turns a
 * press of 0x4000 (the HUD's nitro button, GS_Run::Update) into the car's
 * nitro key 0x20 for CCar::UpdateNitro -- which stamps car+6428 with the
 * game time whenever it sees that key. On hardware (2026-11-19) the stamp
 * never moved, button pressed and gauge full: the press, a one-frame edge,
 * never reached the car. So the port hands it over itself: a press of A (or
 * a finger on the button) stays pending, and every UpdateKeysState adds
 * 0x4000 to the pressed keys until car+6428 moves (the car took it) or
 * 300 ms pass. UpdateKeysState (three getters) is replaced by a C copy. */
static volatile int g_nitro_pending;
static u64 g_nitro_since;
static int g_nitro_stamp;

static uint8_t *player_car(uint8_t **game_out) {
  static uintptr_t a_game;
  if (!a_game)
    a_game = so_try_find_addr_rx(&g_a5_mod, "g_pMainGameClass");
  uint8_t *game = a_game ? *(uint8_t **)a_game : NULL;
  if (game_out)
    *game_out = game;
  uint8_t *scene = game ? *(uint8_t **)(game + 7112) : NULL;
  if (!scene)
    return NULL;
  uint8_t *pi = *(uint8_t **)(scene + 101456);
  int idx = pi ? *(int *)(pi + 8) : -1;
  return idx >= 0 && idx < 16 ? *(uint8_t **)(scene + 101500 + idx * 4) : NULL;
}

static void nitro_request(const char *why) {
  uint8_t *car = player_car(NULL);
  if (!car)
    return;
  g_nitro_stamp = *(int *)(car + 6428);
  g_nitro_since = armGetSystemTick();
  g_nitro_pending = 1;
  static int n;
  if (n++ < 20)
    debugPrintf("[nitro] %s: press handed to the car\n", why);
}

static void h_update_keys_state(uint8_t *mgr, int i) {
  uint8_t *gp = *(uint8_t **)(mgr + 4 + i * 4);
  uint32_t *s = (uint32_t *)(mgr + 8 + i * 12);
  s[0] = *(uint32_t *)gp | *(uint32_t *)(gp + 0x18);
  s[1] = *(uint32_t *)(gp + 0x1c);
  s[2] = *(uint32_t *)(gp + 0xc) | *(uint32_t *)(gp + 0x20);
  if (i != 0)
    return;
  static int native;
  if ((s[0] & 0x4000) && !g_nitro_pending && native++ < 10)
    debugPrintf("[nitro] the game's own button press arrived (held 0x%x)\n", (unsigned)s[2]);
  if (!g_nitro_pending)
    return;
  uint8_t *game;
  uint8_t *car = player_car(&game);
  static int took, lost;
  if (!car) {
    g_nitro_pending = 0;
  } else if (*(int *)(car + 6428) != g_nitro_stamp) {
    g_nitro_pending = 0;
    if (took++ < 20)
      debugPrintf("[nitro] the car took the press (state %d, gauge %g)\n", *(int *)(car + 6384),
                  (double)*(float *)(car + 6376));
  } else if (armTicksToNs(armGetSystemTick() - g_nitro_since) > 300000000ull) {
    g_nitro_pending = 0;
    if (lost++ < 20)
      debugPrintf("[nitro] not taken in 300 ms: pressed 0x%x held 0x%x, block %d, car flags 0x%x\n",
                  (unsigned)s[0], (unsigned)s[2], *(int *)(*(uint8_t **)(game + 7112) + 409612),
                  *(unsigned *)(car + 6028));
  } else {
    s[0] |= 0x4000;
  }
}

static void nitro_hook(void) {
  uintptr_t a = so_try_find_addr_rx(&g_a5_mod, "_ZN14GamePadManager15UpdateKeysStateEi");
  if (a)
    hook_arm(a, (uintptr_t)h_update_keys_state);
  debugPrintf("[nitro] GamePadManager::UpdateKeysState %s\n", a ? "replaced (nitro hand-over)" : "NOT FOUND");
}

/* A held virtual finger at a fixed spot of the 854x480 game screen (the
 * race HUD: nitro button bottom right, brake pedal bottom left). */
typedef struct {
  float fx, fy;
  int finger, down;
} VTouch;
static VTouch g_nitro = {0.879f, 0.917f, 0x10000, 0}, g_brake = {0.086f, 0.944f, 0x10001, 0};

static void vtouch(VTouch *v, int want) {
  int x = (int)(v->fx * g_w), y = (int)(v->fy * g_h);
  if (want && !v->down) {
    int id = slot_of(v->finger, 1);
    if (id < 0)
      return;
    touch(TOUCH_DOWN, x, y, id);
    v->down = 1;
  } else if (!want && v->down) {
    int id = slot_of(v->finger, 0);
    if (id >= 0) {
      touch(TOUCH_UP, x, y, id);
      g_slot_finger[id] = -1;
    }
    v->down = 0;
  } else if (want && v->down) {
    int id = slot_of(v->finger, 0);
    if (id >= 0)
      touch(TOUCH_MOVE, x, y, id);
  }
}

/* The pointer (a5_cursor.c draws it): the right stick moves it, R3 is a
 * finger at its tip (press, drag, let go). Hidden after 3 s of rest. */
static float g_ptr_x = 640.0f, g_ptr_y = 360.0f;
static int g_ptr_down, g_ptr_on;
static u64 g_ptr_last;

/* ------------------------------------------------------------ menu focus
 * Every menu screen is a gxGameState whose touch areas are a list of
 * RectEntry (gxGameState+0x10: RectEntry**, +0x18: count; a RectEntry starts
 * with x1 y1 x2 y2 as floats in game pixels, the id at +0x24) -- the same
 * list its touch handling searches (gxGameState::FindRect). The d-pad and
 * the left stick move a focus from area to area, A touches the focused one:
 * the menus behave as if they had key navigation. A hidden area is moved off
 * the screen by the game (HideRectangle), so only areas on screen count. */
typedef struct {
  float x, y;
  int id;
} Area;
static Area g_area[64];
static int g_narea;
static int g_focus_mode = 1;            /* 1: focus, 0: free pointer */
static float g_focus_x = -1, g_focus_y = -1;
static int g_focus_id = -1;

static void *cur_state(void) {
  static uintptr_t a_game, a_cur;
  static int looked;
  if (!looked++) {
    a_game = so_try_find_addr_rx(&g_a5_mod, "g_pMainGameClass");
    a_cur = so_try_find_addr_rx(&g_a5_mod, "_ZN4Game12CurrentStateEv");
  }
  void *game = a_game ? *(void **)a_game : NULL;
  return game && a_cur ? ((void *(*)(void *))a_cur)(game) : NULL;
}

static void read_areas(void) {
  g_narea = 0;
  uint8_t *st = cur_state();
  if (!st)
    return;
  float **arr = *(float ***)(st + 0x10);
  int n = *(int *)(st + 0x18);
  if (!arr || n <= 0 || n > 512)
    return;
  for (int i = 0; i < n && g_narea < 64; i++) {
    float *r = arr[i];
    if (!r)
      continue;
    float x1 = r[0], y1 = r[1], x2 = r[2], y2 = r[3];
    if (!(x2 > x1 + 2 && y2 > y1 + 2)) /* also catches NaN */
      continue;
    if (x1 >= g_w || y1 >= g_h || x2 <= 0 || y2 <= 0) /* hidden: moved off screen */
      continue;
    if (x2 - x1 > g_w * 0.9f && y2 - y1 > g_h * 0.9f) /* a whole-screen catch-all */
      continue;
    float cx = (x1 < 0 ? 0 : x1) + ((x2 > g_w ? g_w : x2) - (x1 < 0 ? 0 : x1)) * 0.5f;
    float cy = (y1 < 0 ? 0 : y1) + ((y2 > g_h ? g_h : y2) - (y1 < 0 ? 0 : y1)) * 0.5f;
    g_area[g_narea++] = (Area){cx, cy, *(int *)((uint8_t *)r + 0x24)};
  }
  /* Bring-up (the yes / no questions could not be reached with the d-pad,
   * hardware 2026-11-20): each new set of areas, with the screen it is on
   * (the first 40), to see where a question's buttons are. */
  static void *last_st;
  static uint32_t last_sig;
  static int dumps;
  uint32_t sig = (uint32_t)n * 2654435761u;
  for (int i = 0; i < n; i++)
    if (arr[i])
      sig = (sig ^ (uint32_t)(*(int *)((uint8_t *)arr[i] + 0x24) + 1)) * 16777619u ^ (uint32_t)(int)arr[i][0];
  if ((st != last_st || sig != last_sig) && dumps < 40) {
    dumps++;
    char who[96];
    dcr_addr_name(*(uint32_t *)st, who, sizeof who);
    debugPrintf("[focus] %s: %d areas, %d on screen\n", who, n, g_narea);
    for (int i = 0; i < n && i < 24; i++) {
      float *r = arr[i];
      if (r)
        debugPrintf("[focus]   id %d: %.0f,%.0f - %.0f,%.0f\n", *(int *)((uint8_t *)r + 0x24), (double)r[0],
                    (double)r[1], (double)r[2], (double)r[3]);
    }
  }
  last_st = st, last_sig = sig;
}

/* the area nearest to (x, y) */
static int nearest_area(float x, float y) {
  int best = -1;
  float bd = 1e30f;
  for (int i = 0; i < g_narea; i++) {
    float dx = g_area[i].x - x, dy = g_area[i].y - y, d = dx * dx + dy * dy;
    if (d < bd)
      bd = d, best = i;
  }
  return best;
}

/* the next area from the focus in a direction (dx, dy = -1/0/1; y down) */
static int step_area(int from, int dx, int dy) {
  int best = -1;
  float bs = 1e30f, fx = g_area[from].x, fy = g_area[from].y;
  for (int i = 0; i < g_narea; i++) {
    if (i == from)
      continue;
    float ax = g_area[i].x - fx, ay = g_area[i].y - fy;
    float along = ax * dx + ay * dy, across = fabsf(ax * dy - ay * dx);
    if (along < 4.0f)
      continue;
    float score = along + 2.5f * across;
    if (score < bs)
      bs = score, best = i;
  }
  return best;
}

/* Returns 1 when the focus is in charge this frame (g_ptr_x / y set to it). */
static int focus_update(const float *sticks, u64 now, u64 down) {
  read_areas();
  if (!g_narea)
    return 0;
  const u64 dpad = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
  int dx = 0, dy = 0;
  if (now & HidNpadButton_Left)
    dx = -1;
  else if (now & HidNpadButton_Right)
    dx = 1;
  else if (now & HidNpadButton_Up)
    dy = -1;
  else if (now & HidNpadButton_Down)
    dy = 1;
  else if (fabsf(sticks[0]) > 0.6f && fabsf(sticks[0]) > fabsf(sticks[1]))
    dx = sticks[0] > 0 ? 1 : -1;
  else if (fabsf(sticks[1]) > 0.6f)
    dy = sticks[1] > 0 ? -1 : 1;
  int moving = dx || dy;
  if (moving || (down & dpad))
    g_focus_mode = 1;
  if (!g_focus_mode)
    return 0;

  /* where the focus is now: the same area if it is still there, else the
   * one nearest to where it was */
  int cur = -1;
  for (int i = 0; i < g_narea && g_focus_id >= 0; i++)
    if (g_area[i].id == g_focus_id)
      cur = i;
  if (cur < 0)
    cur = nearest_area(g_focus_x >= 0 ? g_focus_x : 0, g_focus_y >= 0 ? g_focus_y : 0);

  /* a step on a new direction at once, then again every 180 ms after 400 */
  static int last_dx, last_dy;
  static u64 next;
  u64 t = armGetSystemTick();
  int step = 0;
  if (moving && (dx != last_dx || dy != last_dy)) {
    step = 1;
    next = t + armNsToTicks(400000000ull);
  } else if (moving && t >= next) {
    step = 1;
    next = t + armNsToTicks(180000000ull);
  }
  last_dx = dx, last_dy = dy;
  if (step) {
    int n = step_area(cur, dx, dy);
    if (n >= 0)
      cur = n;
  }
  g_focus_id = g_area[cur].id;
  g_focus_x = g_area[cur].x, g_focus_y = g_area[cur].y;
  return 1;
}

static void pointer_update(const float *sticks, u64 now) {
  u64 t = armGetSystemTick();
  static u64 prev;
  float dt = prev ? (float)armTicksToNs(t - prev) / 1e9f : 0.016f;
  prev = t;
  if (dt > 0.1f)
    dt = 0.1f;
  float x = sticks[2], y = sticks[3];
  /* the right stick takes the free pointer (from where the focus was) */
  if (g_ptr_on && x * x + y * y > 0.25f * 0.25f && g_focus_mode) {
    g_focus_mode = 0;
    if (g_focus_x >= 0)
      g_ptr_x = g_focus_x, g_ptr_y = g_focus_y;
  }
  static u64 old_now;
  u64 dn = now & ~old_now;
  old_now = now;
  int focused = g_ptr_on && focus_update(sticks, now, dn);
  if (focused) {
    x = y = 0.0f;
    if (!g_ptr_down) /* a held touch stays where it began */
      g_ptr_x = g_focus_x, g_ptr_y = g_focus_y;
  } else if (x * x + y * y < sticks[0] * sticks[0] + sticks[1] * sticks[1]) {
    x = sticks[0], y = sticks[1]; /* no areas: the left stick moves it too */
  }
  if (!focused && (now & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right))) {
    x = (now & HidNpadButton_Right) ? 0.7f : (now & HidNpadButton_Left) ? -0.7f : 0.0f;
    y = (now & HidNpadButton_Up) ? 0.7f : (now & HidNpadButton_Down) ? -0.7f : 0.0f;
  }
  float m = sqrtf(x * x + y * y);
  if (m > 0.15f) {
    float speed = 250.0f + 1150.0f * (m - 0.15f) * (m - 0.15f) / (0.85f * 0.85f); /* px/s */
    float k = speed * dt * (g_h / 720.0f) / m;
    g_ptr_x += x * k;
    g_ptr_y -= y * k;
    g_ptr_x = g_ptr_x < 0 ? 0 : g_ptr_x > g_w - 1 ? (float)(g_w - 1) : g_ptr_x;
    g_ptr_y = g_ptr_y < 0 ? 0 : g_ptr_y > g_h - 1 ? (float)(g_h - 1) : g_ptr_y;
    g_ptr_last = t;
    if (g_ptr_down)
      touch(TOUCH_MOVE, (int)g_ptr_x, (int)g_ptr_y, VF_POINTER);
  }
  if (!g_ptr_on) {
    if (g_ptr_down) {
      touch(TOUCH_UP, (int)g_ptr_x, (int)g_ptr_y, VF_POINTER);
      g_ptr_down = 0;
    }
    a5_cursor_set(g_ptr_x, g_ptr_y, 0, 0, 0);
    return;
  }
  int tap = (now & (HidNpadButton_StickR | HidNpadButton_ZR | HidNpadButton_A)) != 0;
  if (tap && !g_ptr_down) {
    touch(TOUCH_DOWN, (int)g_ptr_x, (int)g_ptr_y, VF_POINTER);
    g_ptr_down = 1;
    g_ptr_last = t;
  } else if (!tap && g_ptr_down) {
    touch(TOUCH_UP, (int)g_ptr_x, (int)g_ptr_y, VF_POINTER);
    g_ptr_down = 0;
    g_ptr_last = t;
  }
  a5_cursor_set(g_ptr_x, g_ptr_y, 1, g_ptr_down, focused); /* focus: crosshair */
}

/* ------------------------------------------------------------ poll */
void a5_input_poll(void) {
  float sticks[4] = {0};
  static u64 old;
  padUpdate(&g_pad); /* rt_pad_read only reads the state padUpdate took */
  u64 now = rt_pad_read(&g_pad, sticks);
  u64 down = now & ~old, up = old & ~now;

  for (unsigned i = 0; i < sizeof k_map / sizeof k_map[0]; i++) {
    if (down & k_map[i].btn)
      key(k_map[i].code, 1);
    if (up & k_map[i].btn)
      key(k_map[i].code, 0);
  }

  /* checked every 4th frame: a state change is not that quick */
  static int race_tick;
  int race = g_race;
  if (!(race_tick++ & 3)) {
    int r = in_race();
    if (r != race)
      debugPrintf("[input] %s\n", r ? "race: A nitro, ZL brake, left stick steers" : "menus: d-pad / stick, A ok");
    g_race = race = r;
    if (race) /* only for the [focus] log: a race's screens have areas too */
      read_areas();
  }

  /* A: the nitro button in a race, DPAD_CENTER elsewhere (a key held across
   * the change is let go where it began). */
  static int a_key;
  int a_now = (now & HidNpadButton_A) != 0;
  if (a_now && !a_key && !g_nitro.down) {
    if (race)
      vtouch(&g_nitro, 1);
    /* menus: A is the pointer's tap (pointer_update) */
  } else if (a_now && g_nitro.down) {
    vtouch(&g_nitro, 1);
  } else if (!a_now) {
    if (a_key)
      key(AK_DPAD_CENTER, 0);
    a_key = 0;
    vtouch(&g_nitro, 0);
  }
  /* the press itself goes to the car through UpdateKeysState (nitro above) */
  if (race && (down & HidNpadButton_A))
    nitro_request("A");

  /* ZL: the brake pedal, in a race only */
  vtouch(&g_brake, race && (now & HidNpadButton_ZL));

  /* L: focus (d-pad picks the screen's buttons) <-> free pointer */
  g_ptr_on = !race;
  if (!race && (down & HidNpadButton_L)) {
    g_focus_mode = !g_focus_mode;
    if (!g_focus_mode && g_focus_x >= 0)
      g_ptr_x = g_focus_x, g_ptr_y = g_focus_y;
    debugPrintf("[input] menus: %s\n", g_focus_mode ? "focus (d-pad / stick, A)" : "free pointer (right stick, A / ZR)");
  }

  /* d-pad, and the left stick as one while it is near an axis and pushed
   * far: menus. Steering takes the stick's X all the time (the game ignores
   * the tilt outside a race), so the stick's left / right are only keys when
   * steering is by motion. */
  unsigned want = 0;
  u64 dn = now; /* menus: the d-pad and the stick move the pointer instead */
  float lx = sticks[0], ly = sticks[1];
  if (!race)
    dn = 0, lx = ly = 0.0f;
  if (dn & HidNpadButton_Up)
    want |= DIR_UP;
  if (dn & HidNpadButton_Down)
    want |= DIR_DOWN;
  if (dn & HidNpadButton_Left)
    want |= DIR_LEFT;
  if (dn & HidNpadButton_Right)
    want |= DIR_RIGHT;
  if (!race && ly > 0.6f && fabsf(lx) < 0.5f)
    want |= DIR_UP;
  if (!race && ly < -0.6f && fabsf(lx) < 0.5f)
    want |= DIR_DOWN;
  if (!race || g_a5_cfg.steer_mode == 1) {
    if (lx < -0.6f && fabsf(ly) < 0.5f)
      want |= DIR_LEFT;
    if (lx > 0.6f && fabsf(ly) < 0.5f)
      want |= DIR_RIGHT;
  }
  dirs_set(want);

  pointer_update(sticks, now);

  /* Steering: tilt. A phone held in landscape reports gravity on Y when it
   * is turned like a wheel; 9.81 is a full tilt. Axis and sign to check on
   * hardware (motion_invert / steer_gain help meanwhile). */
  if (f_accel) {
    float x = sticks[0];
    if (fabsf(x) < 0.12f)
      x = 0.0f;
    x *= g_a5_cfg.steer_gain;
    if (g_a5_cfg.steer_mode == 1)
      x = motion_steer();
    else if (g_a5_cfg.steer_mode == 2) {
      float mo = motion_steer();
      x = fabsf(x) > fabsf(mo) ? x : mo;
    }
    f_accel(g_jni_env, g_game_obj, 0.0f, 9.81f * x, 9.81f);
  }

  /* L + R + Minus held 2 s: ask for a thread dump (a5_main.c). */
  extern int g_a5_dump_combo;
  static uint64_t combo_since;
  static int combo_fired;
  const u64 combo = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus;
  if (g_a5_dump_combo && (now & combo) == combo) {
    uint64_t t = armTicksToNs(armGetSystemTick());
    if (!combo_since)
      combo_since = t;
    else if (!combo_fired && t - combo_since > 2000000000ull) {
      g_a5_dump_request = 1;
      combo_fired = 1; /* once per hold */
    }
  } else {
    combo_since = 0;
    combo_fired = 0;
  }

  poll_touch();
  old = now;
}
