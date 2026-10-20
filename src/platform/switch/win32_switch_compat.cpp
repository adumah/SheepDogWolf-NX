/* win32_sdl_compat.cpp for the Nintendo Switch: the same Win32 names the game code calls (threads, mutexes, events,
 * the performance counter, message boxes), on SDL2 instead of SDL3. */
#include <SDL2/SDL.h>

#include "../platform.h"
#include "../../sdk/win32.h"
#include "../../sdk/d3d7.h"
#include "../../sdk/dinput.h"
#include "../../sdk/dsound.h"
#include "../../sdk/mmstream.h"

#include <string.h>

struct SdwPlatformHandle {
    enum Kind { Mutex, Event, Thread } kind;
    SDL_mutex *mutex;
    SDL_sem *semaphore;
    SDL_Thread *thread;
};

struct SdwThreadStart {
    LPTHREAD_START_ROUTINE proc;
    void *arg;
};

static int SDLCALL SdwThreadEntry(void *opaque)
{
    SdwThreadStart start = *(SdwThreadStart *)opaque;
    SDL_free(opaque);
    return (int)start.proc(start.arg);
}

/* no dialog to ask in: a question is answered No (the safe answer), the rest goes to Platform_ShowMessage */
extern "C" int MessageBoxA(HWND, const char *text, const char *caption, UINT type)
{
    if (type & MB_YESNO) {
        Platform_Log("%s: %s (answered No)", caption ? caption : "", text ? text : "");
        return 7;
    }
    Platform_ShowMessage(caption, text, (type & (MB_ICONHAND | MB_ICONEXCLAMATION)) != 0);
    return IDOK;
}

extern "C" void OutputDebugStringA(const char *text)
{
    SDL_Log("%s", text ? text : "");
}

extern "C" HRESULT CoInitialize(void *) { return S_OK; }
extern "C" void CoUninitialize(void) {}
extern "C" HRESULT CoCreateInstance(const GUID &, void *, DWORD, const GUID &, void **out)
{
    if (out)
        *out = 0;
    return E_NOTIMPL;
}

extern "C" BOOL QueryPerformanceCounter(s64 *count)
{
    *count = (s64)SDL_GetPerformanceCounter();
    return TRUE;
}

extern "C" BOOL QueryPerformanceFrequency(s64 *frequency)
{
    *frequency = (s64)SDL_GetPerformanceFrequency();
    return TRUE;
}

extern "C" HANDLE CreateMutexA(void *, BOOL initialOwner, const char *)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)SDL_calloc(1, sizeof(*handle));
    if (!handle)
        return 0;
    handle->kind = SdwPlatformHandle::Mutex;
    handle->mutex = SDL_CreateMutex();
    if (!handle->mutex) {
        SDL_free(handle);
        return 0;
    }
    if (initialOwner)
        SDL_LockMutex(handle->mutex);
    return handle;
}

extern "C" HANDLE CreateEventA(void *, BOOL, BOOL initialState, const char *)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)SDL_calloc(1, sizeof(*handle));
    if (!handle)
        return 0;
    handle->kind = SdwPlatformHandle::Event;
    handle->semaphore = SDL_CreateSemaphore(initialState ? 1 : 0);
    if (!handle->semaphore) {
        SDL_free(handle);
        return 0;
    }
    return handle;
}

extern "C" HANDLE CreateThread(void *, DWORD, LPTHREAD_START_ROUTINE proc, void *arg, DWORD, DWORD *threadId)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)SDL_calloc(1, sizeof(*handle));
    SdwThreadStart *start = (SdwThreadStart *)SDL_malloc(sizeof(*start));
    if (!handle || !start) {
        SDL_free(handle);
        SDL_free(start);
        return 0;
    }
    start->proc = proc;
    start->arg = arg;
    handle->kind = SdwPlatformHandle::Thread;
    handle->thread = SDL_CreateThread(SdwThreadEntry, "SheepD3D loader", start);
    if (!handle->thread) {
        SDL_free(start);
        SDL_free(handle);
        return 0;
    }
    if (threadId)
        *threadId = (DWORD)SDL_GetThreadID(handle->thread);
    return handle;
}

extern "C" BOOL ReleaseMutex(HANDLE opaque)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)opaque;
    if (!handle || handle->kind != SdwPlatformHandle::Mutex)
        return FALSE;
    SDL_UnlockMutex(handle->mutex);
    return TRUE;
}

