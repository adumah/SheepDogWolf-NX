/* The OpenGL ES 3 backend of the renderer interface (src/render/render_device.h), for the Nintendo Switch: Mesa's
 * nouveau driver through EGL, drawing into the console's default native window.
 *
 * It is the SDL3 GPU backend (src/render/sdl3/) in GL terms, and keeps its behaviour: the draws of a frame are recorded
 * and replayed in as few passes as possible (a clear, a texture changed after the frame drew with it, or a frame copy
 * ends one: Flush); triangles are culled on the CPU with Direct3D's rules; the shaders are game.hlsl's.
 *
 * What differs from the SDL3 backend:
 *   - the frames are framebuffer objects, whose row 0 is the bottom of the picture: the frame copies for the iris
 *     transition flip it back (CopyFrameToTexture);
 *   - the game's textures are uploaded in their own 16-bit formats (rotated into GL's component order) instead of
 *     being widened to RGBA8 on every upload;
 *   - the GPU frame follows the console's screen: 1920 x 1080 docked, 1280 x 720 handheld, times the screen percent.
 *     The game keeps believing it renders at the options' size (1920 x 1080). The native window stays 1920 x 1080; in
 *     handheld mode the picture goes into its 1280 x 720 corner and the compositor is told to show only that (crop). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
/* no switch.h: the console is reached through platform_switch.h (libnx's u64 is not the game's, sdw_types.h) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdw_types.h"
#include "sdw_enums.h"
#include "../render_device.h"
#include "../perf_overlay.h"
#include "../../platform/platform.h"
#include "../../platform/options.h"
#include "../../platform/switch/platform_switch.h"

/* d3dapp.cpp */
long D3DApp_StartWithoutDirectDraw(D3DApp *app, u32 width, u32 height);

#define GL_E_FAIL ((long)0x80004005L)
/* the size the game renders at: the options' (--width / --height), 1920 x 1080 by default */
#define GL_GAME_WIDTH (g_options.width > 0 ? g_options.width : 1920)
#define GL_GAME_HEIGHT (g_options.height > 0 ? g_options.height : 1080)
/* the native window: the docked screen's size; handheld mode shows its 1280 x 720 corner */
#define GL_WINDOW_WIDTH 1920
#define GL_WINDOW_HEIGHT 1080

#define GL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GL_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Screen percent (g_options.renderScale: --scale or the in-game row; 25..200, 100 when unset) */
static int Gl_RenderScale()
{
    int percent = g_options.renderScale;
    if (percent <= 0)
        return 100;
    return percent < 25 ? 25 : (percent > 200 ? 200 : percent);
}

/* The sample counts this GPU can render with, as bits: 1 = 2x, 2 = 4x, 4 = 8x (the pause menu reads it, as with the
 * SDL3 backend) */
u8 g_msaaSupportMask = 0;

/* the largest supported count up to wanted (mask as above); 1, no anti-aliasing, when wanted is below 2 or none fits */
static int Gl_BestSampleCount(u8 mask, int wanted)
{
    if (wanted >= 8 && (mask & 4))
        return 8;
    if (wanted >= 4 && (mask & 2))
        return 4;
    if (wanted >= 2 && (mask & 1))
        return 2;
    return 1;
}

/* Direct3D 7 values the backend interprets (render_device.h passes the game's) */
enum {
    D3D_BLEND_ZERO = 1,
    D3D_BLEND_ONE = 2,
    D3D_BLEND_SRCCOLOR = 3,
    D3D_BLEND_INVSRCCOLOR = 4,
    D3D_BLEND_SRCALPHA = 5,
    D3D_BLEND_INVSRCALPHA = 6,
    D3D_BLEND_DESTALPHA = 7,
    D3D_BLEND_INVDESTALPHA = 8,
    D3D_BLEND_DESTCOLOR = 9,
    D3D_BLEND_INVDESTCOLOR = 10,
    D3D_BLEND_SRCALPHASAT = 11,
    D3D_CULL_NONE = 1,
    D3D_CULL_CW = 2,
    D3D_CULL_CCW = 3,
    D3D_FVF_POSITION_MASK = 0x00e,
    D3D_FVF_NORMAL = 0x010,
    D3D_FVF_PSIZE = 0x020,
    D3D_FVF_TEXCOUNT_SHIFT = 8,
    D3D_FVF_TEXCOUNT_MASK = 0xf00,
};

/* ---- vertices ---- */

/* the layout of one Direct3D 7 flexible vertex format (offsets in bytes, -1 when absent) */
struct GlFvf {
    u32 stride;
    int transformed; /* XYZRHW */
    int diffuse, specular, tex;
};

static void Gl_ParseFvf(u32 fvf, GlFvf *out)
{
    u32 offset;
    u32 position = fvf & D3D_FVF_POSITION_MASK;
    out->transformed = position == RD_FVF_XYZRHW;
    offset = out->transformed ? 16 : 12; /* XYZ, and the XYZB1..5 blend weights the game never uses */
    if (position > RD_FVF_XYZRHW)
        offset += 4 * ((position - RD_FVF_XYZRHW) / 2);
    if (fvf & D3D_FVF_NORMAL)
        offset += 12;
    if (fvf & D3D_FVF_PSIZE)
        offset += 4;
    out->diffuse = -1;
    out->specular = -1;
    out->tex = -1;
    if (fvf & RD_FVF_DIFFUSE) {
        out->diffuse = (int)offset;
        offset += 4;
    }
    if (fvf & RD_FVF_SPECULAR) {
        out->specular = (int)offset;
        offset += 4;
    }
    if ((fvf & D3D_FVF_TEXCOUNT_MASK) >> D3D_FVF_TEXCOUNT_SHIFT) {
        out->tex = (int)offset;
        offset += 8 * ((fvf & D3D_FVF_TEXCOUNT_MASK) >> D3D_FVF_TEXCOUNT_SHIFT);
    }
    out->stride = offset;
}

/* what the GPU gets: a pre-transformed vertex (the vertex shader's inputs) */
struct GlVertex {
    float x, y, z, rhw;
    u32 diffuse, specular; /* D3DCOLOR */
    float u, v;
};

/* the viewport transform after world x view x projection: Direct3D 7's, for a viewport of width x height, z 0..1 */
static void Gl_Transform(const float *m, const float *in, float width, float height, float *out)
{
    float x = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
    float y = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
    float z = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
    float w = in[0] * m[3] + in[1] * m[7] + in[2] * m[11] + m[15];
    float rhw = 1.0f / w;
    out[0] = (1.0f + x * rhw) * width * 0.5f;
    out[1] = (1.0f - y * rhw) * height * 0.5f;
    out[2] = z * rhw;
    out[3] = rhw;
}

static void Gl_MatMul(const float *a, const float *b, float *out)
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

class GlRenderDevice;

/* a vertex buffer: plain memory (the game only transforms into it and reads it back) */
class GlVertexBuffer : public RdVertexBuffer {
public:
    GlVertexBuffer(u32 fvf, u32 count) : fvf(fvf), count(count)
    {
        Gl_ParseFvf(fvf, &layout);
        data = (u8 *)calloc(count ? count : 1, layout.stride);
    }
    long Lock(u32 flags, void **out, u32 *size)
    {
        (void)flags;
        *out = data;
        if (size)
            *size = count * layout.stride;
        return 0;
    }
    long Unlock() { return 0; }
    long ProcessVertices(u32 op, u32 dstIndex, u32 n, RdVertexBuffer *src, u32 srcIndex, RenderDevice *device,
                         u32 flags);
    u32 Release()
    {
        delete this;
        return 0;
    }

