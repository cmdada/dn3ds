//
//  n3ds_compat.h - platform shims for the 3DS (devkitARM + newlib).
//
//  Based on chocolate_duke3D's unix_compat.h; differences are called out
//  individually so it stays obvious what the 3DS actually lacks.
//

#ifndef DN3DS_COMPAT_H
#define DN3DS_COMPAT_H

#define PLATFORM_UNIX 1
#define PLATFORM_N3DS 1
#define PLATFORM_SUPPORTS_SDL

#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>        // strcasecmp lives here on newlib, not string.h
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
// No <sys/uio.h> on devkitARM newlib; only the networking code wanted it.

#define kmalloc(x)  malloc(x)
#define kkmalloc(x) malloc(x)
#define kfree(x)    free(x)
#define kkfree(x)   free(x)

#ifdef FP_OFF
#undef FP_OFF
#endif
// The 1996 code casts pointers to 32-bit ints everywhere. Valid here because
// ARMv6K is 32-bit: sizeof(void*) == sizeof(int32_t) == 4.
#define FP_OFF(x) ((int32_t)(x))

#ifndef max
#define max(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef min
#define min(x, y) (((x) < (y)) ? (x) : (y))
#endif

#define __int64 int64_t

// SD access goes through a newlib devoptab; no text/binary distinction.
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define stricmp  strcasecmp
#define strcmpi  strcasecmp

// The game's own itoa/strupr/strlwr take uint8_t*; newlib also declares itoa,
// over char*, and they collide. Defined *after* <stdlib.h> so newlib keeps its
// name and every reference in the game tree moves together.
#define itoa    dn3ds_itoa
#define strupr  dn3ds_strupr
#define strlwr  dn3ds_strlwr

#ifndef S_IREAD
#define S_IREAD S_IRUSR
#endif

// Netplay is out of scope; keep the engine's dummy multiplayer path.
#define USER_DUMMY_NETWORK 1
#undef  UDP_NETWORKING

#endif  // DN3DS_COMPAT_H
