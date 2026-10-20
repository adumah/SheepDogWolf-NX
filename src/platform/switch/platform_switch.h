/* What the Nintendo Switch's platform layer (platform_switch.cpp) gives the OpenGL ES renderer
 * (src/render/gl/gl_render_device.cpp) besides platform.h: the display, made with EGL on the console's default window.
 *
 * The settings: /switch/sheepdogwolf/environment.txt, NAME=value lines read at start-up into the environment ('#'
 * starts a comment):
 *   SDW_CPU_BOOST=MHz      the CPU clock while the game runs (default 1785; 0 leaves it alone)
 *   SDW_GPU_BOOST=MHz      the GPU clock while the game runs (default 0: left alone)
 *   SDW_SWAP_INTERVAL=N    vertical syncs per frame (default 1: 60 fps; 2: 30 fps)
 *   SDW_SWAP_AB=1          A is Cross and B is Circle (the labels), instead of the positions (B Cross, A Circle)
 *   SDW_PERF_OVERLAY=1     the performance overlay from the start (F3 on a USB keyboard toggles it)
 *   SDW_NXLINK=1           the log to nxlink's terminal too
 * Command-line flags (options.h) can be given the same way, as SDW_ARGS="--scale=75 --level=3". */
#ifndef SDW_PLATFORM_SWITCH_H
#define SDW_PLATFORM_SWITCH_H

/* the console is docked (the TV's 1920 x 1080) rather than handheld (1280 x 720) */
int Switch_IsDocked();
/* the EGL display, surface and OpenGL ES 3 context on the default window, made once; 0 when they cannot be */
int Switch_InitGL();
void Switch_ShutdownGL();
void Switch_SwapBuffers();
/* the compositor shows only the top-left width x height of the 1920 x 1080 window (handheld: 1280 x 720) */
void Switch_SetDisplayCrop(unsigned width, unsigned height);

#endif
