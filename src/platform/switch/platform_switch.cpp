/* The game's platform layer (platform.h) on the Nintendo Switch: libnx for the display (EGL on the default window,
 * platform_switch.h), the controllers, the USB keyboard, the clocks and the system's language; SDL2 for the log (the
 * audio, the .wav files and the threads use it too: platform_audio_switch.cpp, wav_switch.cpp,
 * win32_switch_compat.cpp).
 *
 * The game lives in /switch/sheepdogwolf/ (the folder of the .nro, when the Homebrew Menu says where it is): the .nro,
 * the game's own data (Levels/, Musics/, Voices/, References/, Bonus/ from your copy), environment.txt (the settings,
 * platform_switch.h), saves/, and log.txt (log-previous.txt: the run before). */
#include <arpa/inet.h> /* struct in_addr, for nxlink.h's __nxlink_host */
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL2/SDL.h>
#include <png.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../platform.h"
#include "platform_switch.h"

/* src/render/perf_overlay.h, which cannot be included here: its sdw_types.h spells u64 and s64 as long long, libnx as
 * long, so the game's headers and switch.h stay in separate files */
void PerfOverlay_Toggle();

int Game_Main(int argc, char **argv); /* src/app/app_main.cpp */

#define SWITCH_DEFAULT_DIR "sdmc:/switch/sheepdogwolf"
#define SWITCH_MAX_ARGS 32

static char s_baseDir[512]; /* the game's folder, with no '/' at the end */
static FILE *s_logFile;
static int s_quit;
static unsigned s_pumpedEvents;
static PadState s_pad;
static u64 s_padHeld;
static HidKeyboardState s_keyboard;
static int s_keyboardF3;
static int s_swapAB;

/* ---- the log: log.txt in the game's folder, the run before kept as log-previous.txt ---- */
static void Switch_WriteLog(const char *text)
{
    if (s_logFile) {
        fprintf(s_logFile, "%s\n", text);
        fflush(s_logFile);
    }
    printf("%s\n", text); /* nxlink's terminal, when it is connected */
}

static void SDLCALL Switch_SdlLog(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;
    Switch_WriteLog(message);
}

static void Switch_OpenLog()
{
    char path[640], previous[640];
    snprintf(path, sizeof(path), "%s/log.txt", s_baseDir);
    snprintf(previous, sizeof(previous), "%s/log-previous.txt", s_baseDir);
    remove(previous);
    rename(path, previous);
    s_logFile = fopen(path, "w");
    SDL_LogSetOutputFunction(Switch_SdlLog, 0);
}

void Platform_Log(const char *fmt, ...)
{
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Switch_WriteLog(text);
}

