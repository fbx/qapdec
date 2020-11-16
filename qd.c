#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/stat.h>

#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include <qap_defs.h>
#include <dolby_ms12.h>
#include <dts_m8.h>

#include "qd.h"

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

#define CASESTR(ENUM) case ENUM: return #ENUM;

#ifndef QAP_LIB_DTS_M8
# define QAP_LIB_DTS_M8 "libdts_m8_wrapper.so"
#endif

#ifndef QAP_LIB_DOLBY_MS12
# define QAP_LIB_DOLBY_MS12 "/usr/lib64/libdolby_ms12_wrapper_prod.so"
#endif

int qd_debug_level = 1;

struct qd_module_handle {
	qap_lib_handle_t handle;
};

typedef void (*qd_sw_decoder_func_t)(void *priv, qap_audio_buffer_t *buffer);

struct qd_sw_decoder {
	AVCodecContext *codec;
	qd_sw_decoder_func_t cb;
	void *cb_data;

	/* resampler */
	SwrContext *swr;
	int swr_in_format;
	int swr_out_format;
	uint64_t swr_in_channel_layout;
	uint64_t swr_out_channel_layout;
	void *swr_buffer;
	int swr_buffer_size;

	/* output pcm config */
	int out_format;
	int out_sample_rate;
	int out_channels;
	uint64_t out_channel_layout;
	qap_output_config_t out_config;
};

static struct qd_module_handle qd_modules[QD_MAX_MODULES];
static uint64_t qd_base_time;

#define WAV_SPEAKER_FRONT_LEFT			0x1
#define WAV_SPEAKER_FRONT_RIGHT			0x2
#define WAV_SPEAKER_FRONT_CENTER		0x4
#define WAV_SPEAKER_LOW_FREQUENCY		0x8
#define WAV_SPEAKER_BACK_LEFT			0x10
#define WAV_SPEAKER_BACK_RIGHT			0x20
#define WAV_SPEAKER_FRONT_LEFT_OF_CENTER	0x40
#define WAV_SPEAKER_FRONT_RIGHT_OF_CENTER	0x80
#define WAV_SPEAKER_BACK_CENTER			0x100
#define WAV_SPEAKER_SIDE_LEFT			0x200
#define WAV_SPEAKER_SIDE_RIGHT			0x400
#define WAV_SPEAKER_TOP_CENTER			0x800
#define WAV_SPEAKER_TOP_FRONT_LEFT		0x1000
#define WAV_SPEAKER_TOP_FRONT_CENTER		0x2000
#define WAV_SPEAKER_TOP_FRONT_RIGHT		0x4000
#define WAV_SPEAKER_TOP_BACK_LEFT		0x8000
#define WAV_SPEAKER_TOP_BACK_CENTER		0x10000
#define WAV_SPEAKER_TOP_BACK_RIGHT		0x20000
#define WAV_SPEAKER_INVALID			0

#define packed_struct struct __attribute__((packed))

packed_struct wav_header {
	uint8_t			riff_magic[4];
	uint32_t		riff_chunk_size;
	uint8_t			wave_magic[4];
	packed_struct {
		uint8_t		chunk_label[4];
		uint32_t	chunk_size;
		uint16_t	audio_format;
		uint16_t	num_channels;
		uint32_t	sample_rate;
		uint32_t	byte_rate;
		uint16_t	block_align;
		uint16_t	bits_per_sample;
		uint16_t	cb_size;
		packed_struct {
			uint16_t	valid_bits_per_sample;
			uint32_t	channel_mask;
			uint8_t		sub_format[16];
		} ext;
	} fmt;
	packed_struct {
		uint8_t		chunk_label[4];
		uint32_t	chunk_size;
	} data;
};

static inline uint64_t
get_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / UINT64_C(1000);
}

uint64_t
qd_get_time(void)
{
	return get_time() - qd_base_time;
}

uint64_t
qd_get_real_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / UINT64_C(1000);
}

static int
mkdir_parents(const char *path, mode_t mode)
{
	const char *p, *e;
	char *t;
	int ret;
	struct stat st;

	p = path + strspn(path, "/");

	while (1) {
		e = p + strcspn(p, "/");
		p = e + strspn(e, "/");

		if (*p == 0)
			return 0;

		if ((t = strndup(path, p - path)) == NULL) {
			errno = ENOMEM;
			return -1;
		}

		ret = mkdir(t, mode);

		if (!ret) {
			free(t);
			continue;
		}

		if (errno != EEXIST) {
			free(t);
			return -1;
		}

		if (lstat(t, &st) || !S_ISDIR(st.st_mode)) {
			free(t);
			errno = ENOTDIR;
			return -1;
		}

		free(t);
	}
}

