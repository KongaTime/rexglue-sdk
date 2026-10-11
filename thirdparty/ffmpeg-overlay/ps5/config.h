/*
 * FFmpeg's configuration for the PS5 (payload SDK homebrew, x86_64-sie-ps5).
 *
 * The bundled FFmpeg carries a configuration per desktop platform and none for
 * the console (thirdparty/FFmpeg/config.h stops with "no config"). The console
 * is FreeBSD-like on a Zen 2, so this takes the Linux x86_64 configuration and
 * turns off what the console's headers and libc lack. Only libavutil and
 * libavcodec are built, for XMA (WMA Pro) decoding. thirdparty/CMakeLists.txt
 * puts this directory ahead of FFmpeg's own on the PS5.
 */
#ifndef REX_FFMPEG_PS5_CONFIG_H
#define REX_FFMPEG_PS5_CONFIG_H

#include "config_linux_x86_64.h"

#undef OS_NAME
#define OS_NAME freebsd

/* Linux headers the console does not have */
#undef HAVE_ASM_TYPES_H
#define HAVE_ASM_TYPES_H 0
#undef HAVE_LINUX_PERF_EVENT_H
#define HAVE_LINUX_PERF_EVENT_H 0
#undef HAVE_SYS_SOUNDCARD_H
#define HAVE_SYS_SOUNDCARD_H 0

/* libc functions no module a title loads exports (the SDK's stubs);
 * cpu.c then counts CPUs with sysconf, which the platform layer answers */
#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
#undef HAVE_GLOB
#define HAVE_GLOB 0
/* No iconv in the console's libc (subtitle charset conversion only) */
#undef CONFIG_ICONV
#define CONFIG_ICONV 0
/* FreeBSD's <malloc.h> is an #error pointing at <stdlib.h> */
#undef HAVE_MALLOC_H
#define HAVE_MALLOC_H 0
/* Only for coloured log output; the console has no terminal */
#undef HAVE_ISATTY
#define HAVE_ISATTY 0

#endif /* REX_FFMPEG_PS5_CONFIG_H */