    u32 fvf, count;
    GlFvf layout;
    u8 *data;

private:
    ~GlVertexBuffer() { free(data); }
};

/* ---- textures ---- */
struct GlTexture {
    GLuint gl;
    GLuint fbo;  /* a frame capture's: the blit target (0 for the game's textures) */
    u16 *pixels; /* the texels, rows packed: 16-bit, or 32-bit RGBA for TEXFMT_RGBA8 (Gl_TexelBytes); 0 for a frame
                  * capture (on the GPU only) */
    u32 width, height, format;
    int dirty;         /* the texels changed since the last upload */
    int lockedToWrite; /* locked with write access: dirty at Unlock */
    u32 drawnInBatch;  /* the batch (Flush count) that last drew with it */
};

/* bytes per texel of a texture format */
static u32 Gl_TexelBytes(u32 format)
{
    return format == TEXFMT_RGBA8 ? 4 : 2;
}

/* the GL storage of a texture format */
static void Gl_TextureFormat(u32 format, GLenum *internal, GLenum *external, GLenum *type)
{
    switch (format) {
        case TEXFMT_RGBA8:
            *internal = GL_RGBA8;
            *external = GL_RGBA;
            *type = GL_UNSIGNED_BYTE;
            break;
        case TEXFMT_ARGB1555:
            *internal = GL_RGB5_A1;
            *external = GL_RGBA;
            *type = GL_UNSIGNED_SHORT_5_5_5_1;
            break;
        case TEXFMT_ARGB4444:
            *internal = GL_RGBA4;
            *external = GL_RGBA;
            *type = GL_UNSIGNED_SHORT_4_4_4_4;
            break;
        default: /* TEXFMT_RGB565: GL's own layout */
            *internal = GL_RGB565;
            *external = GL_RGB;
            *type = GL_UNSIGNED_SHORT_5_6_5;
    }
}

/* The texels in GL's order, into out (as many as the texture has). Direct3D's 16-bit formats put alpha in the top
 * bits, GL's in the bottom ones: ARGB1555 rotated left by 1 is RGBA5551, ARGB4444 rotated left by 4 is RGBA4444. 565
 * and RGBA8 are uploaded as they are (returns the texels themselves). */
static const void *Gl_ConvertTexels(const GlTexture *t, u16 *out)
{
    u32 i, n = t->width * t->height;
    const u16 *p = t->pixels;
    switch (t->format) {
        case TEXFMT_ARGB1555:
            for (i = 0; i < n; i++)
                out[i] = (u16)((p[i] << 1) | (p[i] >> 15));
            return out;
        case TEXFMT_ARGB4444:
            for (i = 0; i < n; i++)
                out[i] = (u16)((p[i] << 4) | (p[i] >> 12));
            return out;
        default:
            return p;
    }
}

/* ---- recorded draws ---- */

struct GlPsParams {
    float fogColor[4];
    float mode[4]; /* texture mode (0: diffuse, 1: MODULATE, 2: MODULATE2X), alpha test, fog, specular */
};

struct GlDraw {
    int lines;
    int blend;
    u32 srcBlend, dstBlend;
    int zTest, zWrite;
    GlTexture *texture;
    int linear;
    GlPsParams ps;
    u32 firstVertex, vertexCount;
};

/* the Direct3D 7 state the game changes */
struct GlState {
    int blend;
    u32 srcBlend, dstBlend;
    int alphaTest, zEnable, zWrite, specular, fog, linear;
    u32 cull;
    int textured; /* 0: diffuse (SELECTARG1 DIFFUSE or DISABLE), 1: MODULATE, 2: MODULATE2X */
    u32 fogColor;
    GlTexture *texture;
};

static GLenum Gl_BlendFactor(u32 d3dBlend)
{
    switch (d3dBlend) {
        case D3D_BLEND_ZERO:
            return GL_ZERO;
        case D3D_BLEND_SRCCOLOR:
            return GL_SRC_COLOR;
        case D3D_BLEND_INVSRCCOLOR:
            return GL_ONE_MINUS_SRC_COLOR;
        case D3D_BLEND_SRCALPHA:
            return GL_SRC_ALPHA;
        case D3D_BLEND_INVSRCALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;
        case D3D_BLEND_DESTALPHA:
            return GL_DST_ALPHA;
        case D3D_BLEND_INVDESTALPHA:
            return GL_ONE_MINUS_DST_ALPHA;
        case D3D_BLEND_DESTCOLOR:
            return GL_DST_COLOR;
        case D3D_BLEND_INVDESTCOLOR:
            return GL_ONE_MINUS_DST_COLOR;
        case D3D_BLEND_SRCALPHASAT:
            return GL_SRC_ALPHA_SATURATE;
        default:
            return GL_ONE;
    }
}

/* ---- the shaders: game.hlsl (src/render/sdl3/shaders/), in GLSL ES ---- */
static const char s_vertexShader[] =
    "#version 300 es\n"
    "layout(location = 0) in highp vec4 aPos;      /* x, y, z, rhw */\n"
    "layout(location = 1) in lowp vec4 aDiffuse;   /* a D3DCOLOR's bytes: b, g, r, a */\n"
    "layout(location = 2) in lowp vec4 aSpecular;  /* the same; its alpha is the fog factor */\n"
    "layout(location = 3) in highp vec2 aUv;\n"
    "uniform highp vec2 uScreen;                    /* 2 / width, 2 / height */\n"
    "out lowp vec4 vDiffuse;\n"
    "out lowp vec4 vSpecular;\n"
    "out highp vec2 vUv;\n"
    "void main()\n"
    "{\n"
    "    /* w keeps its sign: triangles reaching behind the eye are cut by the clipper at the near plane */\n"
    "    highp float w = 1.0 / (abs(aPos.w) > 1e-30 ? aPos.w : 1e-30);\n"
    "    /* + 0.5: Direct3D 7 puts a pixel's centre at whole coordinates */\n"
    "    highp float x = (aPos.x + 0.5) * uScreen.x - 1.0;\n"
    "    highp float y = 1.0 - (aPos.y + 0.5) * uScreen.y;\n"
    "    /* z 0..1 (Direct3D) to GL's -1..1 clip range */\n"
    "    gl_Position = vec4(x * w, y * w, (aPos.z * 2.0 - 1.0) * w, w);\n"
    "    vDiffuse = aDiffuse.zyxw;\n"
    "    vSpecular = aSpecular.zyxw;\n"
    "    vUv = aUv;\n"
    "}\n";

