#include "rtmp-client.h"
#include "librtmp/log.h"
#include "librtmp/rtmp.h"
#include "h264-parser.h"
#include "h264-util.h"
#include "sys/sock.h"
#include <assert.h>
#include <memory.h>
//#pragma comment(lib, "libmpeg.lib")
//#pragma comment(lib, "h264.lib")

#define N_URL 1024
#define N_STREAM 2
#define VIDEO_STREAM 0
#define AUDIO_STREAM 1

typedef struct _RTMPContext
{
	RTMP* rtmp;
	RTMPPacket pkt;
	char url[N_URL];

	void* streams[N_STREAM];
	size_t stream_bytes[N_STREAM];

	uint32_t capacity;
} RTMPContext;

static void rtmp_write_int32(uint8_t* p, uint32_t bytes)
{
	p[0] = (bytes >> 24) & 0xFF;
	p[1] = (bytes >> 16) & 0xFF;
	p[2] = (bytes >> 8) & 0xFF;
	p[3] = bytes & 0xFF;
}

void* rtmp_client_create(const char* url)
{
	int r;
	RTMPContext* ctx;

	ctx = (RTMPContext*)malloc(sizeof(RTMPContext));
	if (!ctx) return NULL;
	memset(ctx, 0, sizeof(RTMPContext));
	strlcpy(ctx->url, url, sizeof(ctx->url));
	
	RTMP_LogSetLevel(RTMP_LOGDEBUG);
	ctx->rtmp = RTMP_Alloc();
	if (!ctx->rtmp)
	{
		free(ctx);
		return NULL;
	}

	RTMP_Init(ctx->rtmp);

	r = RTMP_SetupURL(ctx->rtmp, ctx->url);
	if (1 != r)
	{
		RTMP_Free(ctx->rtmp);
		free(ctx);
		return NULL;
	}
	
	RTMP_EnableWrite(ctx->rtmp);

	return ctx;
}

void rtmp_client_destroy(void* p)
{
	RTMPContext* ctx;
	ctx = (RTMPContext*)p;

	if (ctx->rtmp)
	{
		RTMP_Close(ctx->rtmp);
		RTMP_Free(ctx->rtmp);
	}

	if (ctx->capacity > 0)
	{
		assert(!!ctx->pkt.m_body);
		RTMPPacket_Free(&ctx->pkt);
	}

	if (ctx->streams[AUDIO_STREAM])
		free(ctx->streams[AUDIO_STREAM]);
	if (ctx->streams[VIDEO_STREAM])
		free(ctx->streams[VIDEO_STREAM]);

	free(ctx);
}

static int rtmp_client_alloc(RTMPContext* ctx, uint32_t len)
{
	if (len > ctx->capacity)
	{
		RTMPPacket_Free(&ctx->pkt);

		if (!RTMPPacket_Alloc(&ctx->pkt, len))
			return ENOMEM;

		ctx->capacity = len;
	}

	RTMPPacket_Reset(&ctx->pkt);
	return 0;
}

static int rtmp_client_send_first(RTMPContext* ctx, const void* audio, unsigned int abytes, const void* video, unsigned int vbytes, unsigned int pts);

static int rtmp_client_send(RTMPContext* ctx, RTMPPacket* packet)
{
	if (!RTMP_IsConnected(ctx->rtmp))
	{
		if (!RTMP_Connect(ctx->rtmp, NULL))
			return -1;

		if (!RTMP_ConnectStream(ctx->rtmp, 0))
			return -1;

		if (!rtmp_client_send_first(ctx, ctx->streams[AUDIO_STREAM], ctx->stream_bytes[AUDIO_STREAM], ctx->streams[VIDEO_STREAM], ctx->stream_bytes[VIDEO_STREAM], packet->m_nTimeStamp))
			return -1;
	}

	packet->m_hasAbsTimestamp = TRUE;
	packet->m_nInfoField2 = ctx->rtmp->m_stream_id;
	return TRUE==RTMP_SendPacket(ctx->rtmp, packet, 0) ? 0 : -1;
}

