#include "utils/init.h"
#include "utils/glutil.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <stdlib.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#ifndef NDK_PORT
#include "reimpl/controls.h"
#else
#include <falso_ndk/FalsoNDK.h>
#endif

int _newlib_heap_size_user = 256 * 1024 * 1024;

// Game logic runs on the render thread, and some of it recurses deeply (the
// ray/octree collision test in CTargetableComponent). The default ~256K stack
// overflows once you're actually playing, so give it room.
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// Swipe d'arme (D-pad G/D) : le WeaponSwitcher attend un glissement horizontal.
static int g_wswipe_active = 0;
static int g_wswipe_frame  = 0;
static int g_wswipe_dir    = 0;   // +1 droite, -1 gauche
static void weapon_swipe_tick(void);

#ifdef NMHD_DEV_SHORTCUTS
static volatile int dev_grab_frame = 0;
#endif


#ifndef NDK_PORT
// GLGame's touch entry points, JNI signature (env, obj, x, y, id). The front
// touchscreen feeds these directly; reimpl/controls.c has already scaled the
// coordinates to 960x544. The game was built around touch to begin with.
static void (*GLGame_nativeTouchPressed)(void*, void*, int, int, int)  = NULL;
static void (*GLGame_nativeTouchMoved)(void*, void*, int, int, int)    = NULL;
static void (*GLGame_nativeTouchReleased)(void*, void*, int, int, int) = NULL;

// Virtual pads (movement and aim) driven by the analog sticks. The game expects
// coordinates in its own 854x480 space, centred at (427,240), with a pad radius
// of about 90 (taken from GLGame.a()). nativeIsRightTouchPad(0/1) tells it which
// of the two pads is being used. Left stick is movement (id 1), right is aim (id 2).
static void (*GLGame_nativeTouchPressedPad)(void*, void*, int, int, int)  = NULL;
static void (*GLGame_nativeTouchMovedPad)(void*, void*, int, int, int)    = NULL;
static void (*GLGame_nativeTouchReleasedPad)(void*, void*, int, int, int) = NULL;
static void (*GLGame_nativeIsRightTouchPad)(void*, void*, int)            = NULL;
static void (*GLGame_nativeSetOnKeyDown)(void*, void*, int) = NULL;
static void (*GLGame_nativeSetOnKeyUp)(void*, void*, int)   = NULL;

#define PAD_CX 427
#define PAD_CY 240
#define PAD_R  90   // pad radius
#endif