/* ---- the settings: environment.txt ---- */
static void Switch_LoadEnvironment()
{
    char path[640], line[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/environment.txt", s_baseDir);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        char *name = line, *value, *end;
        while (*name == ' ' || *name == '\t')
            name++;
        if (*name == '#' || !(value = strchr(name, '=')))
            continue;
        *value++ = 0;
        for (end = value + strlen(value); end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ');)
            *--end = 0;
        for (end = name + strlen(name); end > name && (end[-1] == ' ' || end[-1] == '\t');)
            *--end = 0;
        if (*name)
            setenv(name, value, 1);
    }
    fclose(f);
}

static int Switch_EnvInt(const char *name, int fallback)
{
    const char *value = getenv(name);
    return value && *value ? atoi(value) : fallback;
}

/* ---- the clocks: raised while the game runs, put back when it ends; a clock already higher is left alone ---- */
struct SwitchClock {
    int module; /* PcvModuleId (clkrst) */
    int legacy; /* PcvModule (pcv, before 8.0.0) */
    u32 previous;
    int changed;
};
static SwitchClock s_cpuClock = {PcvModuleId_CpuBus, PcvModule_CpuBus, 0, 0};
static SwitchClock s_gpuClock = {PcvModuleId_GPU, PcvModule_GPU, 0, 0};
static int s_clockService; /* 1: clkrst, 2: pcv */

static void Switch_SetClock(SwitchClock *clock, int mhz, const char *what)
{
    u32 hz = (u32)mhz * 1000000u, now = 0;
    if (mhz <= 0)
        return;
    if (!s_clockService) {
        if (hosversionAtLeast(8, 0, 0))
            s_clockService = R_SUCCEEDED(clkrstInitialize()) ? 1 : -1;
        else
            s_clockService = R_SUCCEEDED(pcvInitialize()) ? 2 : -1;
    }
    if (s_clockService == 1) {
        ClkrstSession session;
        if (R_FAILED(clkrstOpenSession(&session, (PcvModuleId)clock->module, 3)))
            return;
        clkrstGetClockRate(&session, &now);
        if (now < hz && R_SUCCEEDED(clkrstSetClockRate(&session, hz))) {
            clock->previous = now;
            clock->changed = 1;
        }
        clkrstCloseSession(&session);
    } else if (s_clockService == 2) {
        pcvGetClockRate((PcvModule)clock->legacy, &now);
        if (now < hz && R_SUCCEEDED(pcvSetClockRate((PcvModule)clock->legacy, hz))) {
            clock->previous = now;
            clock->changed = 1;
        }
    }
    Platform_Log("SheepD3D: %s clock %u MHz%s", what, (unsigned)(clock->changed ? hz : now) / 1000000u,
                 clock->changed ? " (raised)" : " (left as it was)");
}

static void Switch_RestoreClock(SwitchClock *clock)
{
    if (!clock->changed)
        return;
    if (s_clockService == 1) {
        ClkrstSession session;
        if (R_SUCCEEDED(clkrstOpenSession(&session, (PcvModuleId)clock->module, 3))) {
            clkrstSetClockRate(&session, clock->previous);
            clkrstCloseSession(&session);
        }
    } else if (s_clockService == 2) {
        pcvSetClockRate((PcvModule)clock->legacy, clock->previous);
    }
    clock->changed = 0;
}

static void Switch_RestoreClocks()
{
    Switch_RestoreClock(&s_cpuClock);
    Switch_RestoreClock(&s_gpuClock);
    if (s_clockService == 1)
        clkrstExit();
    else if (s_clockService == 2)
        pcvExit();
    s_clockService = 0;
}

/* ---- the watchdog: a heartbeat in the log, and where the main thread is when frames stop ----
 *
 * Every 5 s the log gets the frames shown and the loop's iterations. When no frame has been shown for 3 s, the main
 * thread is paused for a moment and its program counter and call chain (the frame pointers) are logged as addresses in
 * the .elf: `aarch64-none-elf-addr2line -f -C -e build-switch/SheepD3D.elf <address>` names them. */
/* The image's base: the start of the memory region holding this code (the .text segment comes first, at the .elf's
 * address 0). switch.ld's __start__ cannot say it: it is an absolute 0, which the loader does not relocate. */
static u64 Switch_ImageBase()
{
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, (u64)(uintptr_t)&Switch_ImageBase)))
        return 0;
    return info.addr;
}

static volatile u32 s_framesShown, s_loopPolls;
static volatile int s_watchdogRun;
static Thread s_watchdog;

static void Switch_LogMainThread()
{
    Handle main = envGetMainThreadHandle();
    ThreadContext context;
    u64 chain[16], base = Switch_ImageBase();
    int n = 0, i;
    Result rc;
    if (R_FAILED(rc = svcSetThreadActivity(main, ThreadActivity_Paused))) {
        Platform_Log("SheepD3D: watchdog: cannot pause the main thread (0x%x)", rc);
        return;
    }
    if (R_SUCCEEDED(rc = svcGetThreadContext3(&context, main))) {
        /* x29 chain: [fp] the caller's fp, [fp + 8] the return address; the stack grows down */
        u64 fp = context.fp;
        while (n < 16 && fp && !(fp & 15) && fp >= context.sp && fp - context.sp < 0x800000) {
            const u64 *frame = (const u64 *)fp;
            chain[n++] = frame[1];
            if (frame[0] <= fp)
                break;
            fp = frame[0];
        }
    }
    svcSetThreadActivity(main, ThreadActivity_Runnable);
    if (R_FAILED(rc)) {
        Platform_Log("SheepD3D: watchdog: no context (0x%x)", rc);
        return;
    }
    Platform_Log("SheepD3D: watchdog: main thread at pc 0x%lx, lr 0x%lx (.elf addresses)",
                 (unsigned long)(context.pc.x - base), (unsigned long)(context.lr - base));
    for (i = 0; i < n; i++)
        Platform_Log("SheepD3D: watchdog:   called from 0x%lx", (unsigned long)(chain[i] - base));
}

