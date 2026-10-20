/* The renderer interface of the game.
 *
 * Everything the game draws goes through a RenderDevice, whose backend is picked at start-up (render_select.cpp):
 * SDL3's GPU API, Vulkan or Direct3D 12. It is the device-level 3D pipeline the game uses, in its own terms:
 *
 *   - state:      the game's RSF_* bundles (sdw_enums.h: blending, alpha test, culling, z test/write, texturing,
 *                 filtering, fog...), set and cleared as the original's Render_SetStateFlags / ClearStateFlags do,
 *                 plus a blend function for the two shadow-splash passes and the fog parameters;
 *   - transforms: world / view / projection (row-major 4x4 floats, the game's Mat44);
 *   - vertices:   every draw is a triangle or line list of pre-transformed (screen-space XYZRHW) vertices in ordinary
 *                 memory; vertex buffers are only used to transform XYZ vertices into XYZRHW ones (ProcessVertices,
 *                 with the current transforms) and to read the result back (Lock);
 *   - textures:   pages in three 16-bit formats (565, 1555, 4444), created, locked for the CPU to read or write, copied
 *                 into each other, bound on stage 0; plus a copy of the frame for the iris transition.
 *
 * The constants are the values the game already passes (they are Direct3D 7's), which a backend interprets. The
 * methods' shapes follow the call sites, so that the game's code reaches the device through SDW_RD() (sdw_render.h)
 * without being rewritten.
 *
 * Not behind the interface (dead in this build): reloading the texture pages after DirectDraw lost its surfaces
 * (PolyBatcher::Render_RestoreLostSurfaces; no backend loses them), text drawn with GDI on the back buffer (d3dapp.cpp's
 * frame counter and app_main.cpp's clipboard copy, both unreferenced), and the video player (DirectShow into
 * DirectDraw's primary surface: the videos are skipped). */
#ifndef SDW_RENDER_DEVICE_H
#define SDW_RENDER_DEVICE_H

#include "sdw_types.h"

class D3DApp;

/* ---- constants (Direct3D 7's values) ---- */
/* SetTransform */
#define RD_TRANSFORM_WORLD 1
#define RD_TRANSFORM_VIEW 2
#define RD_TRANSFORM_PROJECTION 3
/* DrawPrimitive */
#define RD_PRIM_LINELIST 2
#define RD_PRIM_TRIANGLELIST 4
/* vertex format bits (FVF) */
#define RD_FVF_XYZ 0x002
#define RD_FVF_XYZRHW 0x004
#define RD_FVF_DIFFUSE 0x040
#define RD_FVF_SPECULAR 0x080
#define RD_FVF_TEX1 0x100
/* Clear */
#define RD_CLEAR_TARGET 0x1
#define RD_CLEAR_ZBUFFER 0x2
/* SetBlendFunc factors */
#define RD_BLEND_ZERO 1
#define RD_BLEND_ONE 2
#define RD_BLEND_SRCALPHA 5
#define RD_BLEND_INVSRCALPHA 6
#define RD_BLEND_DESTCOLOR 9
/* CreateVertexBuffer caps */
#define RD_VBCAPS_DONOTCLIP 0x00000001
/* RdVertexBuffer::Lock flags (hints; a backend may ignore them) */
#define RD_LOCK_WAIT 0x00000001
/* RdVertexBuffer::ProcessVertices */
#define RD_VOP_TRANSFORM 0x00000001
#define RD_PV_DONOTCOPYDATA 0x00000001
/* Present: the frame could not be shown because the display surfaces were lost (DDERR_SURFACELOST) */
#define RD_ERR_SURFACELOST ((long)0x887601C2L)

/* A texture, as the backend knows it. The Direct3D 7 backend's is the page's DirectDraw surface. */
struct RdTexture;

/* LockTexture's access */
#define RD_TEXLOCK_READ 1
#define RD_TEXLOCK_WRITE 2
#define RD_TEXLOCK_READWRITE 3

/* A locked texture's pixels: 16-bit texels in its format, rows pitch bytes apart. Some of the game's code writes
 * rows width * 2 bytes apart whatever the pitch (the pause menu's noise), so a backend should keep them packed. */