static int
mkdir_p(const char *path, mode_t mode)
{
	int ret;
	struct stat st;

	if ((ret = mkdir_parents(path, mode)) < 0)
		return ret;

	if (!mkdir(path, mode))
		return 0;

	if (errno != EEXIST)
		return -1;

	if (lstat(path, &st) || !S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	return 0;
}

static const char *
audio_format_to_str(qap_audio_format_t format)
{
	switch (format) {
	CASESTR(QAP_AUDIO_FORMAT_PCM_16_BIT)
	CASESTR(QAP_AUDIO_FORMAT_PCM_8_24_BIT)
	CASESTR(QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED)
	CASESTR(QAP_AUDIO_FORMAT_PCM_32_BIT)
	CASESTR(QAP_AUDIO_FORMAT_AC3)
	CASESTR(QAP_AUDIO_FORMAT_AC4)
	CASESTR(QAP_AUDIO_FORMAT_EAC3)
	CASESTR(QAP_AUDIO_FORMAT_AAC)
	CASESTR(QAP_AUDIO_FORMAT_AAC_ADTS)
	CASESTR(QAP_AUDIO_FORMAT_MP2)
	CASESTR(QAP_AUDIO_FORMAT_MP3)
	CASESTR(QAP_AUDIO_FORMAT_FLAC)
	CASESTR(QAP_AUDIO_FORMAT_ALAC)
	CASESTR(QAP_AUDIO_FORMAT_APE)
	CASESTR(QAP_AUDIO_FORMAT_DTS)
	CASESTR(QAP_AUDIO_FORMAT_DTS_HD)
	}
	return "unknown";
}

static const char *
audio_profile_to_str(qap_audio_format_t format, uint32_t profile)
{
	if (format == QAP_AUDIO_FORMAT_AAC ||
	    format == QAP_AUDIO_FORMAT_AAC_ADTS) {
		switch (profile) {
		CASESTR(QAP_PROFILE_AAC_MAIN)
		CASESTR(QAP_PROFILE_AAC_LOW_COMPLEXITY)
		CASESTR(QAP_PROFILE_AAC_SSR)
		}

	} else if (format == QAP_AUDIO_FORMAT_DTS ||
		   format == QAP_AUDIO_FORMAT_DTS_HD) {
		switch (profile) {
		CASESTR(QAP_PROFILE_DTS_LEGACY)
		CASESTR(QAP_PROFILE_DTS_ES_MATRIX)
		CASESTR(QAP_PROFILE_DTS_ES_DISCRETE)
		CASESTR(QAP_PROFILE_DTS_9624)
		CASESTR(QAP_PROFILE_DTS_ES_8CH_DISCRETE)
		CASESTR(QAP_PROFILE_DTS_HIRES)
		CASESTR(QAP_PROFILE_DTS_MA)
		CASESTR(QAP_PROFILE_DTS_LBR)
		CASESTR(QAP_PROFILE_DTS_LOSSLESS)
		}
	}

	return "unknown";
}

static const char *
audio_format_extension(qap_audio_format_t format)
{
	switch (format) {
	case QAP_AUDIO_FORMAT_PCM_16_BIT:
	case QAP_AUDIO_FORMAT_PCM_8_24_BIT:
	case QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED:
	case QAP_AUDIO_FORMAT_PCM_32_BIT:
		return "wav";
	case QAP_AUDIO_FORMAT_AC3:
		return "ac3";
	case QAP_AUDIO_FORMAT_AC4:
		return "ac4";
	case QAP_AUDIO_FORMAT_EAC3:
		return "ec3";
	case QAP_AUDIO_FORMAT_AAC:
	case QAP_AUDIO_FORMAT_AAC_ADTS:
		return "aac";
	case QAP_AUDIO_FORMAT_MP2:
		return "mp2";
	case QAP_AUDIO_FORMAT_MP3:
		return "mp3";
	case QAP_AUDIO_FORMAT_FLAC:
		return "flac";
	case QAP_AUDIO_FORMAT_ALAC:
		return "alac";
	case QAP_AUDIO_FORMAT_APE:
		return "ape";
	case QAP_AUDIO_FORMAT_DTS:
	case QAP_AUDIO_FORMAT_DTS_HD:
		return "dts";
	}
	return "raw";
}

static const char *
audio_channel_to_str(qap_pcm_chmap channel)
{
	switch (channel) {
	case QAP_AUDIO_PCM_CHANNEL_L: return "L";
	case QAP_AUDIO_PCM_CHANNEL_R: return "R";
	case QAP_AUDIO_PCM_CHANNEL_C: return "C";
	case QAP_AUDIO_PCM_CHANNEL_LS: return "LS";
	case QAP_AUDIO_PCM_CHANNEL_RS: return "RS";
	case QAP_AUDIO_PCM_CHANNEL_LFE: return "LFE";
	case QAP_AUDIO_PCM_CHANNEL_CS: return "CS";
	case QAP_AUDIO_PCM_CHANNEL_LB: return "LB";
	case QAP_AUDIO_PCM_CHANNEL_RB: return "RB";
	case QAP_AUDIO_PCM_CHANNEL_TS: return "TS";
	case QAP_AUDIO_PCM_CHANNEL_CVH: return "CVH";
	case QAP_AUDIO_PCM_CHANNEL_MS: return "MS";
	case QAP_AUDIO_PCM_CHANNEL_FLC: return "FLC";
	case QAP_AUDIO_PCM_CHANNEL_FRC: return "FRC";
	case QAP_AUDIO_PCM_CHANNEL_RLC: return "RLC";
	case QAP_AUDIO_PCM_CHANNEL_RRC: return "RRC";
	case QAP_AUDIO_PCM_CHANNEL_LFE2: return "LFE2";
	case QAP_AUDIO_PCM_CHANNEL_SL: return "SL";
	case QAP_AUDIO_PCM_CHANNEL_SR: return "SR";
	case QAP_AUDIO_PCM_CHANNEL_TFL: return "TFL";
	case QAP_AUDIO_PCM_CHANNEL_TFR: return "TFR";
	case QAP_AUDIO_PCM_CHANNEL_TC: return "TC";
	case QAP_AUDIO_PCM_CHANNEL_TBL: return "TBL";
	case QAP_AUDIO_PCM_CHANNEL_TBR: return "TBR";
	case QAP_AUDIO_PCM_CHANNEL_TSL: return "TSL";
	case QAP_AUDIO_PCM_CHANNEL_TSR: return "TSR";
	case QAP_AUDIO_PCM_CHANNEL_TBC: return "TBC";
	case QAP_AUDIO_PCM_CHANNEL_BFC: return "BFC";
	case QAP_AUDIO_PCM_CHANNEL_BFL: return "BFL";
	case QAP_AUDIO_PCM_CHANNEL_BFR: return "BFR";
	case QAP_AUDIO_PCM_CHANNEL_LW: return "LW";
	case QAP_AUDIO_PCM_CHANNEL_RW: return "RW";
	case QAP_AUDIO_PCM_CHANNEL_LSD: return "LSD";
	case QAP_AUDIO_PCM_CHANNEL_RSD: return "RSD";
	}
	return "??";
}

static const char *
qd_input_id_to_str(enum qd_input_id id)
{
	switch (id) {
	case QD_INPUT_MAIN:
		return "MAIN";
	case QD_INPUT_MAIN2:
		return "MAIN2";
	case QD_INPUT_ASSOC:
		return "ASSOC";
	case QD_INPUT_SYS_SOUND:
		return "SYS_SOUND";
	case QD_INPUT_APP_SOUND:
		return "APP_SOUND";
	case QD_INPUT_OTT_SOUND:
		return "OTT_SOUND";
	case QD_INPUT_EXT_PCM:
		return "EXT_PCM";
	case QD_MAX_INPUTS:
		break;
	}

	return "UNKNOWN";
}

static const char *
qd_output_id_to_str(enum qd_output_id id)
{
	switch (id) {
	case QD_OUTPUT_STEREO:
		return "STEREO";
	case QD_OUTPUT_5DOT1:
		return "5DOT1";
	case QD_OUTPUT_7DOT1:
		return "7DOT1";
	case QD_OUTPUT_AC3:
		return "AC3";
	case QD_OUTPUT_EAC3:
		return "EAC3";
	case QD_OUTPUT_AC3_DECODED:
		return "AC3_DECODED";
	case QD_OUTPUT_EAC3_DECODED:
		return "EAC3_DECODED";
	case QD_OUTPUT_NONE:
	case QD_MAX_OUTPUTS:
		break;
	}

	return "UNKNOWN";
}

static const char *
audio_chmap_to_str(int channels, uint8_t *map)
{
	static char buf[256];
	int len = sizeof (buf);
	int offset = 0;

	for (int i = 0; i < channels && offset < len; i++) {
		int r = snprintf(buf + offset, len - offset, "%s%s",
				 audio_channel_to_str(map[i]),
				 i == channels  - 1 ? "" : ",");
		if (r > 0)
			offset += r;
	}

	buf[offset] = '\0';

	return buf;
}

static void
qd_sw_decoder_destroy(struct qd_sw_decoder *dec)
{
	if (!dec)
		return;

	avcodec_free_context(&dec->codec);
	swr_free(&dec->swr);
	free(dec->swr_buffer);
	free(dec);
}

static struct qd_sw_decoder *
qd_sw_decoder_create(qap_audio_format_t format)
{
	struct qd_sw_decoder *dec;
	enum AVCodecID avcodec_id;
	AVCodec *avcodec;

	switch (format) {
	case QAP_AUDIO_FORMAT_AC3:
		avcodec_id = AV_CODEC_ID_AC3;
		break;
	case QAP_AUDIO_FORMAT_EAC3:
		avcodec_id = AV_CODEC_ID_EAC3;
		break;
	default:
		return NULL;
	}

	avcodec = avcodec_find_decoder(avcodec_id);
	if (avcodec == NULL) {
		err("swdec: no decoder available for codec %s",
		    audio_format_to_str(format));
		return NULL;
	}

	dec = calloc(1, sizeof (*dec));
	if (!dec)
		return NULL;

	dec->codec = avcodec_alloc_context3(avcodec);
	if (!dec->codec) {
		err("swdec: failed to create %s decoder",
		    audio_format_to_str(format));
		goto fail;
	}

	if (avcodec_open2(dec->codec, avcodec, NULL)) {
		err("swdec: failed to open decoder");
		goto fail;
	}

	dec->swr = swr_alloc();
	if (!dec->swr) {
		err("swdec: failed to allocate resampler context");
		goto fail;
	}

	dec->codec->time_base.num = QD_SECOND;
	dec->codec->time_base.den = 1;

	dec->swr_out_format = AV_SAMPLE_FMT_NONE;
	dec->swr_in_format = AV_SAMPLE_FMT_NONE;
	dec->swr_in_channel_layout = 0;

	dec->out_format = AV_SAMPLE_FMT_NONE;

	return dec;

fail:
	qd_sw_decoder_destroy(dec);
	return NULL;
}

static void
qd_sw_decoder_set_callback(struct qd_sw_decoder *dec,
			   qd_sw_decoder_func_t func, void *userdata)
{
	dec->cb = func;
	dec->cb_data = userdata;
}

static uint8_t
convert_from_av_channel(uint64_t ch)
{
	switch (ch) {
	case AV_CH_STEREO_LEFT:
	case AV_CH_FRONT_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_L;
	case AV_CH_STEREO_RIGHT:
	case AV_CH_FRONT_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_R;
	case AV_CH_FRONT_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_C;
	case AV_CH_LOW_FREQUENCY:
		return QAP_AUDIO_PCM_CHANNEL_LFE;
	case AV_CH_BACK_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_LB;
	case AV_CH_BACK_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_RB;
	case AV_CH_FRONT_LEFT_OF_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_FLC;
	case AV_CH_FRONT_RIGHT_OF_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_FRC;
	case AV_CH_BACK_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_CB;
	case AV_CH_SIDE_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_LS;
	case AV_CH_SIDE_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_RS;
	case AV_CH_TOP_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_TC;
	case AV_CH_TOP_FRONT_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_TFL;
	case AV_CH_TOP_FRONT_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_TFC;
	case AV_CH_TOP_FRONT_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_TFR;
	case AV_CH_TOP_BACK_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_TBL;
	case AV_CH_TOP_BACK_CENTER:
		return QAP_AUDIO_PCM_CHANNEL_TBC;
	case AV_CH_TOP_BACK_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_TBR;
	case AV_CH_WIDE_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_LW;
	case AV_CH_WIDE_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_RW;
	case AV_CH_SURROUND_DIRECT_LEFT:
		return QAP_AUDIO_PCM_CHANNEL_LSD;
	case AV_CH_SURROUND_DIRECT_RIGHT:
		return QAP_AUDIO_PCM_CHANNEL_RSD;
	case AV_CH_LOW_FREQUENCY_2:
		return QAP_AUDIO_PCM_CHANNEL_LFE2;
	default:
		return -1;
	}
}

static int
qd_sw_decoder_process_frame(struct qd_sw_decoder *dec)
{
	AVFrame frame = {};
	qap_audio_buffer_t out;
	int size;
	int ret;

	ret = avcodec_receive_frame(dec->codec, &frame);
	if (ret == AVERROR(EAGAIN))
		return ret;

	if (ret != 0) {
		err("failed to read decoded audio: %s", av_err2str(ret));
		return ret;
	}

	/* generate output pcm config once, based on first decoded frame */
	if (dec->out_config.channels == 0) {
		/* ffmpeg config */
		dec->out_format = AV_SAMPLE_FMT_S16;
		dec->out_sample_rate = frame.sample_rate;
		dec->out_channels = frame.channels;
		dec->out_channel_layout = frame.channel_layout;

		/* same config in qap format */
		dec->out_config.format = QAP_AUDIO_FORMAT_PCM_16_BIT;
		dec->out_config.is_interleaved = true;
		dec->out_config.bit_width = 16;
		dec->out_config.sample_rate = frame.sample_rate;
		dec->out_config.channels = frame.channels;
		for (int i = 0; i < frame.channels; i++) {
			uint64_t ch = av_channel_layout_extract_channel(
				frame.channel_layout, i);
			dec->out_config.ch_map[i] = convert_from_av_channel(ch);
		}
	}

	/* reconfigure resampler if input or output format changes */
	if (dec->swr_in_format != frame.format ||
	    dec->swr_in_channel_layout != frame.channel_layout ||
	    dec->swr_out_format != dec->out_format ||
	    dec->swr_out_channel_layout != dec->out_channel_layout) {
		av_opt_set_int(dec->swr, "in_channel_layout",
			       frame.channel_layout, 0);
		av_opt_set_int(dec->swr, "in_sample_fmt",
			       frame.format, 0);
		av_opt_set_int(dec->swr, "in_sample_rate",
			       frame.sample_rate, 0);
		av_opt_set_int(dec->swr, "out_channel_layout",
			       dec->out_channel_layout, 0);
		av_opt_set_int(dec->swr, "out_sample_fmt",
			       dec->out_format, 0);
		av_opt_set_int(dec->swr, "out_sample_rate",
			       dec->out_sample_rate, 0);

		ret = swr_init(dec->swr);
		if (ret < 0) {
			err("failed to setup resampler: %s", av_err2str(ret));
			return ret;
		}

		dec->swr_in_format = frame.format;
		dec->swr_in_channel_layout = frame.channel_layout;
		dec->swr_out_format = dec->out_format;
		dec->swr_out_channel_layout = dec->out_channel_layout;
	}

	/* resample/convert pcm buffer */
	size = av_samples_get_buffer_size(NULL, dec->out_channels,
					  frame.nb_samples,
					  dec->out_format, 1);
	if (size < 0) {
		err("failed to get resampler buffer size: %s", av_err2str(ret));
		return ret;
	}

	if (size != dec->swr_buffer_size) {
		void *p;

		p = realloc(dec->swr_buffer, size);
		if (!p)
			return AVERROR(ENOMEM);

		dec->swr_buffer_size = size;
		dec->swr_buffer = p;
	}

	ret = swr_convert(dec->swr,
			  (uint8_t **)&dec->swr_buffer, dec->swr_buffer_size,
			  (const uint8_t **)frame.data, frame.nb_samples);
	if (ret < 0) {
		err("failed to resample audio: %s", av_err2str(ret));
		return ret;
	}

	/* output converted pcm buffer in qap format */
	memset(&out, 0, sizeof (out));
	out.common_params.data = dec->swr_buffer;
	out.common_params.size = dec->swr_buffer_size;
	out.common_params.timestamp = frame.pts;
	out.buffer_parms.output_buf_params.output_config = dec->out_config;

	dec->cb(dec->cb_data, &out);

	av_frame_unref(&frame);

	return 0;
}

static int
qd_sw_decoder_write(struct qd_sw_decoder *dec, qap_audio_buffer_t *buffer)
{
	AVPacket pkt = {};
	int ret;

	pkt.data = buffer->common_params.data;
	pkt.size = buffer->common_params.size;
	pkt.pts = buffer->common_params.timestamp;

	ret = avcodec_send_packet(dec->codec, &pkt);
	if (ret != 0) {
		err("failed to decode audio: %s", av_err2str(ret));
		return ret;
	}

	do {
		ret = qd_sw_decoder_process_frame(dec);
	} while (ret == 0);

	return 0;
}

bool
qd_format_is_pcm(qap_audio_format_t format)
{
	return format == QAP_AUDIO_FORMAT_PCM_16_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_32_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_8_24_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED;
}

bool
qd_format_is_raw(qap_audio_format_t format)
{
	return qd_format_is_pcm(format) ||
		format == QAP_AUDIO_FORMAT_AAC;
}

static qap_lib_handle_t
qd_module_load(enum qd_module_type type)
{
	struct qd_module_handle *module;
	const char *lib;

	switch (type) {
	case QD_MODULE_DTS_M8:
		lib = QAP_LIB_DTS_M8;
		break;
	case QD_MODULE_DOLBY_MS12:
		lib = QAP_LIB_DOLBY_MS12;
		break;
	default:
		err("unsupported module %d", type);
		return NULL;
	}

	module = &qd_modules[type];
	assert(!module->handle);

	module->handle = qap_load_library(lib);
	if (!module->handle)
		err("failed to load library %s", lib);

	return module->handle;
}

static void
qd_module_unload(enum qd_module_type type)
{
	struct qd_module_handle *module;

	module = &qd_modules[type];
	assert(module->handle);

	qap_unload_library(module->handle);
	module->handle = NULL;
}

static struct qd_output *
qd_session_get_primary_output(struct qd_session *session)
{
	for (int i = 0; i < QD_MAX_OUTPUTS; i++) {
		if (session->outputs[i].enabled)
			return &session->outputs[i];
	}
	return NULL;
}

static struct {
	uint32_t wav_channel;
	uint8_t qap_channel;
} wav_channel_table[] = {
	/* keep in order of wav channels in pcm sample */
	{ WAV_SPEAKER_FRONT_LEFT, QAP_AUDIO_PCM_CHANNEL_L },
	{ WAV_SPEAKER_FRONT_RIGHT, QAP_AUDIO_PCM_CHANNEL_R },
	{ WAV_SPEAKER_FRONT_CENTER, QAP_AUDIO_PCM_CHANNEL_C },
	{ WAV_SPEAKER_LOW_FREQUENCY, QAP_AUDIO_PCM_CHANNEL_LFE },
	{ WAV_SPEAKER_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_LS },
	{ WAV_SPEAKER_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_RS },
	{ WAV_SPEAKER_SIDE_LEFT, QAP_AUDIO_PCM_CHANNEL_LB },
	{ WAV_SPEAKER_SIDE_RIGHT, QAP_AUDIO_PCM_CHANNEL_RB },
	{ WAV_SPEAKER_FRONT_LEFT_OF_CENTER, QAP_AUDIO_PCM_CHANNEL_FLC },
	{ WAV_SPEAKER_FRONT_RIGHT_OF_CENTER, QAP_AUDIO_PCM_CHANNEL_FRC },
	{ WAV_SPEAKER_BACK_CENTER, QAP_AUDIO_PCM_CHANNEL_CS },
	{ WAV_SPEAKER_SIDE_LEFT, QAP_AUDIO_PCM_CHANNEL_SL },
	{ WAV_SPEAKER_SIDE_RIGHT, QAP_AUDIO_PCM_CHANNEL_SR },
	{ WAV_SPEAKER_TOP_CENTER, QAP_AUDIO_PCM_CHANNEL_TC },
	{ WAV_SPEAKER_TOP_FRONT_LEFT, QAP_AUDIO_PCM_CHANNEL_TFL },
	{ WAV_SPEAKER_TOP_FRONT_CENTER, QAP_AUDIO_PCM_CHANNEL_TFC },
	{ WAV_SPEAKER_TOP_FRONT_RIGHT, QAP_AUDIO_PCM_CHANNEL_TFR },
	{ WAV_SPEAKER_TOP_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_TBL },
	{ WAV_SPEAKER_TOP_BACK_CENTER, QAP_AUDIO_PCM_CHANNEL_TBC },
	{ WAV_SPEAKER_TOP_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_TBR },
};

static int
output_write_header(struct qd_output *out)
{
	qap_output_config_t *cfg = &out->config;
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	int wav_channel_count = 0;
	bool output_is_stdout;
	struct wav_header hdr;
	uint32_t channel_mask = 0;
	const char *output_dir;
	char filename[PATH_MAX];

	output_dir = out->session->output_dir;
	if (!output_dir)
		return 0;

	output_is_stdout = !strcmp(output_dir, "-");

	if (out->discont) {
		if (out->stream) {
			if (output_is_stdout) {
				err("cannot reconfigure output when writing to stdout");
				return -1;
			}
			fclose(out->stream);
		}
		out->stream = NULL;
		out->discont = false;
	}

	if (out->stream)
		return 0;

	if (output_is_stdout) {
		out->stream = stdout;
	} else {
		if (mkdir_p(output_dir, 0777)) {
			err("failed to create output directory %s: %m",
			    output_dir);
			return -1;
		}

		snprintf(filename, sizeof (filename),
			 "%s/%03u.%s.%s", output_dir,
			 out->session->outputs_configure_count, out->name,
			 audio_format_extension(out->config.format));

		out->stream = fopen(filename, "w");
		if (!out->stream) {
			err("failed to create output file %s: %m", filename);
			return -1;
		}

		info("dumping audio output to %s", filename);
	}

	if (!qd_format_is_pcm(out->config.format)) {
		// nothing to do here
		return 0;
	}

	for (unsigned i = 0; i < QD_N_ELEMENTS(wav_channel_table); i++) {
		uint8_t qap_ch = wav_channel_table[i].qap_channel;
		uint32_t wav_ch = wav_channel_table[i].wav_channel;

		for (int pos = 0; pos < cfg->channels; pos++) {
			if (cfg->ch_map[pos] == qap_ch) {
				wav_channel_offset[wav_channel_count++] =
					pos * cfg->bit_width / 8;
				channel_mask |= wav_ch;
			}
		}
	}

	if (wav_channel_count != cfg->channels) {
		fprintf(stderr, "dropping %d channels from output",
			cfg->channels - wav_channel_count);
	}

	memcpy(&hdr.riff_magic, "RIFF", 4);
	hdr.riff_chunk_size = 0xffffffff;
	memcpy(&hdr.wave_magic, "WAVE", 4);

	memcpy(&hdr.fmt.chunk_label, "fmt ", 4);
	hdr.fmt.chunk_size = sizeof (hdr.fmt) - 8;
	hdr.fmt.audio_format = 0xfffe; // WAVE_FORMAT_EXTENSIBLE
	hdr.fmt.num_channels = wav_channel_count;
	hdr.fmt.sample_rate = cfg->sample_rate;
	hdr.fmt.byte_rate = cfg->sample_rate * wav_channel_count *
		cfg->bit_width / 8;
	hdr.fmt.block_align = wav_channel_count * cfg->bit_width / 8;
	hdr.fmt.bits_per_sample = cfg->bit_width;

	hdr.fmt.cb_size = sizeof (hdr.fmt.ext);
	hdr.fmt.ext.valid_bits_per_sample = cfg->bit_width;
	hdr.fmt.ext.channel_mask = channel_mask;
	memcpy(&hdr.fmt.ext.sub_format,
	       "\x01\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71",
	       16);

	memcpy(&hdr.data.chunk_label, "data", 4);
	hdr.data.chunk_size = 0xffffffff;

	if (fwrite(&hdr, sizeof (hdr), 1, out->stream) != 1) {
		fprintf(stderr, "failed to write wav header\n");
		return -1;
	}

	memcpy(out->wav_channel_offset, wav_channel_offset,
	       sizeof (wav_channel_offset));

	out->wav_channel_count = wav_channel_count;
	out->wav_enabled = true;

	return 0;
}

static int
output_write_buffer(struct qd_output *out, const qap_buffer_common_t *buffer)
{
	if (!out->stream)
		return 0;

	if (out->wav_enabled) {
		const uint8_t *src;
		int sample_size;
		int frame_size;
		int n_frames;

		src = buffer->data;

		sample_size = out->config.bit_width / 8;
		frame_size = out->config.channels * sample_size;
		n_frames = buffer->size / frame_size;

		assert(buffer->size % frame_size == 0);

		for (int i = 0; i < n_frames; i++) {
			for (int ch = 0; ch < out->wav_channel_count; ch++) {
				fwrite(src + out->wav_channel_offset[ch],
				       sample_size, 1, out->stream);
			}
			src += frame_size;
		}
	} else {
		fwrite(buffer->data, buffer->size, 1, out->stream);
	}

	return 0;
}

static void
qd_output_set_config(struct qd_output *output, qap_output_config_t *cfg)
{
	info("out: %s: config: id=0x%x format=%s sr=%d ss=%d "
	     "interleaved=%d channels=%d chmap[%s]",
	     output->name, cfg->id,
	     audio_format_to_str(cfg->format),
	     cfg->sample_rate, cfg->bit_width, cfg->is_interleaved,
	     cfg->channels, audio_chmap_to_str(cfg->channels, cfg->ch_map));

	output->config = *cfg;

	if (!output->start_time)
		output->start_time = qd_get_time();

	output_write_header(output);
}

static void handle_buffer(struct qd_session *session,
			  qap_audio_buffer_t *buffer);

static void
handle_decoded_buffer(void *userdata, qap_audio_buffer_t *buffer)
{
	struct qd_output *output = userdata;
	qap_output_config_t *config;

	buffer->buffer_parms.output_buf_params.output_id = output->id;
	config = &buffer->buffer_parms.output_buf_params.output_config;

	if (memcmp(config, &output->config, sizeof (output->config)))
		qd_output_set_config(output, config);

	handle_buffer(output->session, buffer);
}

static void
handle_encoded_buffer(struct qd_output *output, qap_audio_buffer_t *buffer)
{
	struct qd_output *dec_output;

	if (output->id == QD_OUTPUT_AC3)
		dec_output = &output->session->outputs[QD_OUTPUT_AC3_DECODED];
	else
		dec_output = &output->session->outputs[QD_OUTPUT_EAC3_DECODED];

	if (!dec_output->enabled)
		return;

	if (!dec_output->swdec) {
		dec_output->swdec = qd_sw_decoder_create(output->config.format);
		if (!dec_output->swdec)
			return;
		qd_sw_decoder_set_callback(dec_output->swdec,
					   handle_decoded_buffer,
					   dec_output);
	}

	qd_sw_decoder_write(dec_output->swdec, buffer);
}

static void
handle_buffer(struct qd_session *session, qap_audio_buffer_t *buffer)
{
	int id = buffer->buffer_parms.output_buf_params.output_id;
	struct qd_output *output = qd_session_get_output(session, id);
	uint64_t pts;

	dbg("out: %s: pcm buffer size=%u pts=%" PRIu64
	    " duration=%lu last_pts=%" PRIu64 " last_diff=%" PRIi64,
	    output->name,
	    buffer->common_params.size, buffer->common_params.timestamp,
	    buffer->common_params.size * 1000000UL /
	    (output->config.channels * output->config.bit_width / 8) /
	    output->config.sample_rate,
	    output->last_ts,
	    buffer->common_params.timestamp - output->last_ts);

	if (qd_format_is_pcm(output->config.format)) {
		output->total_frames += buffer->common_params.size /
			(output->config.bit_width / 8 *
			 output->config.channels);
	} else {
		output->total_frames++;
	}

	output->last_ts = buffer->common_params.timestamp;
	output->total_bytes += buffer->common_params.size;

	if (qd_format_is_pcm(output->config.format)) {
		pts = output->total_frames * 1000000 /
			output->config.sample_rate;
	} else if (output->config.format == QAP_AUDIO_FORMAT_AC3 ||
		   output->config.format == QAP_AUDIO_FORMAT_EAC3) {
		/* Dolby encoder always outputs 32ms frames */
		pts = output->total_frames * 32000;
	} else {
		err("unsupported output format");
		return;
	}

	if (session->realtime &&
	    output == qd_session_get_primary_output(session)) {
		uint64_t now;
		int64_t delay;

		now = qd_get_time() - output->start_time;
		delay = pts - now;

		if (delay <= 0) {
			dbg("out: %s: buffer late by %" PRIi64 "us",
			    output->name, -delay);
		} else {
			dbg("out: %s: wait %" PRIi64 "us for sync",
			    output->name, delay);
			usleep(delay);
		}
	}

	if (output->id == QD_OUTPUT_AC3 || output->id == QD_OUTPUT_EAC3)
		handle_encoded_buffer(output, buffer);

	if (pts <= session->output_discard_ms * 1000) {
		dbg("out: %s: discard buffer at pos %" PRIu64 "ms",
		    output->name, pts / 1000);
		return;
	}

	dbg("out: %s: render buffer, output time=%" PRIu64, output->name,
	    output->pts);

	output_write_buffer(output, &buffer->common_params);

	if (session->output_cb_func) {
		session->output_cb_func(output, buffer,
					session->output_cb_data);
	}

	output->pts = pts;
}

static void
handle_output_config(struct qd_session *session,
		     qap_output_buff_params_t *out_buffer)
{
	struct qd_output *output =
		qd_session_get_output(session, out_buffer->output_id);

	qd_output_set_config(output, &out_buffer->output_config);
}

static void
handle_output_delay(struct qd_session *session, qap_output_delay_t *delay)
{
	struct qd_output *output =
		qd_session_get_output(session, delay->output_id);
	int log_level;

	if (output->delay.algo_delay == delay->algo_delay &&
	    output->delay.buffering_delay == delay->buffering_delay &&
	    output->delay.non_main_data_length == delay->non_main_data_length &&
	    output->delay.non_main_data_offset == delay->non_main_data_offset)
		log_level = 4;
	else
		log_level = 3;

	log(log_level, "out: %s: delay: "
	    "algo_delay=%u/%ums "
	    "buffering_delay=%u/%ums "
	    "non_main_data_offset=%u "
	    "non_main_data_length=%u\n",
	    output->name,
	    delay->algo_delay,
	    delay->algo_delay / 48,
	    delay->buffering_delay,
	    delay->buffering_delay / 48,
	    delay->non_main_data_offset,
	    delay->non_main_data_length);

	output->delay = *delay;
}

static void
handle_qap_session_event(qap_session_handle_t session, void *priv,
			 qap_callback_event_t event_id, int size, void *data)
{
	struct qd_session *qd_session = priv;

	switch (event_id) {
	case QAP_CALLBACK_EVENT_DATA:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_DATA "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_buffer(qd_session, (qap_audio_buffer_t *)data);
		break;
	case QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_output_config(qd_session,
				     &((qap_audio_buffer_t *)data)->buffer_parms.output_buf_params);
		break;
	case QAP_CALLBACK_EVENT_EOS:
		info("qap: EOS for primary");
		pthread_mutex_lock(&qd_session->lock);
		qd_session->eos_inputs |= 1 << QD_INPUT_MAIN;
		pthread_cond_signal(&qd_session->cond);
		pthread_mutex_unlock(&qd_session->lock);
		break;
	case QAP_CALLBACK_EVENT_MAIN_2_EOS:
		info("qap: EOS for secondary");
		pthread_mutex_lock(&qd_session->lock);
		qd_session->eos_inputs |= 1 << QD_INPUT_MAIN2;
		pthread_cond_signal(&qd_session->cond);
		pthread_mutex_unlock(&qd_session->lock);
		break;
	case QAP_CALLBACK_EVENT_EOS_ASSOC:
		info("qap: EOS for assoc");
		pthread_mutex_lock(&qd_session->lock);
		qd_session->eos_inputs |= 1 << QD_INPUT_ASSOC;
		pthread_cond_signal(&qd_session->cond);
		pthread_mutex_unlock(&qd_session->lock);
		break;
	case QAP_CALLBACK_EVENT_ERROR:
		info("qap: error");
		qd_session->terminated = true;
		//FIXME: close inputs
		break;
	case QAP_CALLBACK_EVENT_SUCCESS:
		info("qap: success");
		break;
	case QAP_CALLBACK_EVENT_METADATA:
		info("qap: metadata");
		break;
	case QAP_CALLBACK_EVENT_DELAY:
		if (size != sizeof (qap_output_delay_t)) {
			err("QAP_CALLBACK_EVENT_DELAY "
			    "size=%d expected=%zu", size,
			    sizeof (qap_output_delay_t));
		}
		handle_output_delay(qd_session, data);
		break;
	default:
		err("unknown QAP session event %u", event_id);
		break;
	}
}

static void
handle_input_config(struct qd_input *input, qap_input_config_t *cfg)
{
	info(" in: %s: codec=%s profile=%s sr=%u ss=%u channels=%u ch_map[%s]",
	     input->name, audio_format_to_str(cfg->format),
	     audio_profile_to_str(cfg->format, cfg->profile),
	     cfg->sample_rate, cfg->bit_width, cfg->channels,
	     audio_chmap_to_str(cfg->channels, cfg->ch_map));

	input->config = *cfg;

	if (input->event_cb_func) {
		input->event_cb_func(input, QD_INPUT_CONFIG_CHANGED,
				     input->event_cb_data);
	}
}

static void
handle_qap_module_event(qap_module_handle_t module, void *priv,
			qap_module_callback_event_t event_id,
			int size, void *data)
{
	struct qd_input *input = priv;

	switch (event_id) {
	case QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER:
		if (size != sizeof (qap_send_buffer_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER "
			    "size=%d expected=%zu", size,
			    sizeof (qap_send_buffer_t));
		} else {
			qap_send_buffer_t *buf = data;
			dbg(" in: %s: notify %u bytes avail", input->name,
			    buf->bytes_available);
		}
		pthread_mutex_lock(&input->lock);
		input->buffer_full = false;
		pthread_cond_signal(&input->cond);
		pthread_mutex_unlock(&input->lock);
		break;
	case QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE:
		if (size != sizeof (qap_input_config_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_input_config_t));
		}
		handle_input_config(input, data);
		break;
	default:
		err("unknown QAP module event %u", event_id);
		break;
	}
}