static const char s_fragmentShader[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uFogColor;\n"
    "uniform vec4 uMode; /* x: 0 diffuse, 1 MODULATE, 2 MODULATE2X; y: alpha test, z: fog, w: specular */\n"
    "in lowp vec4 vDiffuse;\n"
    "in lowp vec4 vSpecular;\n"
    "in highp vec2 vUv;\n"
    "out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 c = vDiffuse;\n"
    "    if (uMode.x > 0.5) {\n"
    "        vec4 t = texture(uTex, vUv);\n"
    "        c = vec4(clamp(t.rgb * vDiffuse.rgb * uMode.x, 0.0, 1.0), t.a);\n"
    "    }\n"
    "    if (uMode.w > 0.5)\n"
    "        c.rgb = clamp(c.rgb + vSpecular.rgb, 0.0, 1.0);\n"
    "    if (uMode.z > 0.5)\n"
    "        c.rgb = mix(uFogColor.rgb, c.rgb, vSpecular.a);\n"
    "    /* ALPHAREF 8, ALPHAFUNC LESSEQUAL (the game's alpha is inverted: 0 is opaque) */\n"
    "    if (uMode.y > 0.5 && c.a * 255.0 > 8.5)\n"
    "        discard;\n"
    "    oColor = c;\n"
    "}\n";

static GLuint Gl_CompileShader(GLenum stage, const char *source)
{
    GLuint shader = glCreateShader(stage);
    GLint ok = 0;
    glShaderSource(shader, 1, &source, 0);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), 0, log);
        Platform_Log("SheepD3D: GL shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* ---- the device ---- */
class GlRenderDevice : public RenderDevice {
public:
    GlRenderDevice(u32 width, u32 height);
    ~GlRenderDevice();
    int Init(); /* 0 when a GL object could not be made */
    const char *Name() const { return name; }

    long BeginScene() { return 0; }
    long EndScene() { return 0; }
    long Clear(u32 rectCount, void *rects, u32 flags, u32 color, float z, u32 stencil);
    long Present();

    void SetStateFlags(u32 rsf);
    void ClearStateFlags(u32 rsf);
    void SetTextureModulation2X(bool enabled) { state.textured = enabled ? 2 : 1; }
    void EnableAlphaBlend(int on) { state.blend = on != 0; }
    void SetBlendFunc(u32 src, u32 dst)
    {
        state.srcBlend = src;
        state.dstBlend = dst;
    }
    void SetFog(u32 color, float start, float end)
    {
        (void)start; /* vertex fog: the game computes the factor into the specular alpha */
        (void)end;
        state.fogColor = color;
    }
    void SetFogColor(u32 color) { state.fogColor = color; }

    long SetTransform(u32 which, void *matrix);
    long SetTexture(u32 stage, RdTexture *texture)
    {
        if (stage == 0)
            state.texture = (GlTexture *)texture;
        return 0;
    }
    long DrawPrimitive(u32 primitive, u32 fvf, void *vertices, u32 count, u32 flags);

    long CreateVertexBuffer(u32 caps, u32 fvf, u32 vertexCount, RdVertexBuffer **out)
    {
        (void)caps;
        *out = new GlVertexBuffer(fvf, vertexCount);
        return 0;
    }

    s32 CreateTexture(u32 width, u32 height, u32 format, RdTexture **out, u32 *outWidth, u32 *outHeight);
    long LockTexture(RdTexture *texture, u32 access, RdLockedRect *out);
    long UnlockTexture(RdTexture *texture);
    long CopyTexture(RdTexture *dst, u32 x, u32 y, RdTexture *src, RdRect *srcRect);
    void ReleaseTexture(RdTexture *texture);
    int IsTextureLost(RdTexture *texture)
    {
        (void)texture;
        return 0;
    }
    long RestoreTexture(RdTexture *texture)
    {
        (void)texture;
        return 0;
    }
    long CreateFrameCaptureTexture(u32 width, u32 height, RdTexture **out, u32 *outWidth, u32 *outHeight);
    long CopyFrameToTexture(RdTexture *dst, RdRect *dstRect, int fromShown);

    /* world x view x projection, for ProcessVertices */
    const float *Combined();
    float Width() const { return (float)width; }
    float Height() const { return (float)height; }

private:
    void Flush();
    int CreateTargets();
    void ReleaseTargets();
    void ApplyRenderSettings();
    void Resolve(); /* the multisampled frame into frames[current] */
    void BeginBatch() { batch++; }
    void MarkUsed(GlTexture *t) { t->drawnInBatch = batch; }
    int DrawnInBatch(const GlTexture *t) const { return t->drawnInBatch == batch && drawCount; }
    void WaitUntilUnused(GlTexture *t)
    {
        if (DrawnInBatch(t))
            Flush();
    }
    GlVertex *AllocVertices(u32 n);
    int TrackTexture(GlTexture *t);
    void UntrackTexture(GlTexture *t);
    void Upload(GlTexture *t);
    void DrawPerfOverlay();

    u32 width, height;             /* the size the game believes it renders at */
    u32 displayWidth, displayHeight; /* the screen's: 1920 x 1080 docked, 1280 x 720 handheld */
    u32 gpuWidth, gpuHeight;       /* the size of the frames: the screen's times the screen percent */
    int appliedScale, appliedMsaa, appliedDocked;
    char name[96];

    GLuint program;
    GLint uScreen, uFogColor, uMode;
    GLuint vao, vbo;
    u32 vboSize;
    GLuint pointSampler, linearSampler;
    GLuint frames[2], frameFbos[2];
    int current;
    GLuint depth;                  /* the depth renderbuffer (multisampled with the MSAA frame) */
    int sampleCount;               /* 1 = no anti-aliasing */
    GLuint msaaColor, msaaFbo;     /* the multisampled frame, resolved into frames[current] */
    int needsResolve;
    GlTexture white;               /* bound when the game has no texture set */
    GlTexture font;                /* the performance overlay's glyphs, made the first time it is shown */
    u16 *scratch;                  /* converted texels on their way to GL */
    u32 scratchTexels;

    /* the recorded batch */
    u32 batch;
    GlVertex *vertices;
    u32 vertexCount, vertexCapacity;
    GlDraw *draws;
    u32 drawCount, drawCapacity;
    u32 frameDraws, frameVertices; /* what the frame submitted so far, for the performance overlay */
    int clearColor, clearDepth;
    float clearColorValue[4];
    float clearDepthValue;

    /* textures, for uploading the changed ones */
    GlTexture **textures;
    u32 textureCount, textureCapacity;

    GlState state;
    float matrices[3][16]; /* world, view, projection */
    float combined[16];
    int combinedValid;
};

GlRenderDevice::GlRenderDevice(u32 width, u32 height) : width(width), height(height)
{
    int i;
    snprintf(name, sizeof(name), "OpenGL ES (%s)", (const char *)glGetString(GL_RENDERER));
    displayWidth = GL_WINDOW_WIDTH;
    displayHeight = GL_WINDOW_HEIGHT;
    gpuWidth = width;
    gpuHeight = height;
    appliedScale = 100;
    appliedMsaa = 0;
    appliedDocked = 1;
    program = 0;
    uScreen = uFogColor = uMode = -1;
    vao = vbo = 0;
    vboSize = 0;
    pointSampler = linearSampler = 0;
    frames[0] = frames[1] = frameFbos[0] = frameFbos[1] = 0;
    current = 0;
    depth = 0;
    sampleCount = 1;
    msaaColor = msaaFbo = 0;
    needsResolve = 0;
    memset(&white, 0, sizeof(white));
    memset(&font, 0, sizeof(font));
    scratch = 0;
    scratchTexels = 0;
    batch = 1;
    vertices = 0;
    vertexCount = vertexCapacity = 0;
    draws = 0;
    drawCount = drawCapacity = 0;
    frameDraws = frameVertices = 0;
    clearColor = clearDepth = 0;
    clearDepthValue = 1.0f;
    memset(clearColorValue, 0, sizeof(clearColorValue));
    textures = 0;
    textureCount = textureCapacity = 0;

    /* Direct3D 7's defaults (the game only changes what its RSF_* bundles touch) */
    memset(&state, 0, sizeof(state));
    state.srcBlend = D3D_BLEND_ONE;
    state.dstBlend = D3D_BLEND_ZERO;
    state.zEnable = 1; /* the render target has a z buffer */
    state.zWrite = 1;
    state.cull = D3D_CULL_CCW;
    state.textured = 1;
    for (i = 0; i < 3; i++) {
        memset(matrices[i], 0, sizeof(matrices[i]));
        matrices[i][0] = matrices[i][5] = matrices[i][10] = matrices[i][15] = 1.0f;
    }
    combinedValid = 0;
}

int GlRenderDevice::Init()
{
    GLuint vs, fs;
    GLint ok = 0, maxSamples = 0;

    if (!(vs = Gl_CompileShader(GL_VERTEX_SHADER, s_vertexShader)) ||
        !(fs = Gl_CompileShader(GL_FRAGMENT_SHADER, s_fragmentShader)))
        return 0;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), 0, log);
        Platform_Log("SheepD3D: GL program: %s", log);
        return 0;
    }
    glUseProgram(program);
    uScreen = glGetUniformLocation(program, "uScreen");
    uFogColor = glGetUniformLocation(program, "uFogColor");
    uMode = glGetUniformLocation(program, "uMode");
    glUniform1i(glGetUniformLocation(program, "uTex"), 0);

    /* one vertex buffer, refilled at every Flush */
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), (const void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), (const void *)16);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), (const void *)20);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), (const void *)24);

    glGenSamplers(1, &pointSampler);
    glGenSamplers(1, &linearSampler);
    glSamplerParameteri(pointSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(pointSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(linearSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(linearSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(pointSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(pointSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glSamplerParameteri(linearSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(linearSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); /* the game's textures can be any width */
    glDisable(GL_CULL_FACE);               /* culled on the CPU (DrawPrimitive) */
    glDepthFunc(GL_LEQUAL);                /* D3DCMP_LESSEQUAL, the default */
    glDisable(GL_DITHER);

    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    g_msaaSupportMask = (u8)((maxSamples >= 2 ? 1 : 0) | (maxSamples >= 4 ? 2 : 0) | (maxSamples >= 8 ? 4 : 0));

    if (!CreateTargets())
        return 0;

    /* a white texel (4444: 0xffff), bound when there is no texture: MODULATE then gives the diffuse colour */
    white.width = white.height = 1;
    white.format = TEXFMT_ARGB4444;
    white.pixels = (u16 *)malloc(2);
    white.pixels[0] = 0xffff;
    glGenTextures(1, &white.gl);
    glBindTexture(GL_TEXTURE_2D, white.gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA4, 1, 1);
    white.dirty = 1;
    TrackTexture(&white);

    /* both frames black, as the original's first Clear */
    clearColor = clearDepth = 1;
    clearColorValue[3] = 1.0f;
    Flush();
    current ^= 1;
    clearColor = clearDepth = 1;
    Flush();
    current ^= 1;
    return 1;
}

void GlRenderDevice::ReleaseTargets()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, frameFbos);
    glDeleteTextures(2, frames);
    frameFbos[0] = frameFbos[1] = frames[0] = frames[1] = 0;
    if (msaaFbo)
        glDeleteFramebuffers(1, &msaaFbo);
    if (msaaColor)
        glDeleteRenderbuffers(1, &msaaColor);
    if (depth)
        glDeleteRenderbuffers(1, &depth);
    msaaFbo = msaaColor = depth = 0;
    needsResolve = 0;
}

/* The frames, the depth buffer and (with anti-aliasing) the multisampled frame, at the screen's size, the screen percent
 * and the sample count the options name now. The game keeps drawing at its own size (width x height): its vertices are
 * turned into clip space with that size, so a different GPU frame just rasterizes the same picture with other pixels. */
int GlRenderDevice::CreateTargets()
{
    int i, docked = Switch_IsDocked();
    GLint maxSize = 4096;

    displayWidth = docked ? 1920 : 1280;
    displayHeight = docked ? 1080 : 720;
    appliedDocked = docked;
    appliedScale = Gl_RenderScale();
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxSize);
    gpuWidth = GL_MAX(1u, GL_MIN((u32)maxSize, displayWidth * (u32)appliedScale / 100u));
    gpuHeight = GL_MAX(1u, GL_MIN((u32)maxSize, displayHeight * (u32)appliedScale / 100u));
    sampleCount = Gl_BestSampleCount(g_msaaSupportMask, g_options.msaa);
    /* g_options.msaa stays what was asked for: the menu shows a choice this GPU cannot do as unsupported */
    appliedMsaa = g_options.msaa < 2 ? 0 : g_options.msaa;

    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    /* 16-bit, as the original's, so that surfaces drawn over each other at nearly the same depth resolve as they did */
    if (sampleCount > 1)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, sampleCount, GL_DEPTH_COMPONENT16, gpuWidth, gpuHeight);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, gpuWidth, gpuHeight);

    /* the frames: RGBA8, sampled by nothing but blitted from */
    glGenTextures(2, frames);
    glGenFramebuffers(2, frameFbos);
    for (i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, frames[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, gpuWidth, gpuHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, frameFbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frames[i], 0);
        if (sampleCount == 1)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ReleaseTargets();
            return 0;
        }
    }
    if (sampleCount > 1) {
        glGenRenderbuffers(1, &msaaColor);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, sampleCount, GL_RGBA8, gpuWidth, gpuHeight);
        glGenFramebuffers(1, &msaaFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ReleaseTargets();
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Switch_SetDisplayCrop(displayWidth, displayHeight);
    Platform_Log("SheepD3D: %s, game %ux%u, rendering at %ux%u (%d%%), MSAA %dx", docked ? "docked" : "handheld",
                 (unsigned)width, (unsigned)height, (unsigned)gpuWidth, (unsigned)gpuHeight, appliedScale,
                 sampleCount == 1 ? 0 : sampleCount);
    return 1;
}

/* Called at the end of every Present, when no frame is half drawn: if the console was docked or undocked, or the screen
 * percent or the anti-aliasing changed (the in-game rows), makes the targets again and clears both frames black. */
void GlRenderDevice::ApplyRenderSettings()
{
    int wantedMsaa = g_options.msaa < 2 ? 0 : g_options.msaa;
    int previousScale = appliedScale, previousMsaa = appliedMsaa;

    if (Gl_RenderScale() == appliedScale && wantedMsaa == appliedMsaa && Switch_IsDocked() == appliedDocked)
        return;
    /* a choice that runs as the sample count already running (an unsupported one) needs no new targets */
    if (Gl_RenderScale() == appliedScale && Switch_IsDocked() == appliedDocked &&
        Gl_BestSampleCount(g_msaaSupportMask, wantedMsaa) == sampleCount) {
        appliedMsaa = wantedMsaa;
        return;
    }
    ReleaseTargets();
    if (!CreateTargets()) {
        /* e.g. out of memory: go back to what worked */
        Platform_Log("SheepD3D: the new render settings failed, going back");
        g_options.renderScale = previousScale;
        g_options.msaa = previousMsaa;
        if (!CreateTargets())
            Platform_Log("SheepD3D: the previous render settings failed too");
    }
    clearColor = clearDepth = 1;
    Flush();
    current ^= 1;
    clearColor = clearDepth = 1;
    Flush();
    current ^= 1;
}

GlRenderDevice::~GlRenderDevice()
{
    ReleaseTargets();
    if (program)
        glDeleteProgram(program);
    if (vbo)
        glDeleteBuffers(1, &vbo);
    if (vao)
        glDeleteVertexArrays(1, &vao);
    if (pointSampler)
        glDeleteSamplers(1, &pointSampler);
    if (linearSampler)
        glDeleteSamplers(1, &linearSampler);
    if (white.gl)
        glDeleteTextures(1, &white.gl);
    free(white.pixels);
    if (font.gl)
        glDeleteTextures(1, &font.gl);
    free(font.pixels);
    free(scratch);
    free(vertices);
    free(draws);
    /* the game's textures still alive (the menu noise is deleted after the device) are left to the context */
    free(textures);
}

int GlRenderDevice::TrackTexture(GlTexture *t)
{
    if (textureCount == textureCapacity) {
        u32 capacity = textureCapacity ? textureCapacity * 2 : 256;
        GlTexture **grown = (GlTexture **)realloc(textures, capacity * sizeof(*textures));
        if (!grown)
            return 0;
        textures = grown;
        textureCapacity = capacity;
    }
    textures[textureCount++] = t;
    return 1;
}

void GlRenderDevice::UntrackTexture(GlTexture *t)
{
    u32 i;
    for (i = 0; i < textureCount; i++)
        if (textures[i] == t) {
            textures[i] = textures[--textureCount];
            return;
        }
}

/* ---- state: Render_SetStateFlags / ClearStateFlags, as the original sets Direct3D's (src/app/d3dapp_inlines.h) ---- */
void GlRenderDevice::SetStateFlags(u32 flags)
{
    if (flags & RSF_BLEND_ALPHA) {
        state.blend = 1;
        state.srcBlend = D3D_BLEND_INVSRCALPHA;
        state.dstBlend = D3D_BLEND_SRCALPHA;
    }
    if (flags & RSF_BLEND_ADD) {
        state.blend = 1;
        state.srcBlend = D3D_BLEND_INVSRCALPHA;
        state.dstBlend = D3D_BLEND_ONE;
    }
    if (flags & RSF_ALPHATEST)
        state.alphaTest = 1;
    if (flags & RSF_SPECULAR)
        state.specular = 1;
    if (flags & RSF_CULL_CW)
        state.cull = D3D_CULL_CW;
    if (flags & RSF_CULL_CCW)
        state.cull = D3D_CULL_CCW;
    if (flags & RSF_ZTEST)
        state.zEnable = 1;
    /* the original turns z testing on only when setting the z write state fails, which it does not */
    if (flags & RSF_ZWRITE_ON)
        state.zWrite = 1;
    if (flags & RSF_ZWRITE_OFF)
        state.zWrite = 0;
    state.textured = (flags & RSF_TEXTURED) ? 2 : 0; /* ColorFix: MODULATE2X; else SELECTARG1 DIFFUSE */
    if (flags & RSF_FILTER_LINEAR)
        state.linear = 1;
    if (flags & RSF_FOG)
        state.fog = 1;
    /* RSF_ANTIALIAS, CLIPPLANE (no effect on pre-transformed vertices), DITHER, LIGHTING, COLORVERTEX: nothing to do */
}

void GlRenderDevice::ClearStateFlags(u32 flags)
{
    if ((flags & RSF_BLEND_ALPHA) || (flags & RSF_BLEND_ADD))
        state.blend = 0;
    if (flags & RSF_ALPHATEST)
        state.alphaTest = 0;
    if (flags & RSF_SPECULAR)
        state.specular = 0;
    if ((flags & RSF_CULL_CW) || (flags & RSF_CULL_CCW))
        state.cull = D3D_CULL_NONE;
    if (flags & RSF_ZTEST)
        state.zEnable = 0;
    if (flags & RSF_ZWRITE_ON)
        state.zWrite = 0;
    if (flags & RSF_ZWRITE_OFF)
        state.zWrite = 1;
    if (flags & RSF_TEXTURED)
        state.textured = 0; /* COLOROP DISABLE: the diffuse colour */
    if (flags & RSF_FILTER_LINEAR)
        state.linear = 0;
    if (flags & RSF_FOG)
        state.fog = 0;
}

long GlRenderDevice::SetTransform(u32 which, void *matrix)
{
    if (which < RD_TRANSFORM_WORLD || which > RD_TRANSFORM_PROJECTION)
        return GL_E_FAIL;
    memcpy(matrices[which - RD_TRANSFORM_WORLD], matrix, sizeof(matrices[0]));
    combinedValid = 0;
    return 0;
}

const float *GlRenderDevice::Combined()
{
    if (!combinedValid) {
        float worldView[16];
        Gl_MatMul(matrices[0], matrices[1], worldView);
        Gl_MatMul(worldView, matrices[2], combined);
        combinedValid = 1;
    }
    return combined;
}

long GlVertexBuffer::ProcessVertices(u32 op, u32 dstIndex, u32 n, RdVertexBuffer *srcBuffer, u32 srcIndex,
                                     RenderDevice *device, u32 flags)
{
    GlVertexBuffer *src = (GlVertexBuffer *)srcBuffer;
    GlRenderDevice *dev = (GlRenderDevice *)device;
    const float *m;
    u32 i;
    (void)op; /* RD_VOP_TRANSFORM: the game never asks for lighting or clipping */
    if (!layout.transformed || src->layout.transformed || dstIndex + n > count || srcIndex + n > src->count)
        return GL_E_FAIL;
    m = dev->Combined();
    for (i = 0; i < n; i++) {
        const u8 *in = src->data + (srcIndex + i) * src->layout.stride;
        u8 *out = data + (dstIndex + i) * layout.stride;
        Gl_Transform(m, (const float *)in, dev->Width(), dev->Height(), (float *)out);
        if (!(flags & RD_PV_DONOTCOPYDATA)) {
            if (layout.diffuse >= 0 && src->layout.diffuse >= 0)
                memcpy(out + layout.diffuse, in + src->layout.diffuse, 4);
            if (layout.specular >= 0 && src->layout.specular >= 0)
                memcpy(out + layout.specular, in + src->layout.specular, 4);
            if (layout.tex >= 0 && src->layout.tex >= 0)
                memcpy(out + layout.tex, in + src->layout.tex, 8);
        }
    }
    return 0;
}

/* ---- draws ---- */
GlVertex *GlRenderDevice::AllocVertices(u32 n)
{
    if (vertexCount + n > vertexCapacity) {
        u32 capacity = vertexCapacity ? vertexCapacity : 16384;
        GlVertex *grown;
        while (capacity < vertexCount + n)
            capacity *= 2;
        grown = (GlVertex *)realloc(vertices, capacity * sizeof(GlVertex));
        if (!grown)
            return 0;
        vertices = grown;
        vertexCapacity = capacity;
    }
    return vertices + vertexCount;
}

long GlRenderDevice::DrawPrimitive(u32 primitive, u32 fvf, void *data, u32 count, u32 flags)
{
    GlFvf layout;
    GlVertex *out;
    const u8 *in = (const u8 *)data;
    u32 i, kept = 0;
    int lines = primitive == RD_PRIM_LINELIST, zTest, zWrite, blend;
    GlDraw *last;
    GlTexture *texture;
    GlPsParams ps;
    (void)flags;

    if (!lines && primitive != RD_PRIM_TRIANGLELIST)
        return GL_E_FAIL; /* the game draws nothing else */
    count -= count % (lines ? 2 : 3);
    if (!count)
        return 0;
    Gl_ParseFvf(fvf, &layout);
    if (!(out = AllocVertices(count)))
        return GL_E_FAIL;
    for (i = 0; i < count; i++, in += layout.stride) {
        GlVertex *v = out + kept + i % (lines ? 2 : 3);
        if (layout.transformed)
            memcpy(&v->x, in, 16);
        else
            Gl_Transform(Combined(), (const float *)in, (float)width, (float)height, &v->x);
        v->diffuse = layout.diffuse >= 0 ? *(const u32 *)(in + layout.diffuse) : 0xffffffff;
        v->specular = layout.specular >= 0 ? *(const u32 *)(in + layout.specular) : 0xff000000;
        if (layout.tex >= 0) {
            v->u = ((const float *)(in + layout.tex))[0];
            v->v = ((const float *)(in + layout.tex))[1];
        } else {
            v->u = v->v = 0.0f;
        }
        if (lines) {
            if (i % 2 == 1)
                kept += 2;
        } else if (i % 3 == 2) {
            /* Direct3D's culling: the winding on the screen (y down), CW = positive area; a triangle reaching behind
             * the eye (rhw <= 0) has no meaningful screen winding and is left to the clipper */
            GlVertex *t = out + kept;
            float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
            int behind = t[0].rhw <= 0.0f || t[1].rhw <= 0.0f || t[2].rhw <= 0.0f;
            if (behind ||
                (!(state.cull == D3D_CULL_CW && area > 0.0f) && !(state.cull == D3D_CULL_CCW && area < 0.0f)))
                kept += 3;
        }
    }
    if (!kept)
        return 0;

    zTest = state.zEnable;
    zWrite = state.zEnable && state.zWrite;
    blend = state.blend;
    texture = state.texture ? state.texture : &white;
    memset(&ps, 0, sizeof(ps));
    ps.fogColor[0] = ((state.fogColor >> 16) & 0xff) / 255.0f;
    ps.fogColor[1] = ((state.fogColor >> 8) & 0xff) / 255.0f;
    ps.fogColor[2] = (state.fogColor & 0xff) / 255.0f;
    ps.fogColor[3] = 1.0f;
    ps.mode[0] = (float)state.textured;
    ps.mode[1] = state.alphaTest ? 1.0f : 0.0f;
    ps.mode[2] = state.fog ? 1.0f : 0.0f;
    ps.mode[3] = state.specular ? 1.0f : 0.0f;
    if (!state.textured)
        texture = &white; /* not sampled, but bound */
    MarkUsed(texture);

    /* the same state as the draw before: one draw */
    last = drawCount ? &draws[drawCount - 1] : 0;
    if (last && last->lines == lines && last->zTest == zTest && last->zWrite == zWrite && last->blend == blend &&
        (!blend || (last->srcBlend == state.srcBlend && last->dstBlend == state.dstBlend)) &&
        last->texture == texture && last->linear == state.linear && !memcmp(&last->ps, &ps, sizeof(ps)) &&
        last->firstVertex + last->vertexCount == vertexCount) {
        last->vertexCount += kept;
    } else {
        if (drawCount == drawCapacity) {
            u32 capacity = drawCapacity ? drawCapacity * 2 : 1024;
            GlDraw *grown = (GlDraw *)realloc(draws, capacity * sizeof(GlDraw));
            if (!grown)
                return GL_E_FAIL;
            draws = grown;
            drawCapacity = capacity;
        }
        last = &draws[drawCount++];
        last->lines = lines;
        last->zTest = zTest;
        last->zWrite = zWrite;
        last->blend = blend;
        last->srcBlend = state.srcBlend;
        last->dstBlend = state.dstBlend;
        last->texture = texture;
        last->linear = state.linear;
        last->ps = ps;
        last->firstVertex = vertexCount;
        last->vertexCount = kept;
    }
    vertexCount += kept;
    return 0;
}

long GlRenderDevice::Clear(u32 rectCount, void *rects, u32 flags, u32 color, float z, u32 stencil)
{
    (void)rectCount; /* the game always clears the whole target */
    (void)rects;
    (void)stencil;
    if (drawCount)
        Flush(); /* what was drawn before goes under the clear */
    if (flags & RD_CLEAR_TARGET) {
        clearColor = 1;
        clearColorValue[0] = ((color >> 16) & 0xff) / 255.0f;
        clearColorValue[1] = ((color >> 8) & 0xff) / 255.0f;
        clearColorValue[2] = (color & 0xff) / 255.0f;
        clearColorValue[3] = 1.0f;
    }
    if (flags & RD_CLEAR_ZBUFFER) {
        clearDepth = 1;
        clearDepthValue = z;
    }
    return 0;
}

/* the texels into the texture, in GL's order (Gl_ConvertTexels) */
void GlRenderDevice::Upload(GlTexture *t)
{
    GLenum internal, external, type;
    const void *texels;
    u32 n = t->width * t->height;
    if (Gl_TexelBytes(t->format) == 2 && t->format != TEXFMT_RGB565 && n > scratchTexels) {
        u16 *grown = (u16 *)realloc(scratch, (size_t)n * 2);
        if (!grown)
            return;
        scratch = grown;
        scratchTexels = n;
    }
    texels = Gl_ConvertTexels(t, scratch);
    Gl_TextureFormat(t->format, &internal, &external, &type);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->width, t->height, external, type, texels);
    t->dirty = 0;
}