static void Switch_WatchdogMain(void *arg)
{
    u32 lastFrames = s_framesShown, beatFrames = s_framesShown, beatPolls = s_loopPolls;
    int stalled = 0, seconds = 0, samples = 0;
    (void)arg;
    while (s_watchdogRun) {
        svcSleepThread(1000000000ull);
        seconds++;
        if (s_framesShown == lastFrames) {
            stalled++;
            if ((stalled == 3 || (stalled > 3 && stalled % 5 == 0)) && samples < 6) {
                Platform_Log("SheepD3D: watchdog: no frame shown for %d s", stalled);
                Switch_LogMainThread();
                samples++;
            }
        } else {
            stalled = 0;
        }
        lastFrames = s_framesShown;
        if (seconds % 5 == 0) {
            Platform_Log("SheepD3D: heartbeat: %u frames, %u loop iterations in 5 s", s_framesShown - beatFrames,
                         s_loopPolls - beatPolls);
            beatFrames = s_framesShown;
            beatPolls = s_loopPolls;
        }
    }
}

static void Switch_StartWatchdog()
{
    s_watchdogRun = 1;
    /* one priority above the main thread's (0x2c), so that waking up it preempts a main thread that spins */
    if (R_FAILED(threadCreate(&s_watchdog, Switch_WatchdogMain, 0, 0, 0x8000, 0x2b, -2)) ||
        R_FAILED(threadStart(&s_watchdog))) {
        s_watchdogRun = 0;
        Platform_Log("SheepD3D: watchdog: not started");
    }
}

static void Switch_StopWatchdog()
{
    if (!s_watchdogRun)
        return;
    s_watchdogRun = 0;
    threadWaitForExit(&s_watchdog);
    threadClose(&s_watchdog);
}

/* ---- the entry point ---- */

/* the folder of the .nro (argv[0], from the Homebrew Menu), else the default one */
static void Switch_FindBaseDir(int argc, char **argv)
{
    char *slash;
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0) {
        snprintf(s_baseDir, sizeof(s_baseDir), "%s", argv[0]);
        if ((slash = strrchr(s_baseDir, '/')) != 0 && slash > s_baseDir + 5) {
            *slash = 0;
            return;
        }
    }
    snprintf(s_baseDir, sizeof(s_baseDir), "%s", SWITCH_DEFAULT_DIR);
}

