/* The game's .wav files read with SDL2 on the Nintendo Switch (platform.h, Platform_Wav*): wav_sdl.cpp with SDL2's
 * names - SDL_LoadWAV_RW for the RIFF/WAVE header and the 'data' chunk, SDL_AudioCVT for the encodings the mixer does
 * not play. See wav_sdl.cpp for why the game's WINMM mmio code is not used. */
#include <SDL2/SDL.h>

#include "../platform.h"

/* the game's path as this system can open it (crt_posix_compat.cpp) */
extern "C" void Sdw_ResolvePath(const char *path, char *out, size_t size);

static Uint32 Wav_ReadLE32(const Uint8 *p)
{
    return (Uint32)p[0] | (Uint32)p[1] << 8 | (Uint32)p[2] << 16 | (Uint32)p[3] << 24;
}

int Platform_WavLoad(const char *path, const void *memory, unsigned memoryBytes, PlatformWav *out)
{
    SDL_RWops *io;
    SDL_AudioSpec spec;
    Uint8 header[12];
    Uint8 *data = 0;
    Uint32 bytes = 0;
    Uint32 riffBytes = 0;
    const char *what = memory ? "(sound bank entry)" : path;

    SDL_zerop(out);
    if (memory) {
        io = SDL_RWFromConstMem(memory, (int)memoryBytes);
    } else {
        char native[1024];
        if (!path)
            return 0;
        Sdw_ResolvePath(path, native, sizeof(native));
        io = SDL_RWFromFile(native, "rb");
    }
    if (!io) {
        SDL_Log("SheepD3D: wav %s: %s", what, SDL_GetError());
        return 0;
    }

    /* the RIFF chunk's size field: StreamSound::ServiceNotify ends a one-shot stream when its play progress reaches it
     * (WaveFile.ckRiff.cksize, as mmioDescend filled it) */
    if (SDL_RWread(io, header, 1, sizeof(header)) == sizeof(header) && SDL_memcmp(header, "RIFF", 4) == 0 &&
        SDL_memcmp(header + 8, "WAVE", 4) == 0)
        riffBytes = Wav_ReadLE32(header + 4);
    if (SDL_RWseek(io, 0, RW_SEEK_SET) < 0) {
        SDL_Log("SheepD3D: wav %s: %s", what, SDL_GetError());
        SDL_RWclose(io);
        return 0;
    }

    if (!SDL_LoadWAV_RW(io, 1, &spec, &data, &bytes)) { /* closes io in every case */
        SDL_Log("SheepD3D: wav %s: %s", what, SDL_GetError());
        return 0;
    }

    /* The mixer (platform_audio_switch.cpp) plays 8-bit unsigned or 16-bit signed little-endian, mono or stereo: what
     * the game's PCM files are. Anything else SDL hands back is converted to 16-bit (stereo when it had more than two
     * channels). */
    if (!((spec.format == AUDIO_U8 || spec.format == AUDIO_S16LSB) && (spec.channels == 1 || spec.channels == 2))) {
        SDL_AudioCVT cvt;
        int channels = spec.channels > 2 ? 2 : spec.channels;
        Uint8 *converted;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16LSB, (Uint8)channels, spec.freq) <
                0 ||
            !(converted = (Uint8 *)SDL_malloc((size_t)bytes * (cvt.len_mult > 0 ? cvt.len_mult : 1)))) {
            SDL_Log("SheepD3D: wav %s: %s", what, SDL_GetError());
            SDL_FreeWAV(data);
            return 0;
        }
        SDL_memcpy(converted, data, bytes);
        SDL_FreeWAV(data);
        cvt.buf = converted;
        cvt.len = (int)bytes;
        if (cvt.needed && SDL_ConvertAudio(&cvt) < 0) {
            SDL_Log("SheepD3D: wav %s: %s", what, SDL_GetError());
            SDL_free(converted);
            return 0;
        }
        data = converted;
        bytes = (Uint32)cvt.len_cvt;
        spec.format = AUDIO_S16LSB;
        spec.channels = (Uint8)channels;
        riffBytes = 0; /* not the file's samples any more: see below */
    } else {
        /* the same allocator for both cases, so that Platform_WavFree is SDL_free (SDL_FreeWAV is SDL_free in SDL2) */
    }

    /* the mixer needs at least one whole sample frame */
    bytes -= bytes % (Uint32)(spec.channels * (SDL_AUDIO_BITSIZE(spec.format) / 8));
    if (!bytes) {
        SDL_Log("SheepD3D: wav %s: no samples", what);
        SDL_free(data);
        return 0;
    }

    out->data = data;
    out->bytes = bytes;
    out->channels = spec.channels;
    out->bits = (int)SDL_AUDIO_BITSIZE(spec.format);
    out->rate = spec.freq;
    /* A PCM file's RIFF chunk holds its samples and at least 36 bytes of header. A smaller (or missing) size means the
     * samples are not the file's own (SDL decoded them): then the size is that of a canonical PCM file around them. */
    if (riffBytes < bytes + 36)
        riffBytes = bytes + 36;
    out->riffBytes = riffBytes;
    return 1;
}

void Platform_WavFree(void *data)
{
    SDL_free(data);
}