static void
wait_buffer_available(struct qd_input *input)
{
	pthread_mutex_lock(&input->lock);
	while (!input->terminated && input->buffer_full) {
		struct timespec delay;

		clock_gettime(CLOCK_REALTIME, &delay);
		delay.tv_sec++;

		if (pthread_cond_timedwait(&input->cond, &input->lock,
					   &delay) == ETIMEDOUT &&
		    input->state == QD_INPUT_STATE_STARTED) {
			err("%s: stalled, buffer has been full for 1 second",
			    input->name);
		}
	}
	pthread_mutex_unlock(&input->lock);
}

static int
get_av_log_level(void)
{
	if (qd_debug_level >= 5)
		return AV_LOG_TRACE;
	if (qd_debug_level >= 4)
		return AV_LOG_DEBUG;
	if (qd_debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (qd_debug_level >= 2)
		return AV_LOG_INFO;
	if (qd_debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

int
qd_input_start(struct qd_input *input)
{
	uint64_t t;
	int ret;

	if (input->state == QD_INPUT_STATE_STARTED) {
		info(" in: %s: already started", input->name);
		return 0;
	}

	info(" in: %s: start", input->name);

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_START,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_START command failed");
		return 1;
	}

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_START)",
	      input->name, (get_time() - t) / 1000);

	input->state = QD_INPUT_STATE_STARTED;
	input->state_change_time = qd_get_time();

	return 0;
}