struct RdLockedRect {
    void *pixels;
    s32 pitch;
    u32 width, height;
};

/* A rectangle, as the Windows RECT (right and bottom exclusive) */
struct RdRect {
    s32 left, top, right, bottom;
};

class RenderDevice;

/* A buffer of vertices in one format (CreateVertexBuffer's fvf), for transforming XYZ vertices with ProcessVertices
 * and reading them back. */
class RdVertexBuffer {
public:
    virtual long Lock(u32 flags, void **data, u32 *size) = 0; /* the vertices, until Unlock */
    virtual long Unlock() = 0;
    /* count XYZ vertices of src from srcIndex, transformed by the device's world, view and projection into this
     * buffer's XYZRHW vertices from dstIndex (op: RD_VOP_TRANSFORM; flags: RD_PV_DONOTCOPYDATA or 0) */
    virtual long ProcessVertices(u32 op, u32 dstIndex, u32 count, RdVertexBuffer *src, u32 srcIndex,
                                 RenderDevice *device, u32 flags) = 0;
    virtual u32 Release() = 0; /* frees the buffer (the game creates and releases each one once) */

protected:
    virtual ~RdVertexBuffer() {}
};

class RenderDevice {
public:
    virtual ~RenderDevice() {}
    virtual const char *Name() const = 0;

    /* ---- the frame ---- */
    virtual long BeginScene() = 0;
    virtual long EndScene() = 0;
    virtual long Clear(u32 rectCount, void *rects, u32 flags, u32 color, float z, u32 stencil) = 0;
    virtual long Present() = 0; /* shows the finished frame; RD_ERR_SURFACELOST when the display surfaces were lost */

    /* ---- state ---- */
    virtual void SetStateFlags(u32 rsf) = 0;   /* turns on each RSF_* bundle in rsf (Render_SetStateFlags) */
    virtual void ClearStateFlags(u32 rsf) = 0; /* turns them off again (Render_ClearStateFlags) */
    virtual void SetTextureModulation2X(bool enabled) = 0; /* textured RGB: MODULATE2X or MODULATE; alpha unchanged */
    virtual void EnableAlphaBlend(int on) = 0;
    virtual void SetBlendFunc(u32 src, u32 dst) = 0; /* RD_BLEND_* factors */
    virtual void SetFog(u32 color, float start, float end) = 0; /* linear per-vertex fog, as the original sets it */
    virtual void SetFogColor(u32 color) = 0;

    /* ---- transforms, textures, draws ---- */
    virtual long SetTransform(u32 which, void *matrix) = 0; /* RD_TRANSFORM_*; matrix: 16 floats, the game's Mat44 */
    virtual long SetTexture(u32 stage, RdTexture *texture) = 0;
    virtual long DrawPrimitive(u32 primitive, u32 fvf, void *vertices, u32 count, u32 flags) = 0;

    /* ---- vertex buffers ---- */
    virtual long CreateVertexBuffer(u32 caps, u32 fvf, u32 vertexCount, RdVertexBuffer **out) = 0;

    /* ---- textures (the Texture class, src/engine/texture.cpp) ----
     * A texture page in one of the game's three 16-bit formats (TEXFMT_RGB565, TEXFMT_ARGB1555, TEXFMT_ARGB4444). The
     * backend may make it larger than asked (the original rounds up to powers of two, or a square, when the device
     * needs it): *outWidth / *outHeight are its real size. Returns a TEXRES_* result (sdw_enums.h). */
    virtual s32 CreateTexture(u32 width, u32 height, u32 format, RdTexture **out, u32 *outWidth, u32 *outHeight) = 0;
    virtual long LockTexture(RdTexture *texture, u32 access, RdLockedRect *out) = 0; /* RD_TEXLOCK_* */
    virtual long UnlockTexture(RdTexture *texture) = 0;
    /* copies src's srcRect to dst at (x, y), both in the same format (the texture scrollers' backups) */
    virtual long CopyTexture(RdTexture *dst, u32 x, u32 y, RdTexture *src, RdRect *srcRect) = 0;
    virtual void ReleaseTexture(RdTexture *texture) = 0;
    /* whether the texture's contents were lost (the display changed) and must be filled again after RestoreTexture */
    virtual int IsTextureLost(RdTexture *texture) = 0;
    virtual long RestoreTexture(RdTexture *texture) = 0;

