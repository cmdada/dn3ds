/*
 * platform_n3ds.c - 3DS start-up that must happen before main().
 *
 * A 3dsx launched from the Homebrew Launcher gets no useful cwd, no argv and
 * nowhere for printf to go, and game.c's main() assumes all three.
 *
 * A constructor rather than a patch to main(): devkitARM's crt0 mounts the SD
 * devoptab before __libc_init_array, so the filesystem is live by this point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include <3ds.h>

void setGameDir(char *gameDir);

#define DN3DS_GAMEDIR "sdmc:/3ds/duke3d"
#define DN3DS_LOG     DN3DS_GAMEDIR "/dn3ds.log"
#define DN3DS_ARGS    DN3DS_GAMEDIR "/args.txt"

#define DN3DS_MAXARGS 32

__attribute__((constructor))
static void n3ds_early_init(void)
{
    mkdir(DN3DS_GAMEDIR, 0777);

    /* dup2, not a second freopen: two FILE* over one path keep separate
       offsets and overwrite each other's lines. */
    freopen(DN3DS_LOG, "w", stdout);
    dup2(fileno(stdout), fileno(stderr));
    /* Line buffered so a hang or reset still leaves its last line on disk. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    chdir(DN3DS_GAMEDIR);
    setGameDir(DN3DS_GAMEDIR);

    printf("dn3ds: early init, gamedir=%s\n", DN3DS_GAMEDIR);
    printf("dn3ds: model=%s\n",
           osGetMemRegionFree(MEMREGION_APPLICATION) > (72 << 20)
               ? "New 3DS (extended memory)" : "Old 3DS / standard memory");
    printf("dn3ds: application memory free = %lu bytes\n",
           (unsigned long)osGetMemRegionFree(MEMREGION_APPLICATION));
    fflush(stdout);
}

/* Stand in for a command line: checkcommandline()'s switches, read from
   args.txt. Returns argc/argv untouched when the file is absent. */
static char  *dn3ds_argstore;
static char  *dn3ds_argv[DN3DS_MAXARGS];

int n3ds_build_args(int argc, char **argv, char ***out_argv)
{
    FILE *f;
    long  len;
    int   n = 0;
    char *p;

    *out_argv = argv;

    f = fopen(DN3DS_ARGS, "rb");
    if (!f)
        return argc;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) {
        fclose(f);
        return argc;
    }

    dn3ds_argstore = malloc((size_t)len + 1);
    if (!dn3ds_argstore) {
        fclose(f);
        return argc;
    }
    len = (long)fread(dn3ds_argstore, 1, (size_t)len, f);
    fclose(f);
    dn3ds_argstore[len] = '\0';

    /* argv[0] is skipped by the game, but keep the slot. */
    dn3ds_argv[n++] = (argc > 0 && argv && argv[0]) ? argv[0] : (char *)"dn3ds";

    for (p = dn3ds_argstore; *p && n < DN3DS_MAXARGS; ) {
        while (*p && isspace((unsigned char)*p))
            *p++ = '\0';
        if (!*p)
            break;
        dn3ds_argv[n++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
    }

    printf("dn3ds: args.txt supplied %d argument(s):", n - 1);
    {
        int i;
        for (i = 1; i < n; i++)
            printf(" %s", dn3ds_argv[i]);
    }
    printf("\n");
    fflush(stdout);

    *out_argv = dn3ds_argv;
    return n;
}
