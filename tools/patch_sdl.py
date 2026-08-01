#!/usr/bin/env python3
"""Build a stereo-capable libSDL for the 3DS out of vendor/sdl12-3ds.

The stock devkitPro SDL 1.2 driver only ever renders the top screen's left
eye: it owns one C3D_RenderTarget bound to (GFX_TOP, GFX_LEFT) and one
texture, and gfxSet3D() is nailed to false in N3DS_VideoInit.  There is no
SDL 1.2 API for a second eye, so the port cannot ask for one.

This script adds exactly one entry point to the driver:

    int SDL_N3DS_SetStereoBuffer(void *paletted_right);

Pass a paletted buffer with the same geometry as the display surface and the
next flip presents it to the right eye; pass NULL and the driver drops its
right-eye resources and goes back to mono.  Non-zero return means the driver
could not build the right eye, and the caller should stay mono.

The submodule checkout is never modified.  Its source is copied into
vendor/sdl12-3ds/build/, patched there, and built into
vendor/sdl12-3ds/lib/libSDL.a - both paths are gitignored, because the
patched library is a build artefact and the patch itself lives here.

Usage:
    tools/patch_sdl.py            # incremental; re-copies if the submodule moved
    tools/patch_sdl.py --clean    # throw the build tree away and start over
    tools/patch_sdl.py -j8        # override the make parallelism
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUBMODULE = REPO / "vendor" / "sdl12-3ds"
BUILD = SUBMODULE / "build"
TREE = BUILD / "sdl"
OBJ = BUILD / "obj"
LIBDIR = SUBMODULE / "lib"
STAMP = BUILD / "stamp"

DRIVER = "src/video/n3ds/SDL_n3dsvideo.c"

# Marker written into the patched driver so a re-run can tell it is done.
MARKER = "SDL_N3DS_SetStereoBuffer"


def die(msg):
    sys.exit(f"patch_sdl: {msg}")


def info(msg):
    print(f"patch_sdl: {msg}", flush=True)


# ---------------------------------------------------------------------------
# The patch
# ---------------------------------------------------------------------------

# Inserted just below the driver's forward declarations, which is above every
# function that touches it - N3DS_SetVideoMode tears the right eye down on a
# mode change and so needs these in scope too, and it comes early in the file.
STEREO_BLOCK = r"""
/* --------------------------------------------------------------------------
   Stereoscopic top screen, added for dn3ds by tools/patch_sdl.py.

   The left eye is untouched: the game keeps drawing into the paletted display
   surface and the driver keeps expanding it through the palette into
   hidden->buffer and on into spritesheet_tex.  The right eye is the same
   pipeline a second time - its own linear buffer, its own texture, its own
   render target bound to (GFX_TOP, GFX_RIGHT) - and it exists only while a
   right-eye buffer is actually being supplied.  At slider zero the caller
   passes NULL, everything here is released, gfxSet3D(false) drops the console
   back to mono, and the per-frame cost returns to exactly what it was.
   -------------------------------------------------------------------------- */

static bool              sceneScale;        /* the `scale` sceneInit() was given */
static SDL_VideoDevice  *stereo_this;
static C3D_RenderTarget *stereo_target;     /* (GFX_TOP, GFX_RIGHT) */
static C3D_Tex           stereo_tex;
static void             *stereo_buffer;     /* linear, expanded through the palette */
static Uint8            *stereo_src;        /* caller's paletted right eye, not owned */
static bool              stereo_tex_ready;
static bool              stereo_active;     /* resources live and gfxSet3D(true) */
static bool              stereo_frame;      /* this frame carries a right eye */
static bool              stereo_failed;     /* gave up; stay mono until the mode changes */