int main(int argc, char **argv)
{
    static char extra[1024];
    char *args[SWITCH_MAX_ARGS];
    char saves[640];
    int count = 0, i, result, nxlink = 0;

    Switch_FindBaseDir(argc, argv);
    chdir(s_baseDir);
    Switch_LoadEnvironment();
    /* the log to nxlink's terminal: when nxlink started the game (it sets __nxlink_host), or SDW_NXLINK=1 */
    if ((__nxlink_host.s_addr || Switch_EnvInt("SDW_NXLINK", 0)) && R_SUCCEEDED(socketInitializeDefault()))
        nxlink = nxlinkStdio() >= 0;
    Switch_OpenLog();
    Platform_Log("SheepD3D: Nintendo Switch, folder %s", s_baseDir);
    if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
        Platform_Log("SheepD3D: running as an applet (from the album): memory is short. Start the Homebrew Menu by "
                     "holding R while starting a game for the full memory.");

    /* the saves beside the game (save_store.h reads $SDW_SAVE_DIR) */
    snprintf(saves, sizeof(saves), "%s/saves", s_baseDir);
    setenv("SDW_SAVE_DIR", saves, 0);

    Switch_SetClock(&s_cpuClock, Switch_EnvInt("SDW_CPU_BOOST", 0), "CPU");
    Switch_SetClock(&s_gpuClock, Switch_EnvInt("SDW_GPU_BOOST", 0), "GPU");
    s_swapAB = Switch_EnvInt("SDW_SWAP_AB", 0);
    if (Switch_EnvInt("SDW_PERF_OVERLAY", 0))
        PerfOverlay_Toggle();

    /* the command line: the controller (there is no keyboard to fall back on), then SDW_ARGS, then the real one */
    args[count++] = argc > 0 ? argv[0] : (char *)"SheepD3D";
    args[count++] = (char *)"--controller=0";
    if (getenv("SDW_ARGS")) {
        char *p;
        snprintf(extra, sizeof(extra), "%s", getenv("SDW_ARGS"));
        for (p = strtok(extra, " \t"); p && count < SWITCH_MAX_ARGS - 1; p = strtok(0, " \t"))
            args[count++] = p;
    }
    for (i = 1; i < argc && count < SWITCH_MAX_ARGS - 1; i++)
        args[count++] = argv[i];
    args[count] = 0;

    Switch_StartWatchdog();
    result = Game_Main(count, args);
    Switch_StopWatchdog();

    Switch_RestoreClocks();
    Platform_Log("SheepD3D: exit %d", result);
    if (s_logFile)
        fclose(s_logFile);
    s_logFile = 0;
    if (nxlink)
        socketExit();
    return result;
}

/* ---- the window: the console's default one ---- */
int Platform_Init(const char *className, int iconResource)
{
    (void)className;
    (void)iconResource;
    /* the stream loader's thread (CreateThread, win32_switch_compat.cpp) runs the game's code, with its path buffers
     * on the stack: Windows' 1 MB, not SDL's small default */
    SDL_SetHint(SDL_HINT_THREAD_STACK_SIZE, "1048576");
    if (SDL_Init(0) < 0) {
        Platform_Log("SheepD3D: SDL_Init: %s", SDL_GetError());
        return 0;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&s_pad); /* the handheld controls and player 1 */
    hidInitializeKeyboard();
    return 1;
}

void *Platform_CreateWindow(const char *title, int width, int height, unsigned flags)
{
    (void)title;
    (void)width;
    (void)height;
    (void)flags;
    return nwindowGetDefault();
}

SDL_Window *Platform_Window()
{
    return 0; /* no SDL window: the renderer draws with EGL on the default one (platform_switch.h) */
}

void Platform_ShowWindow() {}

void Platform_PumpEvents()
{
    s_pumpedEvents |= Platform_PollEvents();
}

unsigned Platform_PollEvents()
{
    unsigned result = s_pumpedEvents;
    int f3;
    s_pumpedEvents = 0;
    s_loopPolls++;
    /* the system's messages: the HOME menu's "close", sleep, docking (appletGetOperationMode) */
    if (!appletMainLoop() || s_quit)
        result |= PLATFORM_EVENT_QUIT;
    padUpdate(&s_pad);
    s_padHeld = padGetButtons(&s_pad);
    hidGetKeyboardStates(&s_keyboard, 1);
    f3 = hidKeyboardStateGetKey(&s_keyboard, HidKeyboardKey_F3);
    if (f3 && !s_keyboardF3)
        PerfOverlay_Toggle(); /* the performance overlay (src/render/perf_overlay.h) */
    s_keyboardF3 = f3;
    return result;
}

int Platform_IsActive()
{
    return appletGetFocusState() == AppletFocusState_InFocus;
}

void Platform_RequestQuit()
{
    s_quit = 1;
}

void Platform_Shutdown()
{
    Switch_ShutdownGL();
    SDL_Quit();
}

int Platform_GetBasePath(char *out, int capacity)
{
    if (capacity <= 0)
        return 0;
    return snprintf(out, (size_t)capacity, "%s/", s_baseDir) < capacity;
}

