/*
 * dsl_n3ds.c - audiolib's low-level output driver.
 *
 * Replaces chocolate_duke3D's SDL_mixer-based dsl.c. audiolib already is the
 * mixer - MultiVoc hands us finished PCM - so plain SDL_OpenAudio is enough,
 * and on the 3DS that lands on NDSP.
 *
 * Contract with MultiVoc, unchanged from the DOS DMA ring: it owns one buffer
 * of NumDivisions divisions, MV_MixPage says which it last filled, and calling
 * CallBackFunc() makes it fill the next.
 */

#include <string.h>
#include <SDL.h>

#include "dsl.h"
#include "multivoc.h"
#include "_multivc.h"

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

/* multivoc.c defines this un-statted for drivers but declares it in no
   header. Plain int, not int32_t, to match that definition. */
extern int MV_MixPage;

static int   DSL_ErrorCode = DSL_Ok;
static int   mixer_initialized;

static void (*_CallBackFunc)(void);
static char *_BufferStart;
static int   _DivisionSize;     /* bytes in one division */
static int   _NumDivisions;
static int   _SampleRate;

/* Bytes still unread in the division MultiVoc last mixed. */
static int   _remainder;

static void DSL_SetErrorCode(int ErrorCode)
{
    DSL_ErrorCode = ErrorCode;
}

/* MultiVoc's DOS IRQ masking becomes real mutual exclusion against SDL's
   audio thread. Safe because only API-side entry points take these - the mix
   path never does, so the audio thread cannot deadlock against itself. The
   depth count is needed because they nest and SDL 1.2 does not promise a
   recursive audio lock. flags is vestigial (saved x86 FLAGS). */
static int dsl_lock_depth;

unsigned long DisableInterrupts(void)
{
    if (dsl_lock_depth++ == 0)
        SDL_LockAudio();
    return 0;
}

void RestoreInterrupts(unsigned long flags)
{
    (void)flags;
    if (dsl_lock_depth > 0 && --dsl_lock_depth == 0)
        SDL_UnlockAudio();
}

char *DSL_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
        case DSL_Warning:
        case DSL_Error:             return DSL_ErrorString(DSL_ErrorCode);
        case DSL_Ok:                return "3DS sound driver ok.";
        case DSL_SDLInitFailure:    return "SDL audio initialization failed.";
        case DSL_MixerActive:       return "Sound driver already initialized.";
        case DSL_MixerInitFailure:  return "Could not open the audio device.";
        default:                    return "Unknown 3DS sound driver error.";
    }
}

int DSL_Init(void)
{
    DSL_SetErrorCode(DSL_Ok);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        /* MultiVoc reports every failure as "Unknown Multivoc error code". */
        printf("dn3ds: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        fflush(stdout);
        DSL_SetErrorCode(DSL_SDLInitFailure);
        return DSL_Error;
    }
    printf("dn3ds: SDL audio subsystem up\n");
    fflush(stdout);
    return DSL_Ok;
}

void DSL_Shutdown(void)
{
    DSL_StopPlayback();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

unsigned DSL_GetPlaybackRate(void)
{
    return (unsigned)_SampleRate;
}

/* Tracks the leftover explicitly rather than assuming the device buffer is a
   whole number of divisions; getting that wrong drops each division's tail,
   which sounds like slightly fast playback rather than like a bug. */
static void mixer_callback(void *userdata, Uint8 *stream, int len)
{
    Uint8 *dst = stream;
    Uint8 *src;
    int    copysize;

    (void)userdata;

    /* Finish whatever is left of the division MultiVoc already mixed. */
    if (_remainder > 0) {
        copysize = min(len, _remainder);
        src = (Uint8 *)&_BufferStart[MV_MixPage * _DivisionSize];
        memcpy(dst, src + (_DivisionSize - _remainder), copysize);

        len       -= copysize;
        _remainder-= copysize;
        dst       += copysize;
    }

    while (len > 0) {
        _CallBackFunc();

        src = (Uint8 *)&_BufferStart[MV_MixPage * _DivisionSize];
        copysize = min(len, _DivisionSize);
        memcpy(dst, src, copysize);

        len -= copysize;
        dst += copysize;
        _remainder = _DivisionSize - copysize;
    }
}

int DSL_BeginBufferedPlayback(char *BufferStart, int BufferSize,
                              int NumDivisions, unsigned SampleRate,
                              int MixMode, void (*CallBackFunc)(void))
{
    SDL_AudioSpec desired, obtained;

    if (mixer_initialized) {
        DSL_SetErrorCode(DSL_MixerActive);
        return DSL_Error;
    }

    _CallBackFunc = CallBackFunc;
    _BufferStart  = BufferStart;
    _DivisionSize = BufferSize / NumDivisions;
    _NumDivisions = NumDivisions;
    _SampleRate   = SampleRate;
    _remainder    = 0;

    memset(&desired, 0, sizeof(desired));
    desired.freq     = SampleRate;
    desired.format   = (MixMode & SIXTEEN_BIT) ? AUDIO_S16LSB : AUDIO_U8;
    desired.channels = (MixMode & STEREO) ? 2 : 1;
    desired.callback = mixer_callback;
    desired.userdata = NULL;

    /* One division per device buffer keeps latency at one division. */
    desired.samples = (Uint16)(_DivisionSize
                               / ((MixMode & SIXTEEN_BIT) ? 2 : 1)
                               / ((MixMode & STEREO) ? 2 : 1));
    if (desired.samples < 256) desired.samples = 256;

    printf("dn3ds: SDL_OpenAudio rate=%d fmt=%s ch=%d samples=%u "
           "(division=%d bytes, %d divisions)\n",
           desired.freq, (MixMode & SIXTEEN_BIT) ? "S16LSB" : "U8",
           desired.channels, (unsigned)desired.samples,
           _DivisionSize, _NumDivisions);
    fflush(stdout);

    if (SDL_OpenAudio(&desired, &obtained) < 0) {
        printf("dn3ds: SDL_OpenAudio failed: %s\n", SDL_GetError());
        fflush(stdout);
        DSL_SetErrorCode(DSL_MixerInitFailure);
        return DSL_Error;
    }

    printf("dn3ds: audio open, obtained rate=%d ch=%d samples=%u\n",
           obtained.freq, obtained.channels, (unsigned)obtained.samples);
    fflush(stdout);

    mixer_initialized = 1;
    SDL_PauseAudio(0);

    DSL_SetErrorCode(DSL_Ok);
    return DSL_Ok;
}

void DSL_StopPlayback(void)
{
    if (!mixer_initialized)
        return;

    /* Close first: the callback runs on SDL's audio thread. */
    SDL_CloseAudio();

    mixer_initialized = 0;
    _CallBackFunc     = NULL;
    _BufferStart      = NULL;
    _remainder        = 0;
}