int rtmp_client_set_header(void* param, const void* audio, unsigned int abytes, const void* video, unsigned int vbytes)
{
	RTMPContext* ctx = (RTMPContext*)param;
	ctx->streams[AUDIO_STREAM] = realloc(ctx->streams[AUDIO_STREAM], abytes + 1);
	ctx->streams[VIDEO_STREAM] = realloc(ctx->streams[VIDEO_STREAM], vbytes + 1);
	if (NULL == ctx->streams[AUDIO_STREAM] || NULL == ctx->streams[VIDEO_STREAM])
	{
		if(ctx->streams[AUDIO_STREAM])
			free(ctx->streams[AUDIO_STREAM]);
		if(ctx->streams[VIDEO_STREAM])
			free(ctx->streams[VIDEO_STREAM]);
		return ENOMEM;
	}

	if(abytes > 0)
		memcpy(ctx->streams[AUDIO_STREAM], audio, abytes);
	if(vbytes > 0)
		memcpy(ctx->streams[VIDEO_STREAM], video, vbytes);
	ctx->stream_bytes[AUDIO_STREAM] = abytes;
	ctx->stream_bytes[VIDEO_STREAM] = vbytes;
	return 0;
}

static void rtmp_client_video_handler(void* param, const unsigned char* nalu, unsigned int bytes)
{
	RTMPContext* ctx = (RTMPContext*)param;
	int type = nalu[0] & 0x1f;

	if (H264_NALU_SPS == type || H264_NALU_PPS == type /*|| H264_NALU_SPS_EXTENSION == type || H264_NALU_SPS_SUBSET == type*/)
	{
		// filter sps/pps
	}
	else
	{
		rtmp_write_int32((uint8_t*)ctx->pkt.m_body + ctx->pkt.m_nBodySize, bytes);
		memcpy(ctx->pkt.m_body + ctx->pkt.m_nBodySize + 4, nalu, bytes);
		ctx->pkt.m_nBodySize += 4 + bytes;

		if (H264_NALU_IDR == type)
			ctx->pkt.m_body[0] = 0x17; // AVC key frame
	}
}

static uint8_t s_abc[2 * 1024 * 1024];
int rtmp_client_send_video(void* p, const void* video, unsigned int len, unsigned int pts, unsigned int dts)
{
	uint32_t compositionTimeOffset;
	uint8_t *out;
	RTMPContext* ctx;
	ctx = (RTMPContext*)p;

	if (0 != rtmp_client_alloc(ctx, len + 32))
		return -1;

	out = (uint8_t*)ctx->pkt.m_body;
	out[0] = 0x27; // AVC inter frame

	ctx->pkt.m_nBodySize = 5;
	h264_nalu((const unsigned char*)video, len, rtmp_client_video_handler, ctx);
	if (0 == ctx->pkt.m_body[0])
		return -1; // don't have video data ???

	compositionTimeOffset = pts - dts;

	out[1] = 0x01; // AVC NALU
	out[2] = (compositionTimeOffset >> 16) & 0xFF; // composition time
	out[3] = (compositionTimeOffset >> 8) & 0xFF;
	out[4] = compositionTimeOffset & 0xFF;

	ctx->pkt.m_packetType = RTMP_PACKET_TYPE_VIDEO; // video
	ctx->pkt.m_nTimeStamp = pts;
	ctx->pkt.m_nChannel = 4;
	ctx->pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;

	return rtmp_client_send(ctx, &ctx->pkt);
}

int rtmp_client_send_audio(void* rtmp, const void* audio, unsigned int len, unsigned int pts, unsigned int dts)
{
	uint8_t *out;
	unsigned int aacHeaderLen = 0;
	RTMPContext* ctx = (RTMPContext*)rtmp;
	const uint8_t *aac = (const uint8_t *)audio;

	if (0 != rtmp_client_alloc(ctx, len + 2))
		return -1;

	out = (uint8_t *)ctx->pkt.m_body;
	out[0] = 0xAF; // AAC 44kHz 16-bits samples Streteo sound
	out[1] = 0x01; // AAC raw

	// check ADTS syncword
    if(0xFF == aac[0] && 0xF0 == aac[1]) {
        aacHeaderLen = (aac[1] & 0x01) ? 7 : 9; // ADTS Protection Absent
        if (len < aacHeaderLen) {
            printf("audio don't have ADTS header\n");
            return -1;
        }
    }

	memcpy(out + 2, aac + aacHeaderLen, len - aacHeaderLen);
	ctx->pkt.m_nBodySize = 2 + len - aacHeaderLen;
	ctx->pkt.m_packetType = RTMP_PACKET_TYPE_AUDIO; // audio
	ctx->pkt.m_nTimeStamp = pts;
	ctx->pkt.m_nChannel = 4;
	ctx->pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;

	return rtmp_client_send(ctx, &ctx->pkt);
}