int
qd_input_pause(struct qd_input *input)
{
	uint64_t t;
	int ret;

	if (input->state != QD_INPUT_STATE_STARTED) {
		info(" in: %s: cannot pause, not started", input->name);
		return 0;
	}

	info(" in: %s: pause", input->name);

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_PAUSE,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_PAUSE command failed");
		return 1;
	}

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_PAUSE)",
	      input->name, (get_time() - t) / 1000);

	input->state = QD_INPUT_STATE_PAUSED;
	input->state_change_time = qd_get_time();

	return 0;
}

int
qd_input_stop(struct qd_input *input)
{
	uint64_t t;
	int ret;

	if (input->state == QD_INPUT_STATE_STOPPED) {
		info(" in: %s: already stopped", input->name);
		return 0;
	}

	info(" in: %s: stop", input->name);

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_STOP,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_STOP command failed");
		return 1;
	}

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_STOP)",
	      input->name, (get_time() - t) / 1000);

	input->state = QD_INPUT_STATE_STOPPED;
	input->state_change_time = qd_get_time();

	return 0;
}

int
qd_input_flush(struct qd_input *input)
{
	uint64_t t;
	int ret;

	info(" in: %s: flush", input->name);

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_FLUSH,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_FLUSH command failed");
		return 1;
	}

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_FLUSH)",
	      input->name, (get_time() - t) / 1000);

	info(" in: %s: flush done", input->name);