int main() {
    soloader_init_all();

    int (*JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    JNI_OnLoad(&jvm);

    extern void java_init(void);
    java_init(); // fabrique les vrais jstring des champs Build.* (voir java.c)

    gl_init();

#ifndef NDK_PORT
    // Startup sequence, lifted from the Java side (GameRenderer.onSurfaceCreated).
    #define J(sym) so_symbol(&so_mod, "Java_com_gameloft_android_ANMP_Gloft9MHM_" sym)
    void (*GLResLoader_nativeInit)(void*, void*) = (void*)J("GLResLoader_nativeInit");
    void (*GLGame_nativeInit)(void*, void*) = (void*)J("GLGame_nativeInit");
    void (*GameRenderer_nativeInit)(void*, void*, int, int, int) = (void*)J("GameRenderer_nativeInit");
    void (*GameRenderer_nativeRender)(void*, void*) = (void*)J("GameRenderer_nativeRender");

    GLGame_nativeTouchPressed  = (void*)J("GLGame_nativeTouchPressed");
    GLGame_nativeTouchMoved    = (void*)J("GLGame_nativeTouchMoved");
    GLGame_nativeTouchReleased = (void*)J("GLGame_nativeTouchReleased");
    GLGame_nativeTouchPressedPad  = (void*)J("GLGame_nativeTouchPressedPad");
    GLGame_nativeTouchMovedPad    = (void*)J("GLGame_nativeTouchMovedPad");
    GLGame_nativeTouchReleasedPad = (void*)J("GLGame_nativeTouchReleasedPad");
    GLGame_nativeIsRightTouchPad  = (void*)J("GLGame_nativeIsRightTouchPad");
    GLGame_nativeSetOnKeyDown     = (void*)J("GLGame_nativeSetOnKeyDown");
    GLGame_nativeSetOnKeyUp       = (void*)J("GLGame_nativeSetOnKeyUp");

    GLResLoader_nativeInit(&jni, NULL);
    GLGame_nativeInit(&jni, NULL);
    // nativeInit(height, width, language). The Vita is 960x544 landscape, and
    // after the Android swap (width >= height), so height=960, width=544. Third
    // argument is the language (GLGame.i = getLanguage(...)), not the orientation.
    GameRenderer_nativeInit(&jni, NULL, 960, 544, 0);

    while (1) {
        controls_poll();
        weapon_swipe_tick();
        GameRenderer_nativeRender(&jni, NULL);
#ifdef NMHD_DEV_SHORTCUTS
        if (dev_grab_frame) {
            dev_grab_frame = 0;
            static unsigned char *buf = NULL;
            if (!buf) buf = malloc(960 * 544 * 4);
            if (buf) {
                glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                SceUID f = sceIoOpen(DATA_PATH "shot.raw",
                                     SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
                if (f >= 0) { sceIoWrite(f, buf, 960 * 544 * 4); sceIoClose(f); }
            }
        }
#endif
        gl_swap();
    }
#else
    // Build a fake ANativeActivity that the game's onCreate will receive
    ANativeActivity *activity = malloc(sizeof(ANativeActivity));
    activity->callbacks = malloc(sizeof(ANativeActivityCallbacks));
    activity->env = &jni; // from FalsoJNI
    activity->vm = &jvm;  // from FalsoJNI
    activity->clazz = (jclass)0x42424242;
    activity->internalDataPath = DATA_PATH "assets/";
    activity->externalDataPath = DATA_PATH "assets/";
    activity->sdkVersion = 14;
    activity->instance = NULL;

    // Drive the activity lifecycle
    int (*ANativeActivity_onCreate)(ANativeActivity *, void *, size_t) =
        (void *)so_symbol(&so_mod, "ANativeActivity_onCreate");
    ANativeActivity_onCreate(activity, NULL, 0);

    activity->callbacks->onStart(activity);
    activity->callbacks->onResume(activity);

    // Wire up input and the native window
    AInputQueue *aInputQueue = AInputQueue_create();
    activity->callbacks->onInputQueueCreated(activity, aInputQueue);

    ANativeWindow *aNativeWindow = ANativeWindow_create();
    activity->callbacks->onNativeWindowCreated(activity, aNativeWindow);

    activity->callbacks->onWindowFocusChanged(activity, 1);
#endif

    sceKernelExitDeleteThread(0);
}

#ifndef NDK_PORT
#ifdef NMHD_DEV_SHORTCUTS
// Ends the current level, same path the level scripts use. Handy for testing
// level transitions without replaying a whole level.
static void dev_skip_level(void) {
    void *(*get_level)(void) = (void *)so_symbol(&so_mod, "_ZN6CLevel8GetLevelEv");
    void (*end_level)(void *, int) = (void *)so_symbol(&so_mod,
        "_ZN6CLevel15RequestEndLevelEb");
    if (!get_level || !end_level) return;
    void *level = get_level();
    if (level) end_level(level, 1);
}
#endif

void controls_handler_key(int32_t keycode, ControlsAction action) {
    // Buttons are turned into synthetic touches on the HUD widgets, same trick as
    // the BackStab port.
    // R1 = tir (bouton milieu-droite), L1 = ralenti/bullet-time (haut-droite).
    // Screen coordinates are 960x544; each button gets its own touch id.
#ifdef NMHD_DEV_SHORTCUTS
    if (keycode == 109) {  // Select: dump the current frame
        if (action == CONTROLS_ACTION_DOWN) dev_grab_frame = 1;
        return;
    }
    if (keycode == 100) {  // Triangle
        if (action == CONTROLS_ACTION_DOWN) dev_skip_level();
        return;
    }
#endif
    // D-pad G/D -> changement d'arme : le WeaponSwitcher exige un vrai SWIPE
    // horizontal swipe, tapping the arrows does nothing. weapon_swipe_tick()
    // replays one over several frames from the render loop.
    if (keycode == 21 || keycode == 22) {
        if (action == CONTROLS_ACTION_DOWN && !g_wswipe_active) {
            g_wswipe_active = 1;
            g_wswipe_frame  = 0;
            g_wswipe_dir    = (keycode == 22) ? -1 : +1;  // right cycles by swiping left
        }
        return;
    }

    // Positions were read off a screenshot of the HUD.
    int bx, by, id;
    switch (keycode) {
        case 103: bx = 790; by = 420; id = 20; break;  // R1     -> tir
        case 102: bx = 880; by = 315; id = 21; break;  // L1     -> ralenti
        case 99:  bx = 870; by =  48; id = 22; break;  // Square: reload, taps the weapon icon
        case 96:  bx = 432; by = 508; id = 23; break;  // Cross: sprint, held
        case 108: bx =  30; by =  27; id = 26; break;  // Start  -> pause (bouton haut-gauche)
        default: return;
    }
    if (action == CONTROLS_ACTION_DOWN) {
        if (GLGame_nativeTouchPressed)  GLGame_nativeTouchPressed(&jni, NULL, bx, by, id);
    } else if (action == CONTROLS_ACTION_UP) {
        if (GLGame_nativeTouchReleased) GLGame_nativeTouchReleased(&jni, NULL, bx, by, id);
    }
}

// Replays a horizontal swipe across the weapon strip over several
// frames: press, a few moves, release.
static void weapon_swipe_tick(void) {
    if (!g_wswipe_active) return;
    const int Y      = 48;    // hauteur de la zone armes
    const int LEN    = 150;   // distance horizontale du glissement
    const int FRAMES = 6;
    const int ID     = 24;
    // start on the opposite side from the direction we're going
    int startx = (g_wswipe_dir > 0) ? 800 : 800 + LEN;
    int x = startx + g_wswipe_dir * (LEN * g_wswipe_frame / FRAMES);
    if (g_wswipe_frame == 0) {
        if (GLGame_nativeTouchPressed)  GLGame_nativeTouchPressed(&jni, NULL, x, Y, ID);
    } else if (g_wswipe_frame < FRAMES) {
        if (GLGame_nativeTouchMoved)    GLGame_nativeTouchMoved(&jni, NULL, x, Y, ID);
    } else {
        if (GLGame_nativeTouchReleased) GLGame_nativeTouchReleased(&jni, NULL, x, Y, ID);
        g_wswipe_active = 0;
    }
    g_wswipe_frame++;
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    int ix = (int)x, iy = (int)y;
    switch (action) {
        case CONTROLS_ACTION_DOWN:
            if (GLGame_nativeTouchPressed)  GLGame_nativeTouchPressed(&jni, NULL, ix, iy, id);
            break;
        case CONTROLS_ACTION_MOVE:
            if (GLGame_nativeTouchMoved)    GLGame_nativeTouchMoved(&jni, NULL, ix, iy, id);
            break;
        case CONTROLS_ACTION_UP:
            if (GLGame_nativeTouchReleased) GLGame_nativeTouchReleased(&jni, NULL, ix, iy, id);
            break;
    }
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    // poll_stick calls this every frame, including at rest, and its own
    // down/move/up can't be used: the constant MOVE(0,0) at rest reads as a
    // finger held down forever. So track press/move/release from whether the
    // stick is actually displaced.
    //
    // Movement is a floating touch joystick, the same way the BackStab port
    // handles this engine: press at the centre, then move to centre + stick *
    // radius each frame, release when it comes back to rest. Touch ids are kept
    // distinct from the real screen. Coordinates are in the game's own space and
    // Y is not inverted.
    static int pad_active[2] = { 0, 0 };
    static int last_x[2], last_y[2];
    int right = (which == CONTROLS_STICK_RIGHT);
    int idx   = right ? 1 : 0;
    int displaced = (x != 0.0f || y != 0.0f);

    if (!right) {
        // MOUVEMENT (stick gauche) = joystick tactile absolu (TouchScreen).
        // press at the base, then move out by stick * radius
        int id = 12;
        int ox = (int)(121.f + 77.f * x);
        int oy = (int)(425.f + 77.f * y);
        if (displaced) {
            if (!pad_active[idx]) {
                pad_active[idx] = 1;
                if (GLGame_nativeTouchPressed) GLGame_nativeTouchPressed(&jni, NULL, 121, 425, id);
            }
            if (GLGame_nativeTouchMoved) GLGame_nativeTouchMoved(&jni, NULL, ox, oy, id);
            last_x[idx] = ox; last_y[idx] = oy;
        } else if (pad_active[idx]) {
            pad_active[idx] = 0;
            if (GLGame_nativeTouchReleased) GLGame_nativeTouchReleased(&jni, NULL, last_x[idx], last_y[idx], id);
        }
    } else {
        // Aiming is a camera drag: the game turns by however far the finger
        // moved between two events, not by where it is. So keep one touch down
        // and keep moving it.
        static float ax, ay;
        static int aim_id = 13;
        const float SPEED = 14.f;      // pixels per frame at full deflection
        const int CX = 600, CY = 272;  // ancre au centre de la zone droite (960x544)
        if (displaced) {
            if (!pad_active[idx]) {
                pad_active[idx] = 1;
                aim_id = 13; ax = CX; ay = CY;
                if (GLGame_nativeTouchPressed) GLGame_nativeTouchPressed(&jni, NULL, CX, CY, aim_id);
            }
            // Let it run off screen instead of re-anchoring at the centre.
            // Only the delta matters, and re-anchoring showed up as a stutter
            // every time the touch was lifted.
            ax += x * SPEED;
            ay += y * SPEED;
            if (GLGame_nativeTouchMoved) GLGame_nativeTouchMoved(&jni, NULL, (int)ax, (int)ay, aim_id);
        } else if (pad_active[idx]) {
            pad_active[idx] = 0;
            if (GLGame_nativeTouchReleased) GLGame_nativeTouchReleased(&jni, NULL, (int)ax, (int)ay, aim_id);
        }
    }
}
#endif