/* Draws the batch into the frame: the changed textures and the vertices are uploaded, then the pending clears and the
 * draws run in order. */
void GlRenderDevice::Flush()
{
    u32 i;
    int boundZTest = -1, boundZWrite = -1, boundBlend = -1, boundLinear = -1;
    u32 boundSrc = 0, boundDst = 0;
    GlTexture *boundTexture = 0;
    const GlPsParams *boundPs = 0;

    for (i = 0; i < textureCount; i++)
        if (textures[i]->dirty)
            Upload(textures[i]);
    if (!drawCount && !clearColor && !clearDepth)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, sampleCount > 1 ? msaaFbo : frameFbos[current]);
    glViewport(0, 0, gpuWidth, gpuHeight);
    if (clearColor || clearDepth) {
        GLbitfield mask = 0;
        glDisable(GL_SCISSOR_TEST);
        if (clearColor) {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(clearColorValue[0], clearColorValue[1], clearColorValue[2], clearColorValue[3]);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (clearDepth) {
            glDepthMask(GL_TRUE);
            glClearDepthf(clearDepthValue);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        glClear(mask);
    }

    if (drawCount) {
        u32 bytes = vertexCount * sizeof(GlVertex);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        /* a fresh store every time (orphaning): the draws of the previous batch may still be reading the old one */
        if (bytes > vboSize)
            vboSize = bytes + bytes / 2;
        glBufferData(GL_ARRAY_BUFFER, vboSize, 0, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, vertices);
        glUniform2f(uScreen, 2.0f / width, 2.0f / height);
        /* the frame's alpha stays 1, as the original's 16-bit back buffer had none (DESTALPHA is then 1) */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glActiveTexture(GL_TEXTURE0);
    }
    for (i = 0; i < drawCount; i++) {
        const GlDraw *d = &draws[i];
        if (d->zTest != boundZTest) {
            if (d->zTest)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            boundZTest = d->zTest;
        }
        if (d->zWrite != boundZWrite) {
            glDepthMask(d->zWrite ? GL_TRUE : GL_FALSE);
            boundZWrite = d->zWrite;
        }
        if (d->blend != boundBlend) {
            if (d->blend)
                glEnable(GL_BLEND);
            else
                glDisable(GL_BLEND);
            boundBlend = d->blend;
        }
        if (d->blend && (d->srcBlend != boundSrc || d->dstBlend != boundDst)) {
            glBlendFunc(Gl_BlendFactor(d->srcBlend), Gl_BlendFactor(d->dstBlend));
            boundSrc = d->srcBlend;
            boundDst = d->dstBlend;
        }
        if (d->texture != boundTexture) {
            glBindTexture(GL_TEXTURE_2D, d->texture->gl);
            boundTexture = d->texture;
        }
        if (d->linear != boundLinear) {
            glBindSampler(0, (d->linear && g_textureFilteringEnabled) ? linearSampler : pointSampler);
            boundLinear = d->linear;
        }
        if (!boundPs || memcmp(boundPs, &d->ps, sizeof(d->ps))) {
            glUniform4fv(uFogColor, 1, d->ps.fogColor);
            glUniform4fv(uMode, 1, d->ps.mode);
            boundPs = &d->ps;
        }
        glDrawArrays(d->lines ? GL_LINES : GL_TRIANGLES, (GLint)d->firstVertex, (GLsizei)d->vertexCount);
    }
    if (sampleCount > 1)
        needsResolve = 1;
    frameDraws += drawCount;
    frameVertices += vertexCount;
    vertexCount = drawCount = 0;
    clearColor = clearDepth = 0;
    BeginBatch();
}

void GlRenderDevice::Resolve()
{
    if (!needsResolve)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameFbos[current]);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, gpuWidth, gpuHeight, 0, 0, gpuWidth, gpuHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    needsResolve = 0;
}

