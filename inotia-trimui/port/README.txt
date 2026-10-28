Inotia 3: Children of Carnia -- TrimUI Brick / Smart Pro port
=============================================================

What this is
------------
The original Android game, run from its own native library (libgame.so,
32-bit ARMv5 Thumb) by a C interpreter compiled for the handheld's 64-bit
CPU. The game's GLES1 fixed-function drawing goes through gl4es to the
firmware's GLES2 driver; SDL2 owns the screen, the pad and the sound.
No game data is included.

Files you must supply (from your own, legally obtained APK, v1.3.2)
--------------------------------------------------------------------
    inotia3/libgame.so    lib/armeabi/libgame.so inside the APK
    inotia3/assets.apk    the APK itself, renamed

On a PC:  python3 tools/inotia_tool.py prepare <your.apk> <this inotia3 dir>

Controls (TrimUI Brick: no touchscreen, no sticks) -- see pad.txt
---------------------------------------------------------------
The game is touch-only. Every button is a finger on the on-screen HUD:

    D-pad            move (taps the on-screen d-pad)
    A (south)        attack / confirm
    B (east)         skill 1
    X (west)         skill 2
    Y (north)        item / shield
    L1 / R1 / L2     quick slots 1 / 2 / 3
    Start            pause menu
    SELECT (hold)    cursor mode: d-pad moves a pointer, A clicks it
    R2               click at the pointer
    SELECT + START   quit

Menus are touch menus: hold SELECT, move the pointer with the d-pad, press A.
On a Smart Pro the left stick moves the pointer, L3 clicks.

Settings
--------
    env.txt    SPIKE_LANG=ko-KR (text language), SPIKE_ROTATE, SDL_VIDEODRIVER,
               INOTIA_USE_BUNDLED_SDL=1, INOTIA_DEBUG=1
    pad.txt    button -> HUD position table
    mods.txt   game modifications (see docs/modding.md in the source tree)

Logs
----
    inotia3/inotia3.log (also copied next to the launcher). The first thing
    to read when nothing shows up. "GLIBC_x not found" = firmware older than
    this build; "no video backend" = SDL could not open the screen (try
    SDL_VIDEODRIVER=KMSDRM in env.txt, or INOTIA_USE_BUNDLED_SDL=1).

Credits
-------
    Inotia 3 is (c) Com2uS. Engine: suchangv/Inotia3-wasm. Guest lifecycle,
    JNI and patches: MetalSyntax/Inotia3-vita (MIT). gl4es: ptitSeb (MIT).
    SDL2 (zlib). Font: Nanum Gothic (SIL OFL 1.1, FONT-LICENSE.txt).
