/*
 * 86Box     A hypervisor and IBM PC system emulator that specializes in
 *           running old operating systems and software designed for IBM
 *           PC systems and compatibles from 1981 through fairly recent
 *           system designs based on the PCI bus.
 *
 *           This file is part of the 86Box distribution.
 *
 *           Interface to the OpenAL sound processing library.
 *
 *
 *
 * Authors:  Sarah Walker, <https://pcem-emulator.co.uk/>
 *           Miran Grca, <mgrca8@gmail.com>
 *           RichardG, <richardg867@gmail.com>
 *
 *           Copyright 2008-2019 Sarah Walker.
 *           Copyright 2016-2019 Miran Grca.
 *           Copyright 2024-2025 RichardG.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#undef AL_API
#undef ALC_API
#define AL_LIBTYPE_STATIC
#define ALC_LIBTYPE_STATIC
#define HAVE_STDARG_H

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include <86box/86box.h>
#include <86box/midi.h>
#include <86box/sound.h>
#include <86box/plat_unused.h>

#define ENABLE_OPENAL_LOG 1
#ifdef ENABLE_OPENAL_LOG
int openal_do_log = ENABLE_OPENAL_LOG;

static void
openal_log(const char *fmt, ...)
{
    va_list ap;

    if (openal_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define openal_log(fmt, ...)
#endif

typedef struct {
    ALuint source;
    ALuint buffers[4];
    ALenum format;
    int    freq;
} al_source_t;

static int         initialized   = 0;
static ALCcontext *Context;
static ALCdevice  *Device;

static ALvoid
alutInit(UNUSED(ALint *argc), UNUSED(ALbyte **argv))
{
    /* Open device */
    Device = alcOpenDevice((ALCchar *) "");
    if (Device != NULL) {
        /* Create context(s) */
        Context = alcCreateContext(Device, NULL);
        if (Context != NULL) {
            /* Set active context */
            alcMakeContextCurrent(Context);
        }
    }
}

static ALvoid
alutExit(ALvoid)
{
    if (Context != NULL) {
        /* Disable context */
        alcMakeContextCurrent(NULL);

        /* Release context(s) */
        alcDestroyContext(Context);

        if (Device != NULL) {
            /* Close device */
            alcCloseDevice(Device);
        }
    }
}

void
sound_backend_close(void)
{
    if (!initialized)
        return;

    // TODO INDIVIDUAL SOURCE CLOSE
    /*alSourceStopv(sources, source);
    alDeleteSources(sources, source);

    if (sources == 4)
        alDeleteBuffers(4, buffers_midi);
    alDeleteBuffers(4, buffers_cd);
    alDeleteBuffers(4, buffers_music);
    alDeleteBuffers(4, buffers);*/

    alutExit();

    initialized = 0;
}

void
sound_backend_reset(void)
{
    if (initialized)
        return;

    alutInit(0, 0);
    atexit(sound_backend_close);

    initialized = 1;
}

void *
sound_backend_add_source(void)
{
    if (!initialized)
        fatal("OpenAL: Adding source without initializing first\n");

    al_source_t *source = (al_source_t *) calloc(1, sizeof(al_source_t));

    ALenum e = alGetError();
    alGenBuffers(sizeof(source->buffers) / sizeof(source->buffers[0]), source->buffers);
    if ((e = alGetError()))
        fatal("OpenAL: alGenBuffers %d failed (%04X)\n", (int) (sizeof(source->buffers) / sizeof(source->buffers[0])), (int) e);
    alGenSources(1, &source->source);
    if ((e = alGetError()))
        fatal("OpenAL: alGenSources failed (%04X)\n", (int) e);

    openal_log("OpenAL: Allocating source %d\n", (int) source->source);
    alSource3i(source->source, AL_POSITION, 0, 0, 0);
    alSource3i(source->source, AL_VELOCITY, 0, 0, 0);
    alSource3i(source->source, AL_DIRECTION, 0, 0, 0);
    alSourcei(source->source, AL_ROLLOFF_FACTOR, 0);
    alSourcei(source->source, AL_SOURCE_RELATIVE, AL_TRUE);

    return source;
}