int Platform_EnterBaseDir()
{
    return chdir(s_baseDir) == 0;
}

extern "C" void Sdw_ResolvePath(const char *path, char *out, size_t size); /* crt_posix_compat.cpp */

int Platform_ResolvePath(const char *path, char *out, int capacity)
{
    if (!path || !out || capacity <= 0)
        return 0;
    Sdw_ResolvePath(path, out, (size_t)capacity);
    return strlen(out) + 1 < (size_t)capacity;
}

/* PNGs with libpng's simplified API (SDL2 has no PNG loader) */
unsigned char *Platform_LoadImageRGBA(const char *path, int *width, int *height)
{
    char resolved[1024];
    png_image image;
    unsigned char *pixels;
    if (!Platform_ResolvePath(path, resolved, sizeof(resolved)) || access(resolved, F_OK) != 0)
        return 0; /* most often: no such file, the normal case for a page without an override */
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, resolved)) {
        Platform_Log("SheepD3D: %s: %s", resolved, image.message);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    if (!(pixels = (unsigned char *)malloc(PNG_IMAGE_SIZE(image)))) {
        png_image_free(&image);
        return 0;
    }
    if (!png_image_finish_read(&image, 0, pixels, 0, 0)) {
        Platform_Log("SheepD3D: %s: %s", resolved, image.message);
        free(pixels);
        return 0;
    }
    *width = (int)image.width;
    *height = (int)image.height;
    return pixels;
}

void Platform_FreeImage(unsigned char *pixels)
{
    free(pixels);
}

/* the log; an error also in the system's error dialog */
void Platform_ShowMessage(const char *title, const char *message, int isError)
{
    Platform_Log("%s: %s", title ? title : "SheepD3D", message ? message : "");
    if (isError) {
        ErrorApplicationConfig config;
        char details[1024];
        snprintf(details, sizeof(details), "%s\n\nThe game's folder: %s\nSee log.txt there.", message ? message : "",
                 s_baseDir);
        if (R_SUCCEEDED(errorApplicationCreate(&config, title ? title : "SheepD3D", details)))
            errorApplicationShow(&config);
    }
}

unsigned Platform_LanguageId()
{
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    unsigned result = 9;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)))
            setMakeLanguage(code, &language);
        setExit();
    }
    switch (language) {
        case SetLanguage_FR:
        case SetLanguage_FRCA:
            result = 0x0c;
            break;
        case SetLanguage_DE:
            result = 0x07;
            break;
        case SetLanguage_NL:
            result = 0x13;
            break;
        case SetLanguage_ES:
        case SetLanguage_ES419:
            result = 0x0a;
            break;
        case SetLanguage_IT:
            result = 0x10;
            break;
        case SetLanguage_PT:
        case SetLanguage_PTBR:
            result = 0x16;
            break;
        default:
            break;
    }
    return result;
}

/* ---- the display: EGL on the default window ---- */
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLContext s_context = EGL_NO_CONTEXT;

int Switch_IsDocked()
{
    return appletGetOperationMode() == AppletOperationMode_Console;
}

int Switch_InitGL()
{
    static const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                              EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    static const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 0,
                                               EGL_NONE};
    NWindow *window = nwindowGetDefault();
    EGLConfig config;
    EGLint configs = 0;

    if (s_context != EGL_NO_CONTEXT)
        return 1;
    /* the docked screen's size; handheld mode shows a corner of it (Switch_SetDisplayCrop) */
    nwindowSetDimensions(window, 1920, 1080);
    if ((s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY)) == EGL_NO_DISPLAY || !eglInitialize(s_display, 0, 0)) {
        Platform_Log("SheepD3D: EGL: no display (0x%x)", eglGetError());
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(s_display, configAttributes, &config, 1, &configs) ||
        !configs) {
        Platform_Log("SheepD3D: EGL: no config (0x%x)", eglGetError());
        Switch_ShutdownGL();
        return 0;
    }
    if ((s_surface = eglCreateWindowSurface(s_display, config, window, 0)) == EGL_NO_SURFACE ||
        (s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributes)) == EGL_NO_CONTEXT ||
        !eglMakeCurrent(s_display, s_surface, s_surface, s_context)) {
        Platform_Log("SheepD3D: EGL: no OpenGL ES 3 context (0x%x)", eglGetError());
        Switch_ShutdownGL();
        return 0;
    }
    eglSwapInterval(s_display, Switch_EnvInt("SDW_SWAP_INTERVAL", 1));
    return 1;
}