    /* ---- the frame as a texture (the iris transition, src/fx/holefx.cpp) ----
     * A texture of at least width x height in the display's format; a negative result when the backend cannot (the
     * transition then uses its mask instead). CopyFrameToTexture copies the shown frame (fromShown) or the one being
     * drawn into dstRect of it. */
    virtual long CreateFrameCaptureTexture(u32 width, u32 height, RdTexture **out, u32 *outWidth, u32 *outHeight) = 0;
    virtual long CopyFrameToTexture(RdTexture *dst, RdRect *dstRect, int fromShown) = 0;
};

/* A renderer before it draws: it sets up the display in the game's window and makes the RenderDevice.
 * The window is the game's (src/platform/platform.h, made with SDL3 after the renderer is chosen); a backend renders
 * into it and never creates or owns it. */
class RenderBackend {
public:
    virtual ~RenderBackend() {}
    virtual const char *Name() const = 0;
    /* the window it needs: PLATFORM_WINDOW_* (platform.h); 0 is the original's bare popup */
    virtual unsigned WindowFlags() const { return 0; }
    /* its size (client area); the original's 800 x 600 unless the backend renders at another size */
    virtual void WindowSize(int *width, int *height) const
    {
        *width = 800;
        *height = 600;
    }
    /* after the launcher, once the window exists: whether this backend can run here (0: it cannot) */
    virtual int PrepareDisplay(D3DApp *app) = 0;
    /* makes the device in the game's window: sets app->clientRect (the render size) and app->deviceReady, and returns
     * the RenderDevice, or 0 (*result: why) */
    virtual RenderDevice *CreateDevice(D3DApp *app, long *result) = 0;
};

/* ---- the current backend and device, and choosing them (render_select.cpp) ---- */
extern RenderBackend *g_renderBackend;
extern RenderDevice *g_renderDevice;

/* Rewooled option: 1 = the game's own bilinear texture filtering, 0 = nearest-neighbour (sharp, pixelated textures).
 * Defined in render_select.cpp; set by the pause menu's "Texture Filtering" item and read by the backend per draw. */
extern u8 g_textureFilteringEnabled;

enum RendererKind {
    RENDERER_VULKAN = 0, /* src/render/sdl3/, SDL_GPU's Vulkan driver */
    RENDERER_D3D12,      /* src/render/sdl3/, SDL_GPU's Direct3D 12 driver */
    RENDERER_METAL,      /* src/render/sdl3/, SDL_GPU's Metal driver */
    RENDERER_COUNT
};
const char *Render_KindName(RendererKind kind);        /* "vulkan", "d3d12", "metal" (the --renderer= / saved spelling) */
int Render_KindAvailable(RendererKind kind);           /* whether the system has the driver */
/* The renderer to use: the one the options name (--renderer=, else the saved setting: src/platform/options.h) when it
 * can run here, else the first available one. */
RendererKind Render_ConfiguredKind();
/* Creates g_renderBackend (at start-up, before the display is set up); 0 when the kind is not available. */
RenderBackend *Render_CreateBackend(RendererKind kind);
/* Sets up the display and creates g_renderDevice with g_renderBackend; 0 when it cannot (*result: why). */
RenderDevice *Render_CreateDevice(D3DApp *app, long *result);
void Render_DestroyDevice();

/* The backend's constructor (src/render/sdl3/). driver: SDL_GPU's name ("vulkan", "direct3d12", "metal"); name: what the backend calls itself */
RenderBackend *Sdl3_CreateRenderBackend(const char *driver, const char *name);
int Sdl3_DriverAvailable(const char *driver);

/* The OpenGL ES 3 backend (src/render/gl/), the only one on the Nintendo Switch (SDW_RENDER_GL): it takes the
 * RENDERER_VULKAN slot, so that the saved settings and the pause menu need nothing new */
RenderBackend *Gl_CreateRenderBackend();

#endif
