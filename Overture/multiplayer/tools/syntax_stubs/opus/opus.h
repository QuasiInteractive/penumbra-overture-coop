/* syntax-check stub only (Overture/multiplayer/tools/syntax_check.sh) —
   NOT the real libopus header. Minimal declarations for VoiceChat.cpp. */
#pragma once
#include <cstdint>
typedef int16_t opus_int16;
typedef int32_t opus_int32;
typedef uint32_t opus_uint32;
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
#define OPUS_OK 0
#define OPUS_BAD_ARG -1
#define OPUS_BUFFER_TOO_SMALL -2
#define OPUS_INTERNAL_ERROR -3
#define OPUS_INVALID_PACKET -4
#define OPUS_UNIMPLEMENTED -5
#define OPUS_INVALID_STATE -6
#define OPUS_ALLOC_FAIL -7
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_APPLICATION_AUDIO 2049
#define OPUS_APPLICATION_RESTRICTED_LOWDELAY 2051
#define OPUS_SIGNAL_VOICE 3001
#define OPUS_SIGNAL_MUSIC 3002
#define OPUS_BANDWIDTH_NARROWBAND 1101
#define OPUS_BANDWIDTH_MEDIUMBAND 1102
#define OPUS_BANDWIDTH_WIDEBAND 1103
#define OPUS_BANDWIDTH_SUPERWIDEBAND 1104
#define OPUS_BANDWIDTH_FULLBAND 1105
#define OPUS_AUTO -1000
#define OPUS_BITRATE_MAX -1
/* ctl requests are (request, args...) varargs in the real header */
#define OPUS_SET_BITRATE(x) 4002, (opus_int32)(x)
#define OPUS_SET_VBR(x) 4006, (opus_int32)(x)
#define OPUS_SET_COMPLEXITY(x) 4010, (opus_int32)(x)
#define OPUS_SET_INBAND_FEC(x) 4012, (opus_int32)(x)
#define OPUS_SET_DTX(x) 4016, (opus_int32)(x)
#define OPUS_SET_SIGNAL(x) 4024, (opus_int32)(x)
#define OPUS_SET_MAX_BANDWIDTH(x) 4004, (opus_int32)(x)
#define OPUS_SET_PACKET_LOSS_PERC(x) 4014, (opus_int32)(x)
#define OPUS_RESET_STATE 4028
extern "C" {
OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
opus_int32 opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);
OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *st);
int opus_decoder_ctl(OpusDecoder *st, int request, ...);
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec);
const char *opus_get_version_string(void);
const char *opus_strerror(int error);
}