/* the frame onto the screen (scaled, aspect kept), shown; then the other frame is drawn into (a flip) */
long GlRenderDevice::Present()
{
    float scale;
    u32 w, h, x, y;
    static u32 s_presented;

    /* the first frames' work in the log: a black screen with draws is the display, without them the game */
    if (++s_presented <= 5 || s_presented == 60 || s_presented == 600) {
        GLenum error = glGetError();
        Platform_Log("SheepD3D: frame %u: %u draws, %u vertices, GL error 0x%x", (unsigned)s_presented,
                     (unsigned)(frameDraws + drawCount), (unsigned)(frameVertices + vertexCount), (unsigned)error);
    }
    PerfOverlay_PresentBegin(frameDraws + drawCount, frameVertices + vertexCount);
    frameDraws = frameVertices = 0;
    if (PerfOverlay_Enabled())
        DrawPerfOverlay();
    Flush();
    frameDraws = frameVertices = 0; /* the overlay's own draws are not counted */
    Resolve();

    /* the screen's part of the window: its bottom-left corner in GL's terms (the crop shows the top-left of the
     * buffer, which is GL's bottom rows) */
    scale = GL_MIN((float)displayWidth / width, (float)displayHeight / height);
    w = (u32)(width * scale);
    h = (u32)(height * scale);
    x = (displayWidth - w) / 2;
    y = (displayHeight - h) / 2;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, GL_WINDOW_WIDTH, GL_WINDOW_HEIGHT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT); /* black bars */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, frameFbos[current]);
    /* rendered below the screen's size (screen percent < 100): nearest, so the pixels stay sharp */
    glBlitFramebuffer(0, 0, gpuWidth, gpuHeight, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT,
                      (gpuWidth == w && gpuHeight == h) || appliedScale < 100 ? GL_NEAREST : GL_LINEAR);
    Switch_SwapBuffers();
    current ^= 1;
    PerfOverlay_PresentEnd();
    ApplyRenderSettings();
    return 0;
}