static void stereo_teardown(void)
{
	if (!stereo_active && !stereo_target && !stereo_tex_ready && !stereo_buffer)
		return;

	/* The video thread may still be drawing the frame that used these. */
	C3D_FrameSync();

	if (stereo_target) {
		C3D_RenderTargetDelete(stereo_target);
		stereo_target = NULL;
	}
	if (stereo_tex_ready) {
		C3D_TexDelete(&stereo_tex);
		stereo_tex_ready = false;
	}
	if (stereo_buffer) {
		linearFree(stereo_buffer);
		stereo_buffer = NULL;
	}
	if (stereo_active) {
		gfxSet3D(false);
		stereo_active = false;
	}
	stereo_frame = false;
	stereo_src = NULL;
}

static int stereo_setup(_THIS)
{
	int hw = this->hidden->w;
	int hh = this->hidden->h;
	int mode = this->hidden->mode;
	size_t bytes = (size_t)hw * hh * this->hidden->byteperpixel;

	if (stereo_active)
		return 0;

	stereo_buffer = linearAlloc(bytes);
	if (!stereo_buffer)
		goto fail;
	SDL_memset(stereo_buffer, 0, bytes);

	/* Same dimensions and format as spritesheet_tex, so the same source
	   window and the same quad work for both eyes. */
	if (!C3D_TexInit(&stereo_tex, hw, hh, mode))
		goto fail;
	stereo_tex_ready = true;
	C3D_TexSetFilter(&stereo_tex, this->hidden->fitscreen ? GPU_LINEAR : GPU_NEAREST, GPU_NEAREST);

	if (sceneScale)
		stereo_target = C3D_RenderTargetCreate(240*2, 400*2, mode, GPU_RB_DEPTH24_STENCIL8);
	else
		stereo_target = C3D_RenderTargetCreate(240, 400, mode, GPU_RB_DEPTH24_STENCIL8);
	if (!stereo_target)
		goto fail;

	C3D_RenderTargetSetOutput(stereo_target, GFX_TOP, GFX_RIGHT,
		displayTranferFlags[mode] | GX_TRANSFER_SCALING(sceneScale ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));

	gfxSet3D(true);
	stereo_active = true;
	return 0;

fail:
	stereo_teardown();
	return -1;
}

/* Expand the caller's paletted right eye into stereo_buffer, using the same
   palette the left eye was just expanded with.  Called from the flip path,
   immediately before drawBuffers() hands the frame to the video thread. */
static void stereo_expand(_THIS)
{
	Uint32 *palette, *dst_baseaddr, *dst_addr;
	Uint8 *src_addr;
	int x, y;

	stereo_frame = false;

	if (!stereo_active || !stereo_src || !stereo_buffer)
		return;
	if (this->hidden->bpp != 8)
		return;

	palette = this->hidden->palette;
	src_addr = stereo_src;
	dst_baseaddr = stereo_buffer;

	for (y = 0; y < this->info.current_h; y++) {
		dst_addr = dst_baseaddr + y * this->hidden->w;
		for (x = 0; x < this->info.current_w; x++) {
			*dst_addr++ = palette[*src_addr++];
		}
	}

	stereo_frame = true;
}

/* Public entry point.  paletted_right must have the same geometry as the
   display surface (surface->pitch * surface->h), and stays owned by the
   caller; it is read during the next flip and not retained beyond it.
   NULL returns the driver to mono.  Returns 0 on success, -1 if the right
   eye is unavailable. */
