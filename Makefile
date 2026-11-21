#---------------------------------------------------------------------------------
# Asphalt 5 HD -- Nintendo Switch wrapper (32-bit / AArch32, android32)
#
# Ships NO game code and NO game assets: the player's own APK and data folder
# are read at run time. Build with ./build.sh (the toolchain container).
#---------------------------------------------------------------------------------
TARGET               := a5_nx
# Pick an id no other android32 port uses (a6_nx ...10A6, a7_nx ...10A7).
PORT_NPDM_PROGRAM_ID := 0x01000000000010A5
PORT_NPDM_VERSION    := 0.3.0
PORT_NPDM_MAIN_STACK := 0x800000
# The engine reads its .bar chunks with many small freads.
PORT_CFLAGS += -DRT_STDIO_READ_BUF=65536
# Guest threads on one core (source/dcr_sched.c).
PORT_CFLAGS += -DRT_SCHED_SELFTEST=0
# Sound is GLMediaPlayer (a5_sound.c): no OpenSL ES.
RT_EXCLUDE += opensles.c

include runtime/runtime.mk

# stb_vorbis (public domain) is compiled inside a5_sound.c.
$(BUILD)/a5_sound.o: CFLAGS += -Wno-unused-value -Wno-unused-but-set-variable -Wno-sign-compare