/* The performance overlay (src/render/perf_overlay.h): its lines in the top-left corner, white on a translucent black
 * box, drawn last into the frame with ordinary pre-transformed draws (and the game's state put back after). */
void GlRenderDevice::DrawPerfOverlay()
{
    struct OverlayVertex {
        float x, y, z, rhw;
        u32 diffuse;
        float u, v;
    };
    static OverlayVertex quads[PERF_OVERLAY_MAX_LINES * PERF_OVERLAY_LINE_SIZE * 6];
    char lines[PERF_OVERLAY_MAX_LINES][PERF_OVERLAY_LINE_SIZE];
    const u32 fvf = RD_FVF_XYZRHW | RD_FVF_DIFFUSE | RD_FVF_TEX1;
    const int cellW = PERF_FONT_WIDTH + 1, cellH = PERF_FONT_HEIGHT + 3; /* a glyph and its spacing, in font pixels */
    int count = PerfOverlay_Lines(lines), longest = 0, glyphs = PerfOverlay_GlyphCount(), i, n = 0;
    float scale = (float)GL_MAX(1u, height / 360), margin = 4.0f * scale, pad = 3.0f * scale;
    float atlasW = (float)(glyphs * cellW);
    GlState saved = state;

    if (!count)
        return;
    if (!font.gl) {
        /* the atlas: every glyph side by side in one row, white, its alpha the glyph (4444) */
        int g, row, col;
        font.width = (u32)(glyphs * cellW);
        font.height = PERF_FONT_HEIGHT + 1;
        font.format = TEXFMT_ARGB4444;
        font.pixels = (u16 *)calloc(font.width * font.height, 2);
        if (!font.pixels)
            return;
        for (g = 0; g < glyphs; g++)
            for (row = 0; row < PERF_FONT_HEIGHT; row++)
                for (col = 0; col < PERF_FONT_WIDTH; col++)
                    if (PerfOverlay_GlyphRows(g)[row] & (0x10 >> col))
                        font.pixels[row * font.width + g * cellW + col] = 0xffff;
        glGenTextures(1, &font.gl);
        glBindTexture(GL_TEXTURE_2D, font.gl);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA4, font.width, font.height);
        font.dirty = 1;
        TrackTexture(&font);
    }
    for (i = 0; i < count; i++)
        longest = GL_MAX(longest, (int)strlen(lines[i]));

    /* the box: untextured, the diffuse alpha blends it */
    state.blend = 1;
    state.srcBlend = D3D_BLEND_SRCALPHA;
    state.dstBlend = D3D_BLEND_INVSRCALPHA;
    state.alphaTest = state.zEnable = state.zWrite = state.specular = state.fog = state.linear = 0;
    state.cull = D3D_CULL_NONE;
    state.textured = 0;
    state.texture = 0;
    {
        float x0 = margin, y0 = margin;
        float x1 = x0 + 2 * pad + (longest * cellW - 1) * scale, y1 = y0 + 2 * pad + (count * cellH - 3) * scale;
        OverlayVertex box[6] = {{x0, y0, 0, 1, 0xa0000000, 0, 0}, {x1, y0, 0, 1, 0xa0000000, 0, 0},
                                {x0, y1, 0, 1, 0xa0000000, 0, 0}, {x1, y0, 0, 1, 0xa0000000, 0, 0},
                                {x1, y1, 0, 1, 0xa0000000, 0, 0}, {x0, y1, 0, 1, 0xa0000000, 0, 0}};
        DrawPrimitive(RD_PRIM_TRIANGLELIST, fvf, box, 6, 0);
    }

    /* the text: one quad per glyph, the texture's alpha cuts it out */
    for (i = 0; i < count; i++) {
        const char *c;
        float x = margin + pad, y = margin + pad + i * cellH * scale;
        for (c = lines[i]; *c; c++, x += cellW * scale) {
            int g = PerfOverlay_GlyphIndex(*c);
            float x1 = x + PERF_FONT_WIDTH * scale, y1 = y + PERF_FONT_HEIGHT * scale;
            float u0 = g * cellW / atlasW, u1 = (g * cellW + PERF_FONT_WIDTH) / atlasW;
            float v1 = (float)PERF_FONT_HEIGHT / font.height;
            OverlayVertex *q = quads + n;
            if (g < 0)
                continue;
            q[0] = {x, y, 0, 1, 0xffffffff, u0, 0};
            q[1] = {x1, y, 0, 1, 0xffffffff, u1, 0};
            q[2] = {x, y1, 0, 1, 0xffffffff, u0, v1};
            q[3] = {x1, y, 0, 1, 0xffffffff, u1, 0};
            q[4] = {x1, y1, 0, 1, 0xffffffff, u1, v1};
            q[5] = {x, y1, 0, 1, 0xffffffff, u0, v1};
            n += 6;
        }
    }
    state.textured = 1;
    state.texture = &font;
    if (n)
        DrawPrimitive(RD_PRIM_TRIANGLELIST, fvf, quads, (u32)n, 0);
    state = saved;
}

