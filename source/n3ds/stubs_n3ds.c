/*
 * stubs_n3ds.c - inert definitions for what this port leaves out.
 *
 * Netplay: chocolate_duke3D gets these from mmulti.c/multi.c, which is not
 * built here, but game.c/menues.c/premap.c still call them from single-player
 * paths - gameexitanycase() calls sendlogoff() unconditionally. dummy_multi.c
 * supplies only the connection variables, not these entry points.
 *
 * Music: MIDI needs a synth on the ARM11; sounds.c calls PlayMusic() anyway.
 */

#include <stdio.h>
#include "platform.h"

/* display.h declares these; _platform_init() assigns them. */
int32_t   _argc;
char    **_argv;

/* ---------------------------------------------------------------- netplay */

void Setup_UnstableNetworking(void) {}
void Setup_StableNetworking(void)   {}

void callcommit(void) {}
void initcrc(void)    {}

int32_t getcrc(uint8_t *buffer, short bufleng)
{
    (void)buffer; (void)bufleng;
    return 0;
}

void initmultiplayers(uint8_t damultioption, uint8_t dacomrateoption,
                      uint8_t dapriority)
{
    (void)damultioption; (void)dacomrateoption; (void)dapriority;
}

void uninitmultiplayers(void) {}

void sendpacket(int32_t other, uint8_t *bufptr, int32_t messleng)
{
    (void)other; (void)bufptr; (void)messleng;
}

void setpackettimeout(int32_t datimeoutcount, int32_t daresendagaincount)
{
    (void)datimeoutcount; (void)daresendagaincount;
}

void sendlogon(void)   {}
void sendlogoff(void)  {}
void flushpackets(void) {}

int getoutputcirclesize(void) { return 0; }

void setsocket(short newsocket) { (void)newsocket; }


short getpacket(short *other, uint8_t *bufptr)
{
    (void)other; (void)bufptr;
    return 0;
}

void genericmultifunction(int32_t other, char *bufptr, int32_t messleng,
                          int32_t command)
{
    (void)other; (void)bufptr; (void)messleng; (void)command;
}

/* ------------------------------------------------------------------ music */

/* sounds.c calls this unprototyped; gnu89 assumes int-returning. */
int PlayMusic(char *filename)
{
    (void)filename;
    return 0;
}