void Switch_ShutdownGL()
{
    if (s_display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_context != EGL_NO_CONTEXT)
        eglDestroyContext(s_display, s_context);
    if (s_surface != EGL_NO_SURFACE)
        eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_display = EGL_NO_DISPLAY;
    s_surface = EGL_NO_SURFACE;
    s_context = EGL_NO_CONTEXT;
}

void Switch_SwapBuffers()
{
    eglSwapBuffers(s_display, s_surface);
    s_framesShown++;
}

void Switch_SetDisplayCrop(unsigned width, unsigned height)
{
    /* GL draws bottom-up: its bottom-left corner is the bottom rows of the buffer the compositor shows */
    if (width >= 1920 && height >= 1080)
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
    else
        nwindowSetCrop(nwindowGetDefault(), 0, 1080 - (s32)height, (s32)width, 1080);
}

/* ---- the keyboard (USB), in DirectInput scan codes ---- */

/* the keys' USB usages (which are SDL's scan codes and libnx's HidKeyboardKey) and DirectInput's codes (the PC's set-1
 * codes, 0x80 added for the E0-prefixed keys): platform_sdl.cpp's table */
static const struct {
    SDL_Scancode usb;
    unsigned char dik;
} s_keyMap[] = {
    {SDL_SCANCODE_ESCAPE, 0x01},       {SDL_SCANCODE_1, 0x02},           {SDL_SCANCODE_2, 0x03},
    {SDL_SCANCODE_3, 0x04},            {SDL_SCANCODE_4, 0x05},           {SDL_SCANCODE_5, 0x06},
    {SDL_SCANCODE_6, 0x07},            {SDL_SCANCODE_7, 0x08},           {SDL_SCANCODE_8, 0x09},
    {SDL_SCANCODE_9, 0x0a},            {SDL_SCANCODE_0, 0x0b},           {SDL_SCANCODE_MINUS, 0x0c},
    {SDL_SCANCODE_EQUALS, 0x0d},       {SDL_SCANCODE_BACKSPACE, 0x0e},   {SDL_SCANCODE_TAB, 0x0f},
    {SDL_SCANCODE_Q, 0x10},            {SDL_SCANCODE_W, 0x11},           {SDL_SCANCODE_E, 0x12},
    {SDL_SCANCODE_R, 0x13},            {SDL_SCANCODE_T, 0x14},           {SDL_SCANCODE_Y, 0x15},
    {SDL_SCANCODE_U, 0x16},            {SDL_SCANCODE_I, 0x17},           {SDL_SCANCODE_O, 0x18},
    {SDL_SCANCODE_P, 0x19},            {SDL_SCANCODE_LEFTBRACKET, 0x1a}, {SDL_SCANCODE_RIGHTBRACKET, 0x1b},
    {SDL_SCANCODE_RETURN, 0x1c},       {SDL_SCANCODE_LCTRL, 0x1d},       {SDL_SCANCODE_A, 0x1e},
    {SDL_SCANCODE_S, 0x1f},            {SDL_SCANCODE_D, 0x20},           {SDL_SCANCODE_F, 0x21},
    {SDL_SCANCODE_G, 0x22},            {SDL_SCANCODE_H, 0x23},           {SDL_SCANCODE_J, 0x24},
    {SDL_SCANCODE_K, 0x25},            {SDL_SCANCODE_L, 0x26},           {SDL_SCANCODE_SEMICOLON, 0x27},
    {SDL_SCANCODE_APOSTROPHE, 0x28},   {SDL_SCANCODE_GRAVE, 0x29},       {SDL_SCANCODE_LSHIFT, 0x2a},
    {SDL_SCANCODE_BACKSLASH, 0x2b},    {SDL_SCANCODE_Z, 0x2c},           {SDL_SCANCODE_X, 0x2d},
    {SDL_SCANCODE_C, 0x2e},            {SDL_SCANCODE_V, 0x2f},           {SDL_SCANCODE_B, 0x30},
    {SDL_SCANCODE_N, 0x31},            {SDL_SCANCODE_M, 0x32},           {SDL_SCANCODE_COMMA, 0x33},
    {SDL_SCANCODE_PERIOD, 0x34},       {SDL_SCANCODE_SLASH, 0x35},       {SDL_SCANCODE_RSHIFT, 0x36},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37},  {SDL_SCANCODE_LALT, 0x38},        {SDL_SCANCODE_SPACE, 0x39},
    {SDL_SCANCODE_CAPSLOCK, 0x3a},     {SDL_SCANCODE_F1, 0x3b},          {SDL_SCANCODE_F2, 0x3c},
    {SDL_SCANCODE_F3, 0x3d},           {SDL_SCANCODE_F4, 0x3e},          {SDL_SCANCODE_F5, 0x3f},
    {SDL_SCANCODE_F6, 0x40},           {SDL_SCANCODE_F7, 0x41},          {SDL_SCANCODE_F8, 0x42},
    {SDL_SCANCODE_F9, 0x43},           {SDL_SCANCODE_F10, 0x44},         {SDL_SCANCODE_NUMLOCKCLEAR, 0x45},
    {SDL_SCANCODE_SCROLLLOCK, 0x46},   {SDL_SCANCODE_KP_7, 0x47},        {SDL_SCANCODE_KP_8, 0x48},
    {SDL_SCANCODE_KP_9, 0x49},         {SDL_SCANCODE_KP_MINUS, 0x4a},    {SDL_SCANCODE_KP_4, 0x4b},
    {SDL_SCANCODE_KP_5, 0x4c},         {SDL_SCANCODE_KP_6, 0x4d},        {SDL_SCANCODE_KP_PLUS, 0x4e},
    {SDL_SCANCODE_KP_1, 0x4f},         {SDL_SCANCODE_KP_2, 0x50},        {SDL_SCANCODE_KP_3, 0x51},
    {SDL_SCANCODE_KP_0, 0x52},         {SDL_SCANCODE_KP_PERIOD, 0x53},   {SDL_SCANCODE_NONUSBACKSLASH, 0x56},
    {SDL_SCANCODE_F11, 0x57},          {SDL_SCANCODE_F12, 0x58},         {SDL_SCANCODE_KP_ENTER, 0x9c},
    {SDL_SCANCODE_RCTRL, 0x9d},        {SDL_SCANCODE_KP_DIVIDE, 0xb5},   {SDL_SCANCODE_PRINTSCREEN, 0xb7},
    {SDL_SCANCODE_RALT, 0xb8},         {SDL_SCANCODE_PAUSE, 0xc5},       {SDL_SCANCODE_HOME, 0xc7},
    {SDL_SCANCODE_UP, 0xc8},           {SDL_SCANCODE_PAGEUP, 0xc9},      {SDL_SCANCODE_LEFT, 0xcb},
    {SDL_SCANCODE_RIGHT, 0xcd},        {SDL_SCANCODE_END, 0xcf},         {SDL_SCANCODE_DOWN, 0xd0},
    {SDL_SCANCODE_PAGEDOWN, 0xd1},     {SDL_SCANCODE_INSERT, 0xd2},      {SDL_SCANCODE_DELETE, 0xd3},
    {SDL_SCANCODE_LGUI, 0xdb},         {SDL_SCANCODE_RGUI, 0xdc},        {SDL_SCANCODE_APPLICATION, 0xdd},
};