/* ---- textures ---- */
s32 GlRenderDevice::CreateTexture(u32 w, u32 h, u32 format, RdTexture **out, u32 *outWidth, u32 *outHeight)
{
    GlTexture *t;
    GLenum internal, external, type;
    *out = 0;
    if (format != TEXFMT_RGB565 && format != TEXFMT_ARGB1555 && format != TEXFMT_ARGB4444 && format != TEXFMT_RGBA8)
        return TEXRES_NO_PIXEL_FORMAT;
    if (!w)
        w = 1;
    if (!h)
        h = 1;
    *outWidth = w;
    *outHeight = h;
    t = (GlTexture *)calloc(1, sizeof(GlTexture));
    if (!t || !(t->pixels = (u16 *)calloc(w * h, Gl_TexelBytes(format)))) {
        free(t);
        return TEXRES_OUT_OF_MEMORY;
    }
    Gl_TextureFormat(format, &internal, &external, &type);
    glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
    if (glGetError() != GL_NO_ERROR || !TrackTexture(t)) {
        glDeleteTextures(1, &t->gl);
        free(t->pixels);
        free(t);
        return TEXRES_OUT_OF_MEMORY;
    }
    t->width = w;
    t->height = h;
    t->format = format;
    t->dirty = 1;
    *out = (RdTexture *)t;
    return TEXRES_OK;
}