int SDL_N3DS_SetStereoBuffer(void *paletted_right)
{
	SDL_VideoDevice *this = stereo_this;

	if (!this || !this->hidden)
		return -1;

	if (!paletted_right) {
		stereo_src = NULL;
		if (stereo_active)
			stereo_teardown();
		return 0;
	}

	/* Only the 8-bit paletted path is expanded here; anything else would
	   need a straight blit and no caller wants one. */
	if (this->hidden->bpp != 8 || !this->hidden->palettedbuffer)
		return -1;
	if (stereo_failed)
		return -1;
	if (!stereo_active && stereo_setup(this) < 0) {
		stereo_failed = true;
		return -1;
	}

	stereo_src = paletted_right;
	return 0;
}
"""

# The video thread binds a texture per target now, because the two eyes are
# two different textures inside one frame.
STEREO_RIGHT_DRAW = r"""if (stereo_frame && stereo_target) {
	C3D_TexBind(0, &stereo_tex);
	C3D_RenderTargetClear(stereo_target, C3D_CLEAR_ALL, RenderClearColor, 0);
	C3D_FrameDrawOn(stereo_target);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	drawTexture((400-this->hidden->w1*this->hidden->scalex)/2,(240-this->hidden->h1*this->hidden->scaley)/2, this->hidden->w1*this->hidden->scalex, this->hidden->h1*this->hidden->scaley, this->hidden->l1, this->hidden->r1, this->hidden->t1, this->hidden->b1);
}"""

# The right eye needs the same flush/transfer/flush the left one gets.
STEREO_TRANSFER = r"""if (stereo_frame && stereo_buffer && stereo_tex_ready) {
	GSPGPU_FlushDataCache(stereo_buffer, this->hidden->w*this->hidden->h*this->hidden->byteperpixel);
	C3D_SyncDisplayTransfer ((u32*)stereo_buffer, GX_BUFFER_DIM(this->hidden->w, this->hidden->h), (u32*)stereo_tex.data, GX_BUFFER_DIM(this->hidden->w, this->hidden->h), textureTranferFlags[this->hidden->mode]);
	GSPGPU_FlushDataCache(stereo_tex.data, this->hidden->w*this->hidden->h*this->hidden->byteperpixel);
}"""


def indent_of(line):
    return line[: len(line) - len(line.lstrip())]


def insert_after(text, needle, block, what):
    """Insert block on the line after the (unique) line containing needle,
    re-indented to match that line."""
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if needle in l]
    if len(hits) != 1:
        die(f"expected 1 anchor for {what!r}, found {len(hits)}")
    i = hits[0]
    pad = indent_of(lines[i])
    new = [pad + l if l.strip() else l for l in block.split("\n")]
    lines[i + 1 : i + 1] = new
    return "\n".join(lines)


def insert_before_all(text, needle, block, what, expect):
    """Insert block before every line containing needle, matching indentation."""
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if needle in l]
    if len(hits) != expect:
        die(f"expected {expect} anchors for {what!r}, found {len(hits)}")
    for i in reversed(hits):
        pad = indent_of(lines[i])
        new = [pad + l if l.strip() else l for l in block.split("\n")]
        lines[i:i] = new
    return "\n".join(lines)


def replace_once(text, old, new, what):
    if text.count(old) != 1:
        die(f"expected 1 occurrence for {what!r}, found {text.count(old)}")
    return text.replace(old, new, 1)


def patch_driver(path):
    src = path.read_text()
    if MARKER in src:
        info("driver already patched")
        return False

    # 1. State, setup/teardown, palette expansion and the public entry point.
    src = replace_once(
        src,
        "int N3DS_ToggleFullScreen(_THIS, int on);\n",
        "int N3DS_ToggleFullScreen(_THIS, int on);\n" + STEREO_BLOCK,
        "forward declarations",
    )

    # 2. Remember what sceneInit() decided about supersampling, so the right
    #    eye's target matches the left one exactly.
    src = insert_after(
        src, "RenderClearColor = clearcolors[mode];", "sceneScale = scale;", "sceneInit scale"
    )

    # 3. Each target now binds its own texture; the single bind that used to
    #    happen in drawBuffers cannot serve two different eyes in one frame.
    src = replace_once(
        src,
        "C3D_RenderTargetClear(VideoSurface1, C3D_CLEAR_ALL, RenderClearColor, 0);",
        "C3D_TexBind(0, &spritesheet_tex);\n\t\t\t\t\t"
        "C3D_RenderTargetClear(VideoSurface1, C3D_CLEAR_ALL, RenderClearColor, 0);",
        "top screen clear",
    )
    src = replace_once(
        src,
        "C3D_RenderTargetClear(VideoSurface2, C3D_CLEAR_ALL, RenderClearColor, 0);",
        "C3D_TexBind(0, &spritesheet_tex);\n\t\t\t\t\t"
        "C3D_RenderTargetClear(VideoSurface2, C3D_CLEAR_ALL, RenderClearColor, 0);",
        "bottom screen clear",
    )

    # 4. Draw the right eye straight after the left one, inside the same frame.
    src = insert_after(
        src,
        "this->hidden->l1, this->hidden->r1, this->hidden->t1, this->hidden->b1);",
        STEREO_RIGHT_DRAW,
        "top screen drawTexture",
    )

    # 5. Transfer the right eye's buffer into its texture alongside the left.
    src = insert_after(
        src,
        "GSPGPU_FlushDataCache(spritesheet_tex.data,",
        STEREO_TRANSFER,
        "spritesheet flush",
    )

    # 6. Expand the right eye just before each flip path hands off the frame.
    src = insert_before_all(
        src, "drawBuffers(this);", "stereo_expand(this);", "drawBuffers call", 2
    )

    # 7. Release the right eye before the scene is torn down, in both the
    #    mode-change path and on quit.
    src = replace_once(
        src,
        "\tif(setVideoModecount>1) sceneExit();",
        "\tstereo_teardown();\n\tif(setVideoModecount>1) sceneExit();",
        "SetVideoMode sceneExit",
    )
    src = replace_once(
        src,
        "\n\tsceneExit();\n",
        "\n\tstereo_teardown();\n\n\tsceneExit();\n",
        "VideoQuit sceneExit",
    )

    # 8. Latch the device and clear any previous give-up state.
    src = replace_once(
        src,
        "\tthis->hidden->currentVideoSurface = current;",
        "\tthis->hidden->currentVideoSurface = current;\n"
        "\tstereo_this = this;\n"
        "\tstereo_failed = false;",
        "currentVideoSurface assignment",
    )

    path.write_text(src)
    info("driver patched")
    return True


# ---------------------------------------------------------------------------
# Tree management
# ---------------------------------------------------------------------------


def patch_identity():
    """Hash of this script's patch inputs, so a changed patch forces a re-copy."""
    h = hashlib.sha256()
    for part in (STEREO_BLOCK, STEREO_RIGHT_DRAW, STEREO_TRANSFER):
        h.update(part.encode())
    h.update(Path(__file__).read_bytes())
    return h.hexdigest()[:16]