void Platform_ReadKeyboard(unsigned char down[256])
{
    size_t i;
    memset(down, 0, 256);
    if (!Platform_IsActive())
        return;
    for (i = 0; i < SDL_arraysize(s_keyMap); i++)
        if (hidKeyboardStateGetKey(&s_keyboard, (HidKeyboardKey)s_keyMap[i].usb))
            down[s_keyMap[i].dik] = 1;
}

int Platform_KeyName(unsigned dikCode, char *out, int outSize)
{
    size_t i;
    *out = 0;
    for (i = 0; i < SDL_arraysize(s_keyMap); i++)
        if (s_keyMap[i].dik == dikCode) {
            snprintf(out, (size_t)outSize, "%s", SDL_GetScancodeName(s_keyMap[i].usb)); /* ASCII names */
            return *out != 0;
        }
    return 0;
}

/* ---- the mouse: none ---- */
void Platform_ReadMouse(int *dx, int *dy, int *wheel, unsigned char buttons[4])
{
    *dx = *dy = *wheel = 0;
    memset(buttons, 0, 4);
}

/* ---- the controller: the handheld controls or player 1's, read as a PlayStation pad ----
 *
 * THE FIXED CONTROLLER MAPPING, in the game's twelve INPUT_BIND_* slots (platform_sdl.cpp's s_padMapping): the face
 * buttons by position, as SDL maps them on the PC (B, the bottom one, is Cross); SDW_SWAP_AB=1 swaps A and B. */
