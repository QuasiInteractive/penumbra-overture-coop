// syntax-check stub
#pragma once
#include <cstddef>
typedef long long ogg_int64_t; typedef unsigned int ogg_uint32_t; typedef int ogg_int32_t; typedef unsigned short ogg_uint16_t; typedef short ogg_int16_t;
typedef struct { unsigned char* header; long header_len; unsigned char* body; long body_len; } ogg_page;
typedef struct { unsigned char* data; int storage; int fill; int returned; int unsynced; int headerbytes; int bodybytes; } ogg_sync_state;
typedef struct { unsigned char* body_data; long body_storage; long body_fill; long body_returned; int* lacing_vals; ogg_int64_t* granule_vals; long lacing_storage; long lacing_fill; long lacing_packet; long lacing_returned; unsigned char header[282]; int header_fill; int e_o_s; int b_o_s; long serialno; long pageno; ogg_int64_t packetno; ogg_int64_t granulepos; } ogg_stream_state;
typedef struct { unsigned char* packet; long bytes; long b_o_s; long e_o_s; ogg_int64_t granulepos; ogg_int64_t packetno; } ogg_packet;
typedef struct { int y_width; int y_height; int y_stride; int uv_width; int uv_height; int uv_stride; unsigned char* y; unsigned char* u; unsigned char* v; } yuv_buffer;
typedef enum { OC_CS_UNSPECIFIED, OC_CS_ITU_REC_470M, OC_CS_ITU_REC_470BG, OC_CS_NSPACES } theora_colorspace;
typedef enum { OC_PF_420, OC_PF_RSVD, OC_PF_422, OC_PF_444 } theora_pixelformat;
typedef struct { ogg_uint32_t width, height, frame_width, frame_height, offset_x, offset_y, fps_numerator, fps_denominator, aspect_numerator, aspect_denominator; theora_colorspace colorspace; int target_bitrate; int quality; int quick_p; unsigned char version_major, version_minor, version_subminor; void* codec_setup; int dropframes_p; int keyframe_auto_p; ogg_uint32_t keyframe_frequency, keyframe_frequency_force, keyframe_data_target_bitrate; ogg_int32_t keyframe_auto_threshold; ogg_uint32_t keyframe_mindistance; ogg_int32_t noise_sensitivity; ogg_int32_t sharpness; theora_pixelformat pixelformat; } theora_info;
typedef struct { theora_info* i; ogg_int64_t granulepos; void* internal_encode; void* internal_decode; } theora_state;
typedef struct { char** user_comments; int* comment_lengths; int comments; char* vendor; } theora_comment;
#define OC_FAULT -1
#define OC_EINVAL -10
#define OC_BADHEADER -20
#define OC_NOTFORMAT -21
#define OC_VERSION -22
#define OC_IMPL -23
#define OC_BADPACKET -24
#define OC_NEWPACKET -25
#define OC_DUPFRAME 1
extern "C" {
int theora_decode_header(theora_info*, theora_comment*, ogg_packet*); int theora_decode_init(theora_state*, theora_info*); int theora_decode_packetin(theora_state*, ogg_packet*); int theora_decode_YUVout(theora_state*, yuv_buffer*); int theora_packet_isheader(ogg_packet*); int theora_packet_iskeyframe(ogg_packet*); double theora_granule_time(theora_state*, ogg_int64_t); ogg_int64_t theora_granule_frame(theora_state*, ogg_int64_t); void theora_info_init(theora_info*); void theora_info_clear(theora_info*); void theora_clear(theora_state*); void theora_comment_init(theora_comment*); void theora_comment_clear(theora_comment*);
int ogg_sync_init(ogg_sync_state*); int ogg_sync_clear(ogg_sync_state*); char* ogg_sync_buffer(ogg_sync_state*, long); int ogg_sync_wrote(ogg_sync_state*, long); int ogg_sync_pageout(ogg_sync_state*, ogg_page*); int ogg_sync_pagein(ogg_sync_state*, ogg_page*); int ogg_sync_reset(ogg_sync_state*);
int ogg_stream_init(ogg_stream_state*, int); int ogg_stream_clear(ogg_stream_state*); int ogg_stream_reset(ogg_stream_state*); int ogg_stream_pagein(ogg_stream_state*, ogg_page*); int ogg_stream_packetout(ogg_stream_state*, ogg_packet*); int ogg_stream_packetpeek(ogg_stream_state*, ogg_packet*); int ogg_page_bos(ogg_page*); int ogg_page_serialno(ogg_page*); ogg_int64_t ogg_page_granulepos(ogg_page*);
}
