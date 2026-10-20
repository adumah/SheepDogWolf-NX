/* The performance overlay's timings and text (perf_overlay.h). */
#ifdef __SWITCH__
#include <SDL2/SDL.h> /* the Nintendo Switch build has SDL2 (src/platform/switch/) */
#else
#include <SDL3/SDL.h>
#endif

#include "perf_overlay.h"
#include "../engine/game_state.h"

static int s_enabled;

/* the timing points of the current frame (performance counter ticks) */
static Uint64 s_workStart, s_presentBegin, s_lastPresentBegin;

/* the window being accumulated, and the last finished one (what the text shows) */
struct PerfWindow {
    Uint64 start;
    u32 frames;
    double frameSum, frameWorst, workSum, workWorst, presentSum;
    double draws, vertices;
};
static PerfWindow s_window;
static int s_shown;
static double s_fps, s_frameMs, s_frameWorstMs, s_workMs, s_workWorstMs, s_presentMs, s_limiterMs;
static u32 s_draws, s_vertices;

#define PERF_WINDOW_SECONDS 0.5

static double PerfOverlay_Ms(Uint64 ticks)
{
    return (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void PerfOverlay_Toggle()
{
    s_enabled = !s_enabled;
    s_shown = 0; /* start a fresh window: nothing stale on screen */
    SDL_zero(s_window);
}

void CamDebug_Toggle() {
    g_camDebugMode = (g_camDebugMode == 2) ? 0 : 2; //0 - game cam, 2 - freecam
}


int PerfOverlay_Enabled()
{
    return s_enabled;
}

void PerfOverlay_FrameStart()
{
    s_workStart = SDL_GetPerformanceCounter();
}

void PerfOverlay_PresentBegin(u32 draws, u32 vertices)
{
    double frame, work;
    s_presentBegin = SDL_GetPerformanceCounter();
    if (!s_enabled)
        return;
    if (!s_lastPresentBegin || !s_workStart || !s_window.start) {
        /* the first frame measured: no previous Present to measure from */
        s_window.start = s_presentBegin;
        s_lastPresentBegin = s_presentBegin;
        return;
    }
    frame = PerfOverlay_Ms(s_presentBegin - s_lastPresentBegin);
    work = s_workStart < s_presentBegin ? PerfOverlay_Ms(s_presentBegin - s_workStart) : 0.0;
    s_lastPresentBegin = s_presentBegin;
    s_window.frames++;
    s_window.frameSum += frame;
    s_window.workSum += work;
    s_window.frameWorst = SDL_max(s_window.frameWorst, frame);
    s_window.workWorst = SDL_max(s_window.workWorst, work);
    s_window.draws += draws;
    s_window.vertices += vertices;
}

void PerfOverlay_PresentEnd()
{
    Uint64 now = SDL_GetPerformanceCounter();
    double elapsed, n;
    if (!s_enabled || !s_window.start)
        return;
    if (s_window.frames)
        s_window.presentSum += PerfOverlay_Ms(now - s_presentBegin);
    elapsed = PerfOverlay_Ms(now - s_window.start) / 1000.0;
    if (elapsed < PERF_WINDOW_SECONDS || !s_window.frames)
        return;
    n = (double)s_window.frames;
    s_fps = n / elapsed;
    s_frameMs = s_window.frameSum / n;
    s_frameWorstMs = s_window.frameWorst;
    s_workMs = s_window.workSum / n;
    s_workWorstMs = s_window.workWorst;
    s_presentMs = s_window.presentSum / n;
    s_limiterMs = SDL_max(0.0, s_frameMs - s_workMs - s_presentMs);
    s_draws = (u32)(s_window.draws / n + 0.5);
    s_vertices = (u32)(s_window.vertices / n + 0.5);
    s_shown = 1;
    SDL_zero(s_window);
    s_window.start = now;
}

#ifndef SDW_VERSION
#define SDW_VERSION "dev" /* set by CMakeLists.txt */
#endif

/* the CPU the game was built for, after the platform on the first line (the release zips' names: macOS-arm64) */
#if defined(__aarch64__) || defined(_M_ARM64)
#define SDW_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define SDW_ARCH "x64"
#else
#define SDW_ARCH ""
#endif

int PerfOverlay_Lines(char lines[PERF_OVERLAY_MAX_LINES][PERF_OVERLAY_LINE_SIZE])
{
    if (!s_enabled)
        return 0;
    SDL_snprintf(lines[0], PERF_OVERLAY_LINE_SIZE, "Rewooled %s %s %s", SDW_VERSION, SDL_GetPlatform(), SDW_ARCH);
    if (!s_shown) {
        SDL_snprintf(lines[1], PERF_OVERLAY_LINE_SIZE, "FPS ...");
        return 2;
    }
    SDL_snprintf(lines[1], PERF_OVERLAY_LINE_SIZE, "FPS %5.1f  FRAME %5.2f MS  WORST %5.2f", s_fps, s_frameMs,
                 s_frameWorstMs);
    SDL_snprintf(lines[2], PERF_OVERLAY_LINE_SIZE, "GAME %5.2f MS  WORST %5.2f", s_workMs, s_workWorstMs);
    SDL_snprintf(lines[3], PERF_OVERLAY_LINE_SIZE, "PRESENT %5.2f MS  LIMITER %5.2f MS", s_presentMs, s_limiterMs);
    SDL_snprintf(lines[4], PERF_OVERLAY_LINE_SIZE, "DRAWS %u  VERTICES %u", (unsigned)s_draws, (unsigned)s_vertices);
    return 5;
}

/* ---- the font ---- */
static const struct {
    char c;
    u8 rows[PERF_FONT_HEIGHT];
} s_font[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'D', {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04}}, {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}}, {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
};

int PerfOverlay_GlyphCount()
{
    return (int)SDL_arraysize(s_font);
}

const u8 *PerfOverlay_GlyphRows(int index)
{
    return s_font[index].rows;
}

int PerfOverlay_GlyphIndex(char c)
{
    int i;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (i = 0; i < (int)SDL_arraysize(s_font); i++)
        if (s_font[i].c == c)
            return i;
    return -1;
}
