/* port_config.h -- Asphalt 5 HD for Switch (android32).
 *
 * com.gameloft.android.GAND.GloftA5HD 3.4.1. The engine is Gameloft's 2010
 * Asphalt 5 build: libasphalt5.so, 32-bit ARM, EABI4 (ARMv5TE code, soft
 * float), OpenGL ES 1.x (it links libGLESv1_CM). No SWP instructions and no
 * kernel-helper words in it. The APK's second library, libigp.so, is only
 * Gameloft's "more games" screen (IGPActivity) and is not loaded.
 * MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define PORT_TITLE   "Asphalt 5 HD"
#define PORT_NAME    "a5_nx"
#define PORT_PACKAGE "com.gameloft.android.GAND.GloftA5HD"
#define PORT_VERSION "0.3.0"
/* First line of debug.log: name, title and version */
#define PORT_BANNER  PORT_NAME " v" PORT_VERSION ": " PORT_TITLE

/* Where the library sits in the APK (an EABI4 build: "armeabi"). */
#define PORT_ABI_DIR "lib/armeabi/"
#define A5_LIB       "libasphalt5.so"

/* The game data: on a phone the "asphalt5" folder of the data download,
 * in /sdcard/gameloft/games/asphalt5/ (GameInstaller, GLMediaPlayer). Here
 * that folder is <game folder>/asphalt5/ (a5_main.c: port_path_fixup). */
#define A5_DATA_ANDROID "/sdcard/gameloft/games/asphalt5"
#define A5_DATA_DIR     "asphalt5"
#define RT_PATH_SD_SUBDIRS "gameloft"
#define RT_PATH_EXTRA_DIRS "asphalt5"

#endif /* PORT_CONFIG_H */