def submodule_head():
    try:
        out = subprocess.run(
            ["git", "-C", str(SUBMODULE), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def want_stamp():
    return f"{submodule_head()} {patch_identity()}\n"


def copy_tree():
    info(f"copying submodule source into {TREE.relative_to(REPO)}")
    if TREE.exists():
        shutil.rmtree(TREE)
    shutil.copytree(
        SUBMODULE,
        TREE,
        ignore=shutil.ignore_patterns(".git", "build", "lib"),
        symlinks=True,
    )


def run(cmd, cwd, env, what):
    info(f"{what} ...")
    proc = subprocess.run(cmd, cwd=str(cwd), env=env)
    if proc.returncode != 0:
        die(f"{what} failed (exit {proc.returncode})")


def build_env():
    dkp = os.environ.get("DEVKITPRO")
    dka = os.environ.get("DEVKITARM")
    if not dkp:
        die("DEVKITPRO is not set")
    if not dka:
        die("DEVKITARM is not set")

    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(
        [f"{dka}/bin", f"{dkp}/tools/bin", env.get("PATH", "")]
    )
    # SDL's configure runs the 3DS compiler for its feature tests; without a
    # freestanding-friendly flag set the link tests all fail and configure
    # concludes the compiler is broken.
    arch = "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mword-relocations"
    env["CC"] = "arm-none-eabi-gcc"
    env["CXX"] = "arm-none-eabi-g++"
    env["AR"] = "arm-none-eabi-ar"
    env["RANLIB"] = "arm-none-eabi-ranlib"
    env["STRIP"] = "arm-none-eabi-strip"
    env["CFLAGS"] = f"-g -O2 {arch} -ffunction-sections -fdata-sections -D__3DS__ -I{dkp}/libctru/include"
    env["CPPFLAGS"] = f"-D__3DS__ -I{dkp}/libctru/include"
    env["LDFLAGS"] = f"-specs=3dsx.specs {arch} -L{dkp}/libctru/lib"
    env["LIBS"] = "-lcitro3d -lctru"
    return env


def configure_and_make(env, jobs):
    if not (TREE / "configure").exists():
        run(["sh", "./autogen.sh"], TREE, env, "autogen")

    OBJ.mkdir(parents=True, exist_ok=True)
    if not (OBJ / "Makefile").exists():
        run(
            [
                str(TREE / "configure"),
                "--host=arm-none-eabi",
                "--prefix=" + str(BUILD / "prefix"),
                "--enable-n3ds",
                "--enable-static",
                "--disable-shared",
                "--disable-sdltest",
                "--disable-video-x11",
                "--disable-video-dga",
                "--disable-video-opengl",
                "--disable-pulseaudio",
                "--disable-esd",
                "--disable-arts",
                "--disable-nas",
                "--disable-alsa",
                "--disable-oss",
                "--disable-diskaudio",
                "--disable-dummyaudio",
                "--disable-cdrom",
                "--disable-loadso",
                "--disable-assembly",
            ],
            OBJ,
            env,
            "configure",
        )

    run(["make", f"-j{jobs}"], OBJ, env, "make")


def install_lib():
    lib = OBJ / "build" / ".libs" / "libSDL.a"
    if not lib.exists():
        alt = list(OBJ.rglob("libSDL.a"))
        if not alt:
            die("build finished but no libSDL.a was produced")
        lib = alt[0]

    LIBDIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(lib, LIBDIR / "libSDL.a")

    # SDL's own main shim, if this build produced one, so -lSDLmain keeps
    # resolving against the same tree rather than the stock portlib.
    for extra in ("libSDLmain.a",):
        found = list(OBJ.rglob(extra))
        if found:
            shutil.copy2(found[0], LIBDIR / extra)

    info(f"installed {(LIBDIR / 'libSDL.a').relative_to(REPO)}")


def verify(env):
    """The whole point of the exercise: the symbol has to be in there."""
    out = subprocess.run(
        ["arm-none-eabi-nm", str(LIBDIR / "libSDL.a")],
        capture_output=True,
        text=True,
        env=env,
    )
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "T" and parts[2] == MARKER:
            info(f"verified: {MARKER} is defined in libSDL.a")
            return
    die(f"{MARKER} is missing from the built libSDL.a")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", action="store_true", help="discard the build tree first")
    ap.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 2,
        help="make parallelism (default: number of cores)",
    )
    args = ap.parse_args()

    if not (SUBMODULE / "configure.in").exists():
        die(
            "vendor/sdl12-3ds is empty - run `git submodule update --init` first"
        )

    env = build_env()

    if args.clean and BUILD.exists():
        info("removing the build tree")
        shutil.rmtree(BUILD)

    BUILD.mkdir(parents=True, exist_ok=True)

    stamp = want_stamp()
    stale = not STAMP.exists() or STAMP.read_text() != stamp
    if stale:
        if TREE.exists():
            info("submodule or patch changed; rebuilding from scratch")
        for path in (TREE, OBJ):
            if path.exists():
                shutil.rmtree(path)
        copy_tree()
        patch_driver(TREE / DRIVER)
        STAMP.write_text(stamp)
    else:
        info("build tree is up to date")
        patch_driver(TREE / DRIVER)

    configure_and_make(env, args.jobs)
    install_lib()
    verify(env)
    info("done - `make` will now link against the patched libSDL")


if __name__ == "__main__":
    main()