#if 0
	// not needed, but adding this works around SEND_INPUT_BUFFER
	// not sent after flush
	pthread_mutex_lock(&input->lock);
	input->buffer_full = false;
	pthread_cond_signal(&input->cond);
	pthread_mutex_unlock(&input->lock);
#endif

	return 0;
}

int
qd_input_block(struct qd_input *input, bool block)
{
	info(" in: %s: %s", input->name, block ? "block" : "unblock");

	pthread_mutex_lock(&input->lock);
	input->blocked = block;
	pthread_cond_signal(&input->cond);
	pthread_mutex_unlock(&input->lock);

	return 0;
}

static int
qd_input_get_param(struct qd_input *input, uint32_t param_id,
		   void *data, size_t size)
{
	qap_status_t ret;
	uint32_t reply_size;
	uint64_t t;

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_GET_PARAM,
			     sizeof (param_id), &param_id,
			     &reply_size, data);

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_GET_PARAM, %u)",
	      input->name, (get_time() - t) / 1000, param_id);

	assert(reply_size == size);

	return ret;
}

static int
qd_input_set_param(struct qd_input *input,
		   uint32_t param_id, uint32_t value)
{
	uint32_t params[] = { param_id, value };
	qap_status_t ret;
	uint64_t t;

	t = get_time();

	ret = qap_module_cmd(input->module, QAP_MODULE_CMD_SET_PARAM,
			     sizeof (params), params, NULL, NULL);

	trace(" in: %s: [t=%" PRIu64 "ms] qap_module_cmd(QAP_MODULE_CMD_SET_PARAM, %u)",
	      input->name, (get_time() - t) / 1000, param_id);

	return ret;
}

uint32_t
qd_input_get_buffer_size(struct qd_input *input)
{
	uint32_t buffer_size = 0;
	int ret;

	ret = qd_input_get_param(input, MS12_STREAM_GET_INPUT_BUF_SIZE,
				 &buffer_size, sizeof (buffer_size));
	if (ret < 0) {
		err("%s: failed to get buffer size", input->name);
		return 0;
	}

	return buffer_size;
}

int
qd_input_set_buffer_size(struct qd_input *input, uint32_t buffer_size)
{
	int ret;

	info(" in: %s: set buffer size %u bytes", input->name, buffer_size);

	ret = qd_input_set_param(input, MS12_STREAM_SET_INPUT_BUF_SIZE,
				 buffer_size);
	if (ret < 0) {
		err("%s: failed to set buffer size %u", input->name,
		    buffer_size);
		return -1;
	}

	assert(buffer_size == qd_input_get_buffer_size(input));

	return 0;
}

uint32_t
qd_input_get_avail_buffer_size(struct qd_input *input)
{
	uint32_t buffer_size = 0;
	int ret;

	ret = qd_input_get_param(input, MS12_STREAM_GET_AVAIL_BUF_SIZE,
				 &buffer_size, sizeof (buffer_size));
	if (ret < 0) {
		err("%s: failed to get avail buffer size", input->name);
		return 0;
	}

	return buffer_size;
}

uint64_t
qd_input_get_output_frames(struct qd_input *input)
{
	uint64_t frames = 0;
	int ret;

	ret = qd_input_get_param(input, MS12_STREAM_GET_DECODER_OUTPUT_FRAME,
				 &frames, sizeof (frames));
	if (ret < 0) {
		err("%s: failed to get output frames", input->name);
		return 0;
	}

	return frames;
}

int
qd_input_get_io_info(struct qd_input *input, qap_report_frames_t *report)
{
	int ret;

	if (!report)
		return -1;

	ret = qd_input_get_param(input, MS12_STREAM_GET_DECODER_IO_FRAMES_INFO,
				 report, sizeof (*report));
	if (ret < 0) {
		err("%s: failed to get decoder io info", input->name);
		return -1;
	}

	return 0;
}