long GlRenderDevice::LockTexture(RdTexture *texture, u32 access, RdLockedRect *out)
{
    GlTexture *t = (GlTexture *)texture;
    if (!t || !t->pixels)
        return GL_E_FAIL; /* a frame capture lives on the GPU only */
    if (access & RD_TEXLOCK_WRITE) {
        WaitUntilUnused(t); /* the batch drew with the texels as they are now */
        t->lockedToWrite = 1;
    }
    out->pixels = t->pixels;
    out->pitch = (s32)(t->width * Gl_TexelBytes(t->format));
    out->width = t->width;
    out->height = t->height;
    return 0;
}

long GlRenderDevice::UnlockTexture(RdTexture *texture)
{
    GlTexture *t = (GlTexture *)texture;
    if (t && t->lockedToWrite) {
        t->lockedToWrite = 0;
        t->dirty = 1;
    }
    return 0;
}

long GlRenderDevice::CopyTexture(RdTexture *dstTexture, u32 x, u32 y, RdTexture *srcTexture, RdRect *srcRect)
{
    GlTexture *dst = (GlTexture *)dstTexture, *src = (GlTexture *)srcTexture;
    RdRect r;
    s32 row;
    if (!dst || !src || !dst->pixels || !src->pixels || Gl_TexelBytes(dst->format) != Gl_TexelBytes(src->format))
        return GL_E_FAIL;
    if (srcRect)
        r = *srcRect;
    else {
        r.left = r.top = 0;
        r.right = (s32)src->width;
        r.bottom = (s32)src->height;
    }
    if (r.left < 0 || r.top < 0 || r.right > (s32)src->width || r.bottom > (s32)src->height || r.right <= r.left ||
        r.bottom <= r.top || x + (u32)(r.right - r.left) > dst->width || y + (u32)(r.bottom - r.top) > dst->height)
        return GL_E_FAIL; /* BltFast's DDERR_INVALIDRECT */
    WaitUntilUnused(dst);
    {
        u32 bytes = Gl_TexelBytes(src->format);
        u8 *to = (u8 *)dst->pixels, *from = (u8 *)src->pixels;
        for (row = r.top; row < r.bottom; row++)
            memmove(to + ((y + (u32)(row - r.top)) * dst->width + x) * bytes,
                    from + ((u32)row * src->width + (u32)r.left) * bytes, (size_t)(r.right - r.left) * bytes);
    }
    dst->dirty = 1;
    return 0;
}

void GlRenderDevice::ReleaseTexture(RdTexture *texture)
{
    GlTexture *t = (GlTexture *)texture;
    if (!t)
        return;
    WaitUntilUnused(t); /* the recorded draws still use it */
    if (state.texture == t)
        state.texture = 0;
    UntrackTexture(t);
    if (t->fbo)
        glDeleteFramebuffers(1, &t->fbo);
    glDeleteTextures(1, &t->gl); /* GL keeps it until the submitted commands are done with it */
    free(t->pixels);
    free(t);
}

long GlRenderDevice::CreateFrameCaptureTexture(u32 w, u32 h, RdTexture **out, u32 *outWidth, u32 *outHeight)
{
    GlTexture *t;
    *out = 0;
    if (!w || !h || w > width || h > height)
        return GL_E_FAIL;
    if (!(t = (GlTexture *)calloc(1, sizeof(GlTexture))))
        return GL_E_FAIL;
    glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->gl);
        free(t);
        return GL_E_FAIL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t->width = *outWidth = w;
    t->height = *outHeight = h;
    *out = (RdTexture *)t;
    return 0;
}

/* The frame (shown or being drawn) into dstRect of a capture. The frames are upside down in GL's terms (row 0 at the
 * bottom) while the capture is sampled the game's way up (v = 0 at the top), so the blit reads the source bottom-up. */
long GlRenderDevice::CopyFrameToTexture(RdTexture *dstTexture, RdRect *dstRect, int fromShown)
{
    GlTexture *dst = (GlTexture *)dstTexture;
    u32 w, h, sw, sh;
    if (!dst || !dst->fbo || !dstRect)
        return GL_E_FAIL;
    WaitUntilUnused(dst);
    Flush(); /* the frame so far */
    if (!fromShown)
        Resolve(); /* the shown frame was resolved before it was shown */
    w = GL_MIN((u32)(dstRect->right - dstRect->left), GL_MIN(width, dst->width - (u32)dstRect->left));
    h = GL_MIN((u32)(dstRect->bottom - dstRect->top), GL_MIN(height, dst->height - (u32)dstRect->top));
    /* the frame's top-left w x h of the game's pixels, in the GPU frame's */
    sw = (u32)((unsigned long long)w * gpuWidth / width);
    sh = (u32)((unsigned long long)h * gpuHeight / height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, frameFbos[fromShown ? current ^ 1 : current]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, gpuHeight, sw, gpuHeight - sh, dstRect->left, dstRect->top, dstRect->left + w,
                      dstRect->top + h, GL_COLOR_BUFFER_BIT,
                      (sw == w && sh == h) || appliedScale < 100 ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

/* ---- the backend ---- */
class GlRenderBackend : public RenderBackend {
public:
    const char *Name() const { return "OpenGL ES"; }
    int PrepareDisplay(D3DApp *app)
    {
        (void)app;
        return Switch_InitGL();
    }
    void WindowSize(int *width, int *height) const
    {
        *width = GL_GAME_WIDTH;
        *height = GL_GAME_HEIGHT;
    }

    RenderDevice *CreateDevice(D3DApp *app, long *result)
    {
        GlRenderDevice *device;
        const u32 width = (u32)GL_GAME_WIDTH, height = (u32)GL_GAME_HEIGHT;

        *result = GL_E_FAIL;
        if (!Switch_InitGL())
            return 0;
        device = new GlRenderDevice(width, height);
        if (!device->Init()) {
            Platform_Log("SheepD3D: %s: could not be set up", device->Name());
            delete device;
            return 0;
        }
        Platform_Log("SheepD3D: using %s, GL %s", device->Name(), (const char *)glGetString(GL_VERSION));
        *result = D3DApp_StartWithoutDirectDraw(app, width, height);
        if (*result < 0) {
            delete device;
            return 0;
        }
        return device;
    }
};

RenderBackend *Gl_CreateRenderBackend()
{
    return new GlRenderBackend();
}