/* A poll that found the object busy gives the core away. Horizon does not time-slice threads of the same priority on a
 * core: the game polls with a zero timeout in a loop until its stream loader thread is done (StreamPlayer's
 * `while (LoaderResult() == STREAM_LOAD_PENDING);`), and the loader, made on the same core at the same priority, would
 * never run - the game hangs on a black screen while the music's buffer loops. SDL_Delay(0) is svcSleepThread(0), the
 * kernel's yield; it returns at once when no other thread is waiting for the core. */
static DWORD Sdw_PollFailed()
{
    SDL_Delay(0);
    return (DWORD)0x102; /* WAIT_TIMEOUT */
}

/* SDL2's lock and wait calls answer 0 on success (SDL3's answer true) */
extern "C" DWORD WaitForSingleObject(HANDLE opaque, DWORD milliseconds)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)opaque;
    if (!handle)
        return (DWORD)-1;
    if (handle->kind == SdwPlatformHandle::Mutex) {
        if (milliseconds == INFINITE) {
            SDL_LockMutex(handle->mutex);
            return WAIT_OBJECT_0;
        }
        if (milliseconds == 0)
            return SDL_TryLockMutex(handle->mutex) == 0 ? WAIT_OBJECT_0 : Sdw_PollFailed();
        Uint32 until = SDL_GetTicks() + milliseconds;
        do {
            if (SDL_TryLockMutex(handle->mutex) == 0)
                return WAIT_OBJECT_0;
            SDL_Delay(1);
        } while (!SDL_TICKS_PASSED(SDL_GetTicks(), until));
        return (DWORD)0x102;
    }
    if (handle->kind == SdwPlatformHandle::Event) {
        int waited = milliseconds == INFINITE ? SDL_SemWait(handle->semaphore)
                                              : SDL_SemWaitTimeout(handle->semaphore, milliseconds);
        return waited == 0 ? WAIT_OBJECT_0 : milliseconds == 0 ? Sdw_PollFailed() : (DWORD)0x102;
    }
    if (handle->thread) {
        int status = 0;
        SDL_WaitThread(handle->thread, &status);
        handle->thread = 0;
    }
    return WAIT_OBJECT_0;
}

extern "C" BOOL CloseHandle(HANDLE opaque)
{
    SdwPlatformHandle *handle = (SdwPlatformHandle *)opaque;
    if (!handle)
        return FALSE;
    if (handle->kind == SdwPlatformHandle::Mutex)
        SDL_DestroyMutex(handle->mutex);
    else if (handle->kind == SdwPlatformHandle::Event)
        SDL_DestroySemaphore(handle->semaphore);
    else if (handle->thread)
        SDL_DetachThread(handle->thread);
    SDL_free(handle);
    return TRUE;
}

extern "C" BOOL SetRect(RECT *rect, int left, int top, int right, int bottom)
{
    if (!rect)
        return FALSE;
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
    return TRUE;
}

extern "C" int ShowCursor(BOOL show)
{
    static int s_displayCount; /* Win32's: the cursor shows while it is >= 0 (there is no cursor here) */
    s_displayCount += show ? 1 : -1;
    return s_displayCount;
}

extern "C" void Sleep(DWORD ms) { SDL_Delay(ms); }

extern "C" BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
extern "C" BOOL SetThreadPriority(HANDLE, int) { return TRUE; }

extern "C" const GUID CLSID_AMMultiMediaStream = {0x49c47ce5, 0x9ba4, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
extern "C" const GUID CLSID_AudioRender = {0xe30629d1, 0x27e5, 0x11ce, {0x87, 0x5d, 0x00, 0x60, 0x8c, 0xb7, 0x80, 0x66}};
extern "C" const GUID IID_IAMMultiMediaStream = {0xbebe595c, 0x9a6f, 0x11d0, {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
extern "C" const GUID IID_IBaseFilter = {0x56a86895, 0x0ad4, 0x11ce, {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0x77, 0x0}};
extern "C" const GUID IID_IDirectDrawMediaStream = {0xf4104fce, 0x9a70, 0x11d0, {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
extern "C" const GUID MSPID_PrimaryVideo = {0xa35ff56a, 0x9fda, 0x11d0, {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
extern "C" const GUID IID_IDirect3DTnLHalDevice = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
extern "C" const GUID IID_IDirectSoundNotify = {0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