void rtmp_client_getserver(void* rtmp, char ip[65])
{
	unsigned short port = 0;
	RTMPContext* ctx = (RTMPContext*)rtmp;
	socket_getpeername(ctx->rtmp->m_sb.sb_socket, ip, &port);
}

char* rtmp_metadata_create(char* out, size_t len, int width, int height, int hasAudio);
static int rtmp_client_send_meta(RTMPContext* ctx, int width, int height, int hasAudio, unsigned int pts)
{
	char* outend;
	if (0 != rtmp_client_alloc(ctx, 1024 * 8))
		return -1;

	outend = rtmp_metadata_create(ctx->pkt.m_body, ctx->capacity, width, height, hasAudio);

	ctx->pkt.m_packetType = RTMP_PACKET_TYPE_INFO; // metadata
	ctx->pkt.m_nBodySize = outend - ctx->pkt.m_body;
	ctx->pkt.m_nTimeStamp = pts;
	ctx->pkt.m_nChannel = 4;
	ctx->pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;

	return RTMP_SendPacket(ctx->rtmp, &ctx->pkt, 0);
}

static int rtmp_client_send_AVCDecoderConfigurationRecord(RTMPContext* ctx, const void* data, unsigned int bytes, unsigned int pts)
{
	uint8_t *out;

	if (0 != rtmp_client_alloc(ctx, bytes + 5))
		return -1;

	out = (uint8_t *)ctx->pkt.m_body;
	out[0] = 0x17; // AVC key frame
	out[1] = 0x00; // AVC sequence header
	out[2] = 0x00; // composition time
	out[3] = 0x00;
	out[4] = 0x00;
	memcpy(out + 5, data, bytes);

	ctx->pkt.m_nBodySize = 5 + bytes;
	ctx->pkt.m_packetType = RTMP_PACKET_TYPE_VIDEO; // video
	ctx->pkt.m_nTimeStamp = pts;
	ctx->pkt.m_nChannel = 4;
	ctx->pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;

	return RTMP_SendPacket(ctx->rtmp, &ctx->pkt, 0);
}

static int rtmp_client_send_AudioSpecificConfig(RTMPContext* ctx, const void* data, unsigned int bytes, unsigned int pts)
{
	uint8_t *out;

	if (0 != rtmp_client_alloc(ctx, bytes + 2))
		return -1;

	out = (uint8_t *)ctx->pkt.m_body;
	out[0] = 0xAF; // AAC 44kHz 16-bits samples Streteo sound
	out[1] = 0x00; // AAC sequence header
	memcpy(out + 2, data, bytes);

	ctx->pkt.m_nBodySize = 2 + bytes;
	ctx->pkt.m_packetType = RTMP_PACKET_TYPE_AUDIO;
	ctx->pkt.m_nTimeStamp = pts;
	ctx->pkt.m_nChannel = 4;
	ctx->pkt.m_headerType = RTMP_PACKET_SIZE_LARGE;

	return RTMP_SendPacket(ctx->rtmp, &ctx->pkt, 0);
}

static int rtmp_client_send_first(RTMPContext* ctx, const void* audio, unsigned int abytes, const void* video, unsigned int vbytes, unsigned int pts)
{
	int r = -1;
	struct h264_sps_t sps;
	const uint8_t* p = (const uint8_t*)video;

	// send first video packet(SPS/PPS only)
	memset(&sps, 0, sizeof(struct h264_sps_t));
	if (video && vbytes > 5 && (p[5] & 0x1F) > 0)
	{
		// get sps from AVCDecoderConfigurationRecord
		if (0 == h264_parse_sps(p + 8,  (p[6] << 8) | p[7], &sps))
		{
			r = rtmp_client_send_meta(ctx, (sps.pic_width_in_mbs_minus1 + 1) * 16, (sps.pic_height_in_map_units_minus1 + 1)*(2 - sps.frame_mbs_only_flag) * 16, (audio && abytes > 0) ? 1 : 0, pts);

			r = rtmp_client_send_AVCDecoderConfigurationRecord(ctx, video, vbytes, pts);
		}
	}

	// send first audio packet(AAC info only)
	if (audio && abytes > 0)
	{
		if(0 == sps.pic_width_in_mbs_minus1 && 0 == sps.pic_height_in_map_units_minus1)
			r = rtmp_client_send_meta(ctx, 0, 0, 1, pts); // audio only

		r = rtmp_client_send_AudioSpecificConfig(ctx, audio, abytes, pts);
	}

	return r;
}