static const u64 s_padMapping[12] = {
    HidNpadButton_X,      /* Triangle */
    HidNpadButton_A,      /* Circle */
    HidNpadButton_B,      /* Cross */
    HidNpadButton_Y,      /* Square */
    HidNpadButton_L,      /* L1 */
    HidNpadButton_R,      /* R1 */
    HidNpadButton_ZL,     /* L2 */
    HidNpadButton_ZR,     /* R2 */
    HidNpadButton_Minus,  /* Select */
    HidNpadButton_Plus,   /* Start */
    HidNpadButton_StickL, /* L3 */
    HidNpadButton_StickR, /* R3 */
};

int Platform_JoystickCount()
{
    return 1; /* the handheld controls or player 1: always there, as far as the game knows */
}

int Platform_WaitForJoystick(int index, int timeoutMs)
{
    (void)timeoutMs;
    return index == 0;
}

const char *Platform_JoystickName(int index)
{
    return index == 0 ? "Nintendo Switch Controller" : "";
}

int Platform_OpenJoystick(int index)
{
    return index == 0;
}

void Platform_CloseJoystick() {}

unsigned Platform_ReadJoystickDpad()
{
    unsigned directions = 0;
    if (!Platform_IsActive())
        return 0;
    if (s_padHeld & HidNpadButton_Up)
        directions |= PLATFORM_DPAD_UP;
    if (s_padHeld & HidNpadButton_Right)
        directions |= PLATFORM_DPAD_RIGHT;
    if (s_padHeld & HidNpadButton_Down)
        directions |= PLATFORM_DPAD_DOWN;
    if (s_padHeld & HidNpadButton_Left)
        directions |= PLATFORM_DPAD_LEFT;
    return directions;
}

int Platform_ReadJoystick(int axes[5], unsigned char *buttons, int buttonCount)
{
    int i, focused = Platform_IsActive(); /* as DirectInput's foreground mode: nothing without the focus */
    HidAnalogStickState left = padGetStickPos(&s_pad, 0), right = padGetStickPos(&s_pad, 1);
    /* -32767..32767, up positive, to about +-65535 with down positive (DirectInput's range and direction) */
    axes[0] = focused ? left.x * 2 : 0;
    axes[1] = focused ? -left.y * 2 : 0;
    axes[2] = 0;
    axes[3] = focused ? right.x * 2 : 0;
    axes[4] = focused ? -right.y * 2 : 0;
    for (i = 0; i < buttonCount; i++) {
        u64 mask = 0;
        if (i < 12) {
            mask = s_padMapping[i];
            if (s_swapAB && (i == 1 || i == 2))
                mask = i == 1 ? HidNpadButton_B : HidNpadButton_A;
        }
        buttons[i] = (unsigned char)(focused && mask && (s_padHeld & mask) != 0);
    }
    return 1;
}
