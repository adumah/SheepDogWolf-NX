/* Choosing and creating the renderer (src/render/render_device.h). */
#include "sdw_types.h"
#include "../sdk/windef.h"
#include "../sdk/win32.h"
#include "../sdk/crt.h"

#include "render_device.h"
#include "../platform/options.h" /* the renderer the command line or the saved settings name */

RenderBackend *g_renderBackend = 0;
RenderDevice *g_renderDevice = 0;
u8 g_textureFilteringEnabled = 1;

static const char *const s_kindNames[RENDERER_COUNT] = {"vulkan", "d3d12", "metal"};
static const char *const s_kindDisplayNames[RENDERER_COUNT] = {"Vulkan (SDL3)", "Direct3D 12 (SDL3)", "Metal (SDL3)"};
static const char *const s_sdl3Drivers[RENDERER_COUNT] = {"vulkan", "direct3d12", "metal"}; /* SDL_GPU's names */

const char *Render_KindName(RendererKind kind)
{
    return (unsigned)kind < RENDERER_COUNT ? s_kindNames[kind] : "?";
}

int Render_KindAvailable(RendererKind kind)
{
#ifdef SDW_RENDER_GL
    return kind == RENDERER_VULKAN; /* one renderer, OpenGL ES (src/render/gl/), in the first slot */
#endif
    /* asking SDL loads the driver: once per kind */
    static int s_known[RENDERER_COUNT], s_available[RENDERER_COUNT];
    if ((unsigned)kind >= RENDERER_COUNT)
        return 0;
    if (!s_known[kind]) {
        s_available[kind] = Sdl3_DriverAvailable(s_sdl3Drivers[kind]);
        s_known[kind] = 1;
    }
    return s_available[kind];
}

static char Render_Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* the kind whose name is the len characters at name (any case) */
static int Render_KindFromName(const char *name, size_t len, RendererKind *out)
{
    int i;
    size_t k;
    for (i = 0; i < RENDERER_COUNT; i++) {
        if (strlen(s_kindNames[i]) != len)
            continue;
        for (k = 0; k < len && Render_Lower(name[k]) == s_kindNames[i][k]; k++)
            ;
        if (k == len) {
            *out = (RendererKind)i;
            return 1;
        }
    }
    return 0;
}

static RendererKind Render_FirstAvailable()
{
    int i;
#ifdef __APPLE__
    if (Render_KindAvailable(RENDERER_METAL))
        return RENDERER_METAL;
#endif
    for (i = 0; i < RENDERER_COUNT; i++)
        if (Render_KindAvailable((RendererKind)i))
            return (RendererKind)i;
    return RENDERER_VULKAN;
}

/* the renderer the options name (options.h) when it can run here, else the first one that can */
RendererKind Render_ConfiguredKind()
{
    RendererKind kind;
    if (Render_KindFromName(g_options.renderer, strlen(g_options.renderer), &kind) && Render_KindAvailable(kind))
        return kind;
    return Render_FirstAvailable();
}

RenderBackend *Render_CreateBackend(RendererKind kind)
{
    delete g_renderBackend;
    g_renderBackend = 0;
    if (!Render_KindAvailable(kind))
        return 0;
#ifdef SDW_RENDER_GL
    g_renderBackend = Gl_CreateRenderBackend();
#else
    g_renderBackend = Sdl3_CreateRenderBackend(s_sdl3Drivers[kind], s_kindDisplayNames[kind]);
#endif
    return g_renderBackend;
}

RenderDevice *Render_CreateDevice(D3DApp *app, long *result)
{
    Render_DestroyDevice();
    *result = -1;
    if (!g_renderBackend)
        return 0;
    g_renderDevice = g_renderBackend->CreateDevice(app, result);
    return g_renderDevice;
}

void Render_DestroyDevice()
{
    delete g_renderDevice;
    g_renderDevice = 0;
}