int
qd_input_get_latency(struct qd_input *input)
{
	int64_t latency = 0;
	int ret;

	ret = qd_input_get_param(input, MS12_STREAM_GET_LATENCY,
				 &latency, sizeof (latency));
	if (ret < 0) {
		err("%s: failed to get latency", input->name);
		return 0;
	}

	return latency;
}

void
qd_input_terminate(struct qd_input *input)
{
	dbg(" in: %s: terminate", input->name);
	pthread_mutex_lock(&input->lock);
	input->terminated = true;
	pthread_cond_signal(&input->cond);
	pthread_mutex_unlock(&input->lock);
}

void
qd_input_destroy(struct qd_input *input)
{
	struct qd_session *session;

	if (!input)
		return;

	qd_input_stop(input);
	qd_input_flush(input);

	if (input->module && qap_module_deinit(input->module))
		err("failed to deinit %s module", input->name);

	if (input->avmux)
		avformat_free_context(input->avmux);

	session = input->session;

	pthread_mutex_lock(&session->lock);
	session->eos_inputs &= ~(1 << input->id);
	pthread_mutex_unlock(&session->lock);

	pthread_cond_destroy(&input->cond);
	pthread_mutex_destroy(&input->lock);
	free(input);
}

struct qd_input *
qd_input_create(struct qd_session *session, enum qd_input_id id,
		qap_module_config_t *qap_config)
{
	struct qd_input *input;
	uint32_t buffer_size;

	input = calloc(1, sizeof *input);
	if (!input)
		return NULL;

	input->name = qd_input_id_to_str(id);
	input->id = id;
	input->session = session;

	pthread_cond_init(&input->cond, NULL);
	pthread_mutex_init(&input->lock, NULL);

	if (qap_module_init(session->handle, qap_config, &input->module)) {
		err("failed to init module");
		goto fail;
	}

	if (qap_module_set_callback(input->module,
				    handle_qap_module_event, input)) {
		err("failed to set module callback");
		goto fail;
	}

	buffer_size = qd_input_get_buffer_size(input);
	if (buffer_size > 0) {
		info(" in: %s: default buffer size %u bytes",
		     input->name, buffer_size);
	}

	if (qd_format_is_pcm(qap_config->format)) {
		if (session->buffer_size_ms > 0) {
			buffer_size = qap_config->sample_rate *
				qap_config->channels *
				qap_config->bit_width / 8 *
				session->buffer_size_ms / 1000;

			if (qd_input_set_buffer_size(input, buffer_size))
				goto fail;
		}
	} else {
		buffer_size = 4 * 1024;
		if (qd_input_set_buffer_size(input, buffer_size))
			goto fail;
	}

	input->buffer_size = buffer_size;

	info(" in: %s: latency %dms", input->name,
	     qd_input_get_latency(input));

	if (qd_input_start(input))
		goto fail;

	return input;

fail:
	qd_input_destroy(input);
	return NULL;
}

struct qd_input *
qd_input_create_from_avstream(struct qd_session *session, enum qd_input_id id,
			      AVStream *avstream)
{
	struct qd_input *input;
	char channel_layout_desc[32];
	qap_audio_format_t qap_format;
	qap_module_config_t qap_mod_cfg;
	qap_module_flags_t qap_flags;
	AVCodecParameters *codecpar;

	codecpar = avstream->codecpar;

	switch (id) {
	case QD_INPUT_MAIN:
	case QD_INPUT_MAIN2:
		qap_flags = QAP_MODULE_FLAG_PRIMARY;
		break;
	case QD_INPUT_ASSOC:
		qap_flags = QAP_MODULE_FLAG_SECONDARY;
		break;
	case QD_INPUT_SYS_SOUND:
		qap_flags = QAP_MODULE_FLAG_SYSTEM_SOUND;
		break;
	case QD_INPUT_APP_SOUND:
		qap_flags = QAP_MODULE_FLAG_APP_SOUND;
		break;
	case QD_INPUT_OTT_SOUND:
		qap_flags = QAP_MODULE_FLAG_OTT_SOUND;
		break;
	case QD_INPUT_EXT_PCM:
		qap_flags = QAP_MODULE_FLAG_EXTERN_PCM;
		break;
	default:
		err("unknown input id %d", id);
		return NULL;
	}

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_AC3:
		qap_format = QAP_AUDIO_FORMAT_AC3;
		break;
	case AV_CODEC_ID_EAC3:
		qap_format = QAP_AUDIO_FORMAT_EAC3;
		break;
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_AAC_LATM:
		qap_format = QAP_AUDIO_FORMAT_AAC_ADTS;
		break;
	case AV_CODEC_ID_DTS:
		qap_format = QAP_AUDIO_FORMAT_DTS;
		break;
	case AV_CODEC_ID_PCM_S16LE:
		qap_format = QAP_AUDIO_FORMAT_PCM_16_BIT;
		break;
	case AV_CODEC_ID_PCM_S24LE:
		qap_format = QAP_AUDIO_FORMAT_PCM_8_24_BIT;
		break;
	case AV_CODEC_ID_PCM_S32LE:
		qap_format = QAP_AUDIO_FORMAT_PCM_32_BIT;
		break;
	default:
		err("cannot decode %s format",
		    avcodec_get_name(codecpar->codec_id));
		return NULL;
	}

	memset(&qap_mod_cfg, 0, sizeof (qap_mod_cfg));
	qap_mod_cfg.module_type = QAP_MODULE_DECODER;
	qap_mod_cfg.flags = qap_flags;
	qap_mod_cfg.format = qap_format;

	if (qd_format_is_raw(qap_format)) {
		qap_mod_cfg.channels = codecpar->channels;
		qap_mod_cfg.is_interleaved = true;
		qap_mod_cfg.sample_rate = codecpar->sample_rate;
		qap_mod_cfg.bit_width = codecpar->bits_per_coded_sample;
	}

	av_get_channel_layout_string(channel_layout_desc,
				     sizeof (channel_layout_desc),
				     codecpar->channels,
				     codecpar->channel_layout);

	if (qd_format_is_pcm(qap_format)) {
		notice(" in: %s: use stream %d, %s, %d Hz, %s, %d bits, %" PRIi64 " kb/s",
		       qd_input_id_to_str(id), avstream->id,
		       avcodec_get_name(codecpar->codec_id),
		       codecpar->sample_rate, channel_layout_desc,
		       codecpar->bits_per_coded_sample,
		       codecpar->bit_rate / 1000);
	} else {
		notice(" in: %s: use stream %d, %s, %d Hz, %s, %" PRIi64 " kb/s",
		       qd_input_id_to_str(id), avstream->id,
		       avcodec_get_name(codecpar->codec_id),
		       codecpar->sample_rate, channel_layout_desc,
		       codecpar->bit_rate / 1000);
	}

	input = qd_input_create(session, id, &qap_mod_cfg);
	if (!input)
		return NULL;

	if (codecpar->codec_id == AV_CODEC_ID_AAC &&
	    codecpar->extradata_size >= 2) {
		uint16_t config;
		int obj_type, rate_idx, channels_idx;

		config = be16toh(*(uint16_t *)codecpar->extradata);
		obj_type = (config & 0xf800) >> 11;
		rate_idx = (config & 0x0780) >> 7;
		channels_idx = (config & 0x0078) >> 3;

		if (obj_type == 0) {
			err("invalid AOT 0");
			goto fail;
		}

		if (obj_type <= 4 && rate_idx < 15) {
			/* prepare ADTS header for MAIN, LC, SSR profiles */
			input->adts_header[0] = 0xff;
			input->adts_header[1] = 0xf9;
			input->adts_header[2] = (obj_type - 1) << 6;
			input->adts_header[2] |= rate_idx << 2;
			input->adts_header[2] |= (channels_idx & 4) >> 2;
			input->adts_header[3] = (channels_idx & 3) << 6;
			input->adts_header[4] = 0;
			input->adts_header[5] = 0x1f;
			input->adts_header[6] = 0x1c;
			input->insert_adts_header = true;
		} else {
			/* otherwise try to use LATM muxer, which also supports
			 * SBR and ALS */
			AVStream *mux_stream;

			int ret = avformat_alloc_output_context2(&input->avmux,
								 NULL, "latm",
								 NULL);
			if (ret < 0) {
				av_err(ret, "failed to create latm mux");
				goto fail;
			}

			mux_stream = avformat_new_stream(input->avmux, NULL);
			if (!mux_stream) {
				err("failed to create latm stream");
				goto fail;
			}

			mux_stream->time_base = avstream->time_base;
			avcodec_parameters_copy(mux_stream->codecpar,
						avstream->codecpar);

			ret = avformat_write_header(input->avmux, NULL);
			if (ret < 0) {
				av_err(ret, "failed to write latm header");
				goto fail;
			}
		}
	}

	return input;

fail:
	qd_input_destroy(input);
	return NULL;
}

