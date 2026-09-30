/* syntax-check stub only (Overture/multiplayer/tools/syntax_check.sh) —
   NOT the real OpenAL header. Minimal ALC + capture declarations. */
#pragma once
typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;
typedef char ALCboolean; typedef char ALCchar; typedef signed char ALCbyte; typedef unsigned char ALCubyte;
typedef short ALCshort; typedef unsigned short ALCushort; typedef int ALCint; typedef unsigned int ALCuint;
typedef int ALCsizei; typedef int ALCenum; typedef float ALCfloat; typedef double ALCdouble; typedef void ALCvoid;
#define ALC_FALSE 0
#define ALC_TRUE 1
#define ALC_FREQUENCY 0x1007
#define ALC_REFRESH 0x1008
#define ALC_SYNC 0x1009
#define ALC_MONO_SOURCES 0x1010
#define ALC_STEREO_SOURCES 0x1011
#define ALC_NO_ERROR 0
#define ALC_INVALID_DEVICE 0xA001
#define ALC_INVALID_CONTEXT 0xA002
#define ALC_INVALID_ENUM 0xA003
#define ALC_INVALID_VALUE 0xA004
#define ALC_OUT_OF_MEMORY 0xA005
#define ALC_DEFAULT_DEVICE_SPECIFIER 0x1004
#define ALC_DEVICE_SPECIFIER 0x1005
#define ALC_EXTENSIONS 0x1006
#define ALC_CAPTURE_DEVICE_SPECIFIER 0x310
#define ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER 0x311
#define ALC_CAPTURE_SAMPLES 0x312
extern "C" {
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist);
ALCboolean alcMakeContextCurrent(ALCcontext *context);
void alcDestroyContext(ALCcontext *context);
ALCcontext *alcGetCurrentContext(void);
ALCdevice *alcGetContextsDevice(ALCcontext *context);
ALCdevice *alcOpenDevice(const ALCchar *devicename);
ALCboolean alcCloseDevice(ALCdevice *device);
ALCenum alcGetError(ALCdevice *device);
ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname);
const ALCchar *alcGetString(ALCdevice *device, ALCenum param);
void alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values);
ALCdevice *alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize);
ALCboolean alcCaptureCloseDevice(ALCdevice *device);
void alcCaptureStart(ALCdevice *device);
void alcCaptureStop(ALCdevice *device);
void alcCaptureSamples(ALCdevice *device, ALCvoid *buffer, ALCsizei samples);
}