static const ALenum formats[SOUND_MAX][8] = {
    [SOUND_U8] = {AL_FORMAT_MONO8, AL_FORMAT_STEREO8, AL_NONE, AL_FORMAT_QUAD8, AL_NONE, AL_FORMAT_51CHN8, AL_FORMAT_61CHN8, AL_FORMAT_71CHN8},
    [SOUND_S16] = {AL_FORMAT_MONO16, AL_FORMAT_STEREO16, AL_NONE, AL_FORMAT_QUAD16, AL_NONE, AL_FORMAT_51CHN16, AL_FORMAT_61CHN16, AL_FORMAT_71CHN16},
    [SOUND_MULAW] = {AL_FORMAT_MONO_MULAW_EXT, AL_FORMAT_STEREO_MULAW_EXT, AL_NONE, AL_FORMAT_QUAD_MULAW, AL_NONE, AL_FORMAT_51CHN_MULAW, AL_FORMAT_61CHN_MULAW, AL_FORMAT_71CHN_MULAW},
    [SOUND_ALAW] = {AL_FORMAT_MONO_ALAW_EXT, AL_FORMAT_STEREO_ALAW_EXT, AL_NONE, AL_NONE, AL_NONE, AL_NONE, AL_NONE, AL_NONE},
    [SOUND_IMA_ADPCM] = {AL_FORMAT_MONO_IMA4, AL_FORMAT_STEREO_IMA4, AL_NONE, AL_NONE, AL_NONE, AL_NONE, AL_NONE, AL_NONE},
};

int
sound_backend_set_format(void *priv, uint8_t format, uint8_t channels, uint32_t freq)
{
    al_source_t *source = (al_source_t *) priv;

    /* Block invalid formats. */
    if ((format >= (sizeof(formats) / sizeof(formats[0]))) || (channels < 1) || (channels > 8)) {
        openal_log("OpenAL: Invalid source %d fmt=%d ch=%d freq=%d\n", (int) source->source, (int) format, (int) channels, (int) freq);
        return 0;
    }

    /* Allow this source to be reused if it's already set to the requested format. */
    ALenum new_format = formats[format][channels - 1];
    if ((source->format == new_format) && (source->freq == freq)) {
        openal_log("OpenAL: Reusing source %d as fmt=%d ch=%d freq=%d\n", (int) source->source, (int) format, (int) channels, (int) freq);
        return 1;
    }

    /* Don't change the format of an active source. */
    ALint state;
    alGetSourcei(source->source, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING) {
        openal_log("OpenAL: Skipping source %d as it is playing\n", (int) source->source);
        return 0;
    }

    /* Checks passed, change the format. */
    openal_log("OpenAL: Setting source %d to fmt=%d ch=%d freq=%d\n", (int) source->source, (int) format, (int) channels, (int) freq);
    source->freq = freq;
    source->format = new_format;

    /* Requeue all buffers. */
    ALint processed;
    alGetSourcei(source->source, AL_BUFFERS_PROCESSED, &processed);
    ALuint buffers[processed];
    alSourceUnqueueBuffers(source->source, processed, buffers);
    static const uint8_t empty[16] = {0};
    for (int i = 0; i < (sizeof(source->buffers) / sizeof(source->buffers[0])); i++)
        alBufferData(source->buffers[i], source->format, &empty, sizeof(empty), source->freq);
    alSourceQueueBuffers(source->source, sizeof(source->buffers) / sizeof(source->buffers[0]), source->buffers);

    return source->format != AL_NONE;
}

void
sound_backend_buffer(void *priv, void *buf, uint32_t bytes)
{
    if (!initialized)
        return;

    al_source_t *source = (al_source_t *) priv;

    ALint state;
    alGetSourcei(source->source, AL_SOURCE_STATE, &state);

    ALint processed;
    alGetSourcei(source->source, AL_BUFFERS_PROCESSED, &processed);
    if (processed >= 1) {
        double gain = pow(10.0, (double) sound_gain / 20.0);
        alListenerf(AL_GAIN, gain);

        ALuint buffer;
        alSourceUnqueueBuffers(source->source, 1, &buffer);
        alBufferData(buffer, source->format, buf, bytes, source->freq);
        alSourceQueueBuffers(source->source, 1, &buffer);
    }

    if (state != AL_PLAYING)
        alSourcePlay(source->source);
}