int
qd_input_write(struct qd_input *input, void *data, int size, int64_t pts)
{
	qap_audio_buffer_t qap_buffer;
	qap_report_frames_t report_frames;
	int offset = 0;
	int ret;

	if (input->written_bytes == 0)
		input->start_time = qd_get_time();

	memset(&qap_buffer, 0, sizeof (qap_buffer));

	/* XXX: don't use timestamps in OTT mode, otherwise dual decode and
	 * pause commands do not work correctly */
	if (pts == AV_NOPTS_VALUE ||
	    input->session->ignore_timestamps > 0 ||
	    (input->session->ignore_timestamps == -1 &&
	     input->session->type == QAP_SESSION_MS12_OTT)) {
		qap_buffer.common_params.timestamp = 0;
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_NO_TSTAMP;
	} else {
		qap_buffer.common_params.timestamp = pts;
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_TSTAMP;
	}

	dbg(" in: %s: buffer size=%d pts=%" PRIi64 " -> %" PRIi64,
	    input->name, size, pts, qap_buffer.common_params.timestamp);

	assert(size <= 24 * 1024);

	while (!input->terminated && offset < size) {
		uint64_t t;

		qap_buffer.common_params.offset = 0;
		qap_buffer.common_params.data = data + offset;
		qap_buffer.common_params.size = size - offset;

		if (input->buffer_size > 0 &&
		    qap_buffer.common_params.size > input->buffer_size)
			qap_buffer.common_params.size = input->buffer_size;

		dbg(" in: %s: %u bytes available", input->name,
		    qd_input_get_avail_buffer_size(input));

		pthread_mutex_lock(&input->lock);
		input->buffer_full = true;
		pthread_mutex_unlock(&input->lock);

		t = qd_get_time();

		ret = qap_module_process(input->module, &qap_buffer);
		if (ret == -EAGAIN) {
			dbg(" in: %s: wait, buffer is full", input->name);
			wait_buffer_available(input);
		} else if (ret < 0) {
			err("%s: qap_module_process error %d", input->name, ret);
			return -1;
		} else if (ret == 0) {
			err("%s: decoder returned zero size",
			    input->name);
			break;
		} else {
			offset += ret;
			input->written_bytes += ret;

			dbg(" in: %s: written %d bytes in %dus, total %" PRIu64,
			    input->name, ret, (int)(qd_get_time() - t),
			    input->written_bytes);

			qap_buffer.common_params.timestamp = 0;
			qap_buffer.buffer_parms.input_buf_params.flags =
				QAP_BUFFER_TSTAMP_CONTINUE;

			assert(offset <= size);
		}
	}

	if (input->terminated)
		return -1;

	dbg(" in: %s: generated %" PRIu64 " frames", input->name,
	    qd_input_get_output_frames(input));

	if (!qd_input_get_io_info(input, &report_frames)) {
		dbg(" in: %s: consumed=%" PRIu64 " decoded=%" PRIu64,
		    input->name, report_frames.consumed_frames,
		    report_frames.decoded_frames);
	}

	return size;
}

void
qd_input_set_event_cb(struct qd_input *input, qd_input_event_func_t func,
		      void *userdata)
{
	input->event_cb_func = func;
	input->event_cb_data = userdata;
}

void
ffmpeg_src_destroy(struct ffmpeg_src *src)
{
	if (!src)
		return;

	for (int i = 0; i < QD_MAX_STREAMS; i++)
		qd_input_destroy(src->streams[i].input);

	if (src->avctx)
		avformat_close_input(&src->avctx);

	free(src);
}

struct ffmpeg_src *
ffmpeg_src_create(const char *url, const char *format)
{
	AVInputFormat *input_format = NULL;
	struct ffmpeg_src *src;
	int ret;

	if (format) {
		input_format = av_find_input_format(format);
		if (!input_format) {
			err("input format %s not supported", format);
			return NULL;
		}
	}

	src = calloc(1, sizeof *src);
	if (!src)
		return NULL;

	ret = avformat_open_input(&src->avctx, url, input_format, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", url);
		goto fail;
	}

	ret = avformat_find_stream_info(src->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	return src;

fail:
	ffmpeg_src_destroy(src);
	return NULL;
}

uint64_t
ffmpeg_src_get_duration(struct ffmpeg_src *src)
{
	AVRational qap_timebase = { 1, 1000000 };
	return av_rescale_q(src->avctx->duration, AV_TIME_BASE_Q, qap_timebase);
}

AVStream *
ffmpeg_src_get_avstream(struct ffmpeg_src *src, int index)
{
	if (index < 0) {
		index = av_find_best_stream(src->avctx, AVMEDIA_TYPE_AUDIO,
					    -1, -1, NULL, 0);
		if (index < 0)
			return NULL;
	}

	if ((unsigned int)index >= src->avctx->nb_streams)
		return NULL;

	return src->avctx->streams[index];
}

struct qd_input *
ffmpeg_src_add_input(struct ffmpeg_src *src, int index,
		     struct qd_session *session,
		     enum qd_input_id input_id)
{
	AVStream *avstream;
	struct qd_input *input;

	avstream = ffmpeg_src_get_avstream(src, index);
	if (!avstream) {
		err("stream index %d is not usable", index);
		return NULL;
	}

	if (src->n_streams >= QD_MAX_STREAMS) {
		err("too many streams");
		return NULL;
	}

	info(" in: %s: create from %s", qd_input_id_to_str(input_id),
	     src->avctx->url);

	input = qd_input_create_from_avstream(session, input_id, avstream);
	if (!input)
		return NULL;

	src->streams[src->n_streams].index = avstream->index;
	src->streams[src->n_streams].input = input;
	src->n_streams++;

	return input;
}

int
ffmpeg_src_seek(struct ffmpeg_src *src, int64_t position_ms)
{
	AVRational ms_time_base = { 1, 1000 };
	AVStream *avstream;
	struct ffmpeg_src_stream *stream;
	int64_t position;

	if (src->n_streams <= 0)
		return -1;

	stream = &src->streams[0];
	avstream = src->avctx->streams[stream->index];
	position = av_rescale_q(position_ms, ms_time_base,
				avstream->time_base);

	info(" in: %s: seek to %" PRId64 "ms", stream->input->name, position_ms);

	if (av_seek_frame(src->avctx, avstream->index, position, 0) < 0) {
		err(" in: %s: failed to seek to position %" PRId64,
		    stream->input->name, position_ms);
		return -1;
	}

	return 0;
}

static struct qd_input *
ffmpeg_src_find_stream_by_index(struct ffmpeg_src *src, int index)
{
	for (int i = 0; i < src->n_streams; i++) {
		if (src->streams[i].index == index)
			return src->streams[i].input;
	}

	return NULL;
}

int
ffmpeg_src_read_frame(struct ffmpeg_src *src)
{
	struct qd_input *input;
	AVStream *avstream;
	AVPacket pkt;
	int64_t pts;
	int ret;

	av_init_packet(&pkt);

	/* get next audio frame from ffmpeg */
	ret = av_read_frame(src->avctx, &pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF)
			av_err(ret, "failed to read frame from input");
		return ret;
	}

	/* find out which input the frame belongs to */
	input = ffmpeg_src_find_stream_by_index(src, pkt.stream_index);
	if (!input) {
		ret = 0;
		goto out;
	}

	avstream = src->avctx->streams[pkt.stream_index];

	pthread_mutex_lock(&input->lock);
	if (input->blocked) {
		info(" in: %s: blocked", input->name);
		while (input->blocked && !input->terminated) {
			pthread_cond_wait(&input->cond, &input->lock);
		}
		info(" in: %s: unblocked", input->name);
	}
	pthread_mutex_unlock(&input->lock);

	pts = pkt.pts;
	if (pts != AV_NOPTS_VALUE) {
		AVRational av_timebase = avstream->time_base;
		AVRational qap_timebase = { 1, 1000000 };

		if (avstream->start_time != AV_NOPTS_VALUE)
			pts -= avstream->start_time;

		pts = av_rescale_q(pts, av_timebase, qap_timebase);
	}

	if (input->insert_adts_header) {
		/* packets should have AV_INPUT_BUFFER_PADDING_SIZE padding in
		 * them, so we can use that to avoid a copy */
		memmove(pkt.data + ADTS_HEADER_SIZE, pkt.data, pkt.size);
		memcpy(pkt.data, input->adts_header, ADTS_HEADER_SIZE);
		pkt.size += ADTS_HEADER_SIZE;

		/* patch ADTS header with frame size */
		pkt.data[3] |= pkt.size >> 11;
		pkt.data[4] |= pkt.size >> 3;
		pkt.data[5] |= (pkt.size & 0x07) << 5;

		/* push the audio frame to the decoder */
		ret = qd_input_write(input, pkt.data, pkt.size, pts);

	} else if (input->avmux) {
		AVIOContext *avio;
		uint8_t *data;
		int size;

		ret = avio_open_dyn_buf(&avio);
		if (ret < 0) {
			av_err(ret, "failed to create avio context");
			goto out;
		}

		input->avmux->pb = avio;

		pkt.stream_index = 0;
		ret = av_write_frame(input->avmux, &pkt);
		if (ret < 0) {
			av_err(ret, "failed to mux data");
			goto out;
		}

		size = avio_close_dyn_buf(input->avmux->pb, &data);
		input->avmux->pb = NULL;

		ret = qd_input_write(input, data, size, pts);
		av_free(data);
	} else {
		/* push the audio frame to the decoder */
		ret = qd_input_write(input, pkt.data, pkt.size, pts);
	}

	if (input->state == QD_INPUT_STATE_PAUSED &&
	    qd_get_time() - input->state_change_time > 1000000) {
		input->state_change_time = qd_get_time();
		err("%s: input still being consumed 1 second after pause",
		    input->name);
	}

out:
	av_packet_unref(&pkt);
	return ret;
}

static void *
ffmpeg_src_thread_func(void *userdata)
{
	struct ffmpeg_src *src = userdata;
	intptr_t ret = 0;

	while (!src->terminated) {
		ret = ffmpeg_src_read_frame(src);
		if (ret == AVERROR_EOF) {
			info(" in: EOS");
			ret = 0;
			break;
		}

		if (ret < 0) {
			ret = 1;
			break;
		}
	}

	return (void *)ret;
}

int
ffmpeg_src_thread_start(struct ffmpeg_src *src)
{
	return pthread_create(&src->tid, NULL, ffmpeg_src_thread_func, src);
}

void
ffmpeg_src_thread_stop(struct ffmpeg_src *src)
{
	src->terminated = true;
	for (int i = 0; i < src->n_streams; i++)
		qd_input_terminate(src->streams[i].input);
}

int
ffmpeg_src_thread_join(struct ffmpeg_src *src)
{
	void *ret = (void *)1;

	pthread_join(src->tid, &ret);

	for (int i = 0; i < src->n_streams; i++)
		qd_input_stop(src->streams[i].input);

	return (intptr_t)ret;
}

int
qd_session_configure_outputs(struct qd_session *session,
			     int num_outputs, const enum qd_output_id *outputs)
{
	qap_session_outputs_config_t qap_session_cfg;
	uint32_t outputs_present = 0;
	uint64_t t;
	int ret;

	info("enable outputs:");
	for (int i = 0; i < num_outputs; i++) {
		if (outputs[i] != QD_OUTPUT_NONE)
			info(" - %s", qd_output_id_to_str(outputs[i]));
	}

	memset(&qap_session_cfg, 0, sizeof (qap_session_cfg));

	for (int i = 0; i < num_outputs; i++) {
		enum qd_output_id id = outputs[i];
		qap_output_config_t *output_cfg =
			qap_session_cfg.output_config +
			qap_session_cfg.num_output;

		switch (id) {
		case QD_OUTPUT_STEREO:
			output_cfg->channels = 2;
			break;
		case QD_OUTPUT_5DOT1:
			output_cfg->channels = 6;
			break;
		case QD_OUTPUT_7DOT1:
			output_cfg->channels = 8;
			break;
		case QD_OUTPUT_AC3:
		case QD_OUTPUT_AC3_DECODED:
			output_cfg->format = QAP_AUDIO_FORMAT_AC3;
			break;
		case QD_OUTPUT_EAC3:
		case QD_OUTPUT_EAC3_DECODED:
			output_cfg->format = QAP_AUDIO_FORMAT_EAC3;
			break;
		case QD_OUTPUT_NONE:
			continue;
		default:
			err("invalid output id %d", id);
			return -1;
		}

		outputs_present |= 1 << id;

		if (id == QD_OUTPUT_AC3_DECODED) {
			outputs_present |= 1 << QD_OUTPUT_AC3;
			id = QD_OUTPUT_AC3;
		}

		if (id == QD_OUTPUT_EAC3_DECODED) {
			outputs_present |= 1 << QD_OUTPUT_EAC3;
			id = QD_OUTPUT_EAC3;
		}

		output_cfg->id = id;
		qap_session_cfg.num_output++;
	}

	session->outputs_configure_count++;

	for (int i = 0; i < QD_MAX_OUTPUTS; i++) {
		struct qd_output *output = &session->outputs[i];
		bool enabled = outputs_present & (1 << i);

		output->discont = enabled != output->enabled;
		output->enabled = enabled;

		if (!output->enabled && output->stream)
			fflush(output->stream);
	}

	/* setup outputs */
	t = get_time();

	ret = qap_session_cmd(session->handle, QAP_SESSION_CMD_SET_OUTPUTS,
			      sizeof (qap_session_cfg), &qap_session_cfg,
			      NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_SET_CONFIG command failed");
		return ret;
	}

	trace("session: [t=%" PRIu64 "ms] qap_session_cmd(QAP_SESSION_CMD_SET_OUTPUTS)",
	      (get_time() - t) / 1000);

	return 0;
}

int
qd_session_set_kvpairs(struct qd_session *session, char *kvpairs_format, ...)
{
	uint64_t t;
	va_list ap;
	char buf[1024];
	int len, ret;

	va_start(ap, kvpairs_format);
	len = vsnprintf(buf, sizeof (buf), kvpairs_format, ap);
	va_end(ap);

	if (len < 0 || len >= sizeof (buf))
		return -1;

	info("set kvpairs %s", buf);

	t = get_time();

	ret = qap_session_cmd(session->handle, QAP_SESSION_CMD_SET_KVPAIRS,
			      len, buf, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_SET_KVPAIRS '%s' failed: %d", buf, ret);
		return ret;
	}

	trace("session: [t=%" PRIu64 "ms] qap_session_cmd(QAP_SESSION_CMD_SET_KVPAIRS)",
	      (get_time() - t) / 1000);

	return 0;
}

void
qd_session_set_buffer_size_ms(struct qd_session *session,
			      uint32_t buffer_size_ms)
{
	session->buffer_size_ms = buffer_size_ms;
}

void
qd_session_set_realtime(struct qd_session *session, bool realtime)
{
	session->realtime = realtime;
}

void
qd_session_set_dump_path(struct qd_session *session, const char *path)
{
	free(session->output_dir);
	session->output_dir = path ? strdup(path) : NULL;
}

void
qd_session_set_output_discard_ms(struct qd_session *session,
				 int64_t discard_ms)
{
	session->output_discard_ms = discard_ms;
}

void qd_session_ignore_timestamps(struct qd_session *session, bool ignore)
{
	session->ignore_timestamps = !!ignore;
}

void
qd_session_terminate(struct qd_session *session)
{
	if (!session)
		return;

	info("terminate session");
	pthread_mutex_lock(&session->lock);
	session->terminated = true;
	pthread_cond_signal(&session->cond);
	pthread_mutex_unlock(&session->lock);
}

void
qd_session_wait_eos(struct qd_session *session, enum qd_input_id input_id)
{
	pthread_mutex_lock(&session->lock);
	while (!session->terminated) {
		bool done;
		switch (input_id) {
		case QD_INPUT_MAIN:
		case QD_INPUT_MAIN2:
		case QD_INPUT_ASSOC:
			done = session->eos_inputs & (1 << input_id);
			break;
		default:
			done = true;
			break;
		}

		if (done)
			break;

		info(" in %s: wait eos", qd_input_id_to_str(input_id));
		pthread_cond_wait(&session->cond, &session->lock);
	}
	pthread_mutex_unlock(&session->lock);
}

void
qd_session_destroy(struct qd_session *session)
{
	if (!session)
		return;

	dbg("destroy session");

	if (session->handle)
		qap_session_close(session->handle);

	for (int i = 0; i < QD_MAX_OUTPUTS; i++) {
		struct qd_output *output = &session->outputs[i];
		if (output->stream)
			fclose(output->stream);
		qd_sw_decoder_destroy(output->swdec);
	}

	pthread_cond_destroy(&session->cond);
	pthread_mutex_destroy(&session->lock);

	qd_module_unload(session->module);

	free(session->output_dir);
	free(session);
}

static void
handle_log_msg(qap_log_level_t level, const char *msg)
{
	int dbg_level;
	int len;

	switch (level) {
	case QAP_LOG_ERROR:
		dbg_level = 1;
		break;
	case QAP_LOG_INFO:
		dbg_level = 3;
		break;
	default:
	case QAP_LOG_DEBUG:
		dbg_level = 4;
		break;
	}

	len = strlen(msg);
	while (msg[len - 1] == '\n')
		len--;

	log(dbg_level, "%.*s\n", len, msg);
}

struct qd_session *
qd_session_create(enum qd_module_type module, qap_session_t type)
{
	struct qd_session *session;
	qap_lib_handle_t lib_handle;

	lib_handle = qd_module_load(module);
	if (!lib_handle)
		return NULL;

	qap_lib_set_log_callback(lib_handle, handle_log_msg);
	qap_lib_set_log_level(lib_handle, qd_debug_level - 3);

	session = calloc(1, sizeof (*session));
	if (!session) {
		qd_module_unload(module);
		return NULL;
	}

	session->module = module;
	session->type = type;

	/* by default, ignore input timetamps in OTT mode and use them in
	 * broadcast mode */
	session->ignore_timestamps = -1;

	pthread_mutex_init(&session->lock, NULL);
	pthread_cond_init(&session->cond, NULL);

	for (int i = 0; i < QD_MAX_OUTPUTS; i++) {
		struct qd_output *output = &session->outputs[i];
		output->id = i;
		output->name = qd_output_id_to_str(output->id);
		output->session = session;
	}

	session->handle = qap_session_open(session->type, lib_handle);
	if (!session->handle) {
		err("failed to open qap session");
		goto fail;
	}

	qap_session_set_callback(session->handle, handle_qap_session_event,
				 session);

	return session;

fail:
	qd_session_destroy(session);
	return NULL;
}

struct qd_output *
qd_session_get_output(struct qd_session *session, enum qd_output_id id)
{
	return &session->outputs[id];
}

void
qd_session_set_output_cb(struct qd_session *session, qd_output_func_t func,
			 void *userdata)
{
	session->output_cb_func = func;
	session->output_cb_data = userdata;
}


int
qd_init(void)
{
	qd_base_time = get_time();
	av_log_set_level(get_av_log_level());
	avformat_network_init();
	avdevice_register_all();
	return 0;
}
