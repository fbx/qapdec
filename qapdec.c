#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>

#include <dolby_ms12.h>
#include <dts_m8.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#define print(l, msg, ...)						\
	do {								\
		if (debug_level >= l)					\
			fprintf(stderr, msg, ##__VA_ARGS__);		\
	} while (0)

#define err(msg, ...) \
	print(1, "error: " msg "\n", ##__VA_ARGS__)

#define info(msg, ...) \
	print(2, msg "\n", ##__VA_ARGS__)

#define dbg(msg, ...) \
	print(3, msg "\n", ##__VA_ARGS__)

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

#define CASESTR(ENUM) case ENUM: return #ENUM;

#define ARRAY_SIZE(x)	(sizeof (x) / sizeof (*(x)))

#ifndef QAP_LIB_DTS_M8
# define QAP_LIB_DTS_M8 "libdts_m8_wrapper.so"
#endif

#ifndef QAP_LIB_DOLBY_MS12
# define QAP_LIB_DOLBY_MS12 "/usr/lib64/libdolby_ms12_wrapper.so"
#endif

#define MAX_OUTPUTS 3
#define AUDIO_OUTPUT_ID_BASE 0x100

/* MS12 bitstream output mode */
#define MS12_KEY_BS_OUT_MODE		"bs_out_mode"
#define MS12_BS_OUT_MODE_PCM		0
#define MS12_BS_OUT_MODE_DD		1
#define MS12_BS_OUT_MODE_DDP		2
#define MS12_BS_OUT_MODE_ALL		3

qap_lib_handle_t qap_lib;
qap_session_handle_t qap_session;

int debug_level = 1;
volatile bool quit;

static struct qap_output_ctx {
	const char *name;
	qap_output_config_t config;
	qap_output_delay_t delay;
	bool wav_enabled;
	int wav_channel_count;
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	int wav_block_size;
	uint64_t last_ts;
	uint64_t total_bytes;
	FILE *stream;
} qap_outputs[MAX_OUTPUTS];

static pthread_mutex_t qap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qap_cond = PTHREAD_COND_INITIALIZER;

static bool qap_eos_received;

struct stream {
	const char *name;
	AVStream *avstream;
	qap_module_handle_t module;
	qap_module_flags_t flags;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool buffer_full;
	bool started;
};

#define MAX_STREAMS	2

struct ffmpeg_src {
	AVFormatContext *avctx;
	struct stream *streams[MAX_STREAMS];
	int n_streams;
};

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

static uint64_t
get_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / UINT64_C(1000);
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
	{ WAV_SPEAKER_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_LB },
	{ WAV_SPEAKER_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_RB },
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

static int output_write_header(struct qap_output_ctx *out)
{
	qap_output_config_t *cfg = &out->config;
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	int wav_channel_count = 0;
	struct wav_header hdr;
	uint32_t channel_mask = 0;

	if (out->wav_enabled)
		return 0;

	if (!out->stream)
		return 0;

	switch (out->config.format) {
	case QAP_AUDIO_FORMAT_PCM_16_BIT:
	case QAP_AUDIO_FORMAT_PCM_8_24_BIT:
	case QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED:
	case QAP_AUDIO_FORMAT_PCM_32_BIT:
		break;
	default:
		// nothing to do here
		return 0;
	}

	for (unsigned i = 0; i < ARRAY_SIZE(wav_channel_table); i++) {
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

	out->wav_block_size = hdr.fmt.block_align;
	out->wav_channel_count = wav_channel_count;
	out->wav_enabled = true;

	return 0;
}

static int output_write_buffer(struct qap_output_ctx *out,
			       const qap_buffer_common_t *buffer)
{
	if (!out->stream)
		return 0;

	if (out->wav_enabled) {
		const uint8_t *src;
		int sample_size;
		int n_blocks;

		src = buffer->data;

		assert(buffer->size % out->wav_block_size == 0);
		n_blocks = buffer->size / out->wav_block_size;
		sample_size = out->wav_block_size / out->wav_channel_count;

		for (int i = 0; i < n_blocks; i++) {
			for (int ch = 0; ch < out->wav_channel_count; ch++) {
				fwrite(src + out->wav_channel_offset[ch],
				       sample_size, 1, out->stream);
			}
			src += out->wav_block_size;
		}
	} else {
		fwrite(buffer->data, buffer->size, 1, out->stream);
	}

	return 0;
}

static const char *audio_format_to_str(qap_audio_format_t format)
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

static const char *audio_channel_to_str(qap_pcm_chmap channel)
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

static const char *audio_chmap_to_str(int channels, uint8_t *map)
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

static struct qap_output_ctx *get_qap_output(int index)
{
	assert(index >= AUDIO_OUTPUT_ID_BASE &&
	       index < AUDIO_OUTPUT_ID_BASE + MAX_OUTPUTS);

	return &qap_outputs[index - AUDIO_OUTPUT_ID_BASE];
}

static void handle_buffer(qap_audio_buffer_t *buffer)
{
	struct qap_output_ctx *output =
		get_qap_output(buffer->buffer_parms.output_buf_params.output_id);

	dbg("qap: out %s: pcm buffer size=%u pts=%" PRIi64
	    " duration=%" PRIi64 " last_diff=%" PRIi64,
	    output->name,
	    buffer->common_params.size, buffer->common_params.timestamp,
	    buffer->common_params.size * 1000000UL /
	    (output->config.channels * output->config.bit_width / 8) /
	    output->config.sample_rate,
	    buffer->common_params.timestamp - output->last_ts);

	output->last_ts = buffer->common_params.timestamp;
	output->total_bytes += buffer->common_params.size;
	output_write_buffer(output, &buffer->common_params);
}

static void handle_output_config(qap_output_buff_params_t *out_buffer)
{
	qap_output_config_t *cfg = &out_buffer->output_config;
	struct qap_output_ctx *output = get_qap_output(out_buffer->output_id);

	info("qap: out %s: config: id=0x%x format=%s sr=%d ss=%d "
	     "interleaved=%d channels=%d chmap[%s]",
	     output->name, cfg->id,
	     audio_format_to_str(cfg->format),
	     cfg->sample_rate, cfg->bit_width, cfg->is_interleaved,
	     cfg->channels, audio_chmap_to_str(cfg->channels, cfg->ch_map));

	output->config = *cfg;

	output_write_header(output);
}

static void handle_output_delay(qap_output_delay_t *delay)
{
	struct qap_output_ctx *output = get_qap_output(delay->output_id);
	int log_level;

	if (output->delay.algo_delay == delay->algo_delay &&
	    output->delay.buffering_delay == delay->buffering_delay &&
	    output->delay.non_main_data_length == delay->non_main_data_length &&
	    output->delay.non_main_data_offset == delay->non_main_data_offset)
		return;

	if (output->delay.algo_delay == delay->algo_delay)
		log_level = 3;
	else
		log_level = 2;

	print(log_level, "qap: out %s: delay: "
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

static void handle_qap_session_event(qap_session_handle_t session, void *priv,
				     qap_callback_event_t event_id,
				     int size, void *data)
{
	switch (event_id) {
	case QAP_CALLBACK_EVENT_DATA:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_DATA "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_buffer((qap_audio_buffer_t *)data);
		break;
	case QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_output_config(&((qap_audio_buffer_t *)data)->buffer_parms.output_buf_params);
		break;
	case QAP_CALLBACK_EVENT_EOS:
		info("qap: EOS for primary");
		pthread_mutex_lock(&qap_lock);
		qap_eos_received = true;
		pthread_cond_signal(&qap_cond);
		pthread_mutex_unlock(&qap_lock);
		break;
	case QAP_CALLBACK_EVENT_MAIN_2_EOS:
		info("qap: EOS for secondary");
		break;
	case QAP_CALLBACK_EVENT_EOS_ASSOC:
		info("qap: EOS for assoc");
		break;
	case QAP_CALLBACK_EVENT_ERROR:
		info("qap: error");
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
		handle_output_delay(data);
		break;
	default:
		err("unknown QAP session event %u", event_id);
		break;
	}
}

static void handle_input_config(struct stream *stream, qap_input_config_t *cfg)
{
	info("qap: in %s: format sr=%u ss=%u channels=%u",
	     stream->name, cfg->sample_rate, cfg->bit_width, cfg->channels);
}

static void handle_qap_module_event(qap_module_handle_t module, void *priv,
				    qap_module_callback_event_t event_id,
				    int size, void *data)
{
	struct stream *stream = priv;

	switch (event_id) {
	case QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER:
		if (size != sizeof (qap_send_buffer_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER "
			    "size=%d expected=%zu", size,
			    sizeof (qap_send_buffer_t));
		} else {
			qap_send_buffer_t *buf = data;
			dbg("qap: in %s: %u bytes avail", stream->name,
			    buf->bytes_available);
		}
		pthread_mutex_lock(&stream->lock);
		stream->buffer_full = false;
		pthread_cond_signal(&stream->cond);
		pthread_mutex_unlock(&stream->lock);
		break;
	case QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE:
		if (size != sizeof (qap_input_config_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_input_config_t));
		}
		handle_input_config(stream, data);
		break;
	default:
		err("unknown QAP module event %u", event_id);
		break;
	}
}

static void wait_buffer_available(struct stream *stream)
{
	dbg(" dec: in %s: wait buffer", stream->name);

	pthread_mutex_lock(&stream->lock);
	while (stream->buffer_full)
		pthread_cond_wait(&stream->cond, &stream->lock);
	pthread_mutex_unlock(&stream->lock);
}

static int get_av_log_level(void)
{
	if (debug_level >= 5)
		return AV_LOG_TRACE;
	if (debug_level >= 4)
		return AV_LOG_DEBUG;
	if (debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (debug_level >= 2)
		return AV_LOG_INFO;
	if (debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

static void handle_quit(int sig)
{
	quit = true;
}

static int
format_is_raw(qap_audio_format_t format)
{
	return format == QAP_AUDIO_FORMAT_PCM_16_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_32_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_8_24_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED ||
		format == QAP_AUDIO_FORMAT_AAC;
}

static int
stream_start(struct stream *stream)
{
	int ret;

	if (stream->started)
		return 0;

	info("qap: in %s: start", stream->name);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_START,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("qap: QAP_SESSION_CMD_START command failed");
		return 1;
	}

	stream->started = true;

	return 0;
}

static int
stream_stop(struct stream *stream)
{
	int ret;

	if (!stream->started)
		return 0;

	info("qap: in %s: stop", stream->name);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_STOP,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("qap: QAP_SESSION_CMD_STOP command failed");
		return 1;
	}

	stream->started = false;

	return 0;
}

static int
stream_send_eos(struct stream *stream)
{
	qap_audio_buffer_t qap_buffer;
	int ret;

	if (!stream->started)
		return 0;

	memset(&qap_buffer, 0, sizeof (qap_buffer));
	qap_buffer.buffer_parms.input_buf_params.flags = QAP_BUFFER_EOS;

	ret = qap_module_process(stream->module, &qap_buffer);
	if (ret) {
		err("qap: in %s: failed to send eos, err %d",
		    stream->name, ret);
		return 1;
	}

	return 0;
}

static void
stream_destroy(struct stream *stream)
{
	if (!stream)
		return;

	if (stream->module && qap_module_deinit(stream->module))
		err("qap: failed to deinit %s module", stream->name);

	pthread_cond_destroy(&stream->cond);
	pthread_mutex_destroy(&stream->lock);
	free(stream);
}

static struct stream *
stream_create(AVStream *avstream, qap_module_flags_t qap_flags)
{
	struct stream *stream;
	const char *name;
	qap_audio_format_t qap_format;
	qap_module_config_t qap_mod_cfg;
	AVCodecParameters *codecpar;

	switch (qap_flags) {
	case QAP_MODULE_FLAG_PRIMARY:
		name = "PRIMARY";
		break;
	case QAP_MODULE_FLAG_SECONDARY:
		name = "SECONDARY";
		break;
	case QAP_MODULE_FLAG_SYSTEM_SOUND:
		name = "SYSTEM_SOUND";
		break;
	case QAP_MODULE_FLAG_APP_SOUND:
		name = "APP_SOUND";
		break;
	case QAP_MODULE_FLAG_OTT_SOUND:
		name = "OTT_SOUND";
		break;
	default:
		err("unknown qap module flags %x\n", qap_flags);
		return NULL;
	}

	codecpar = avstream->codecpar;

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

	stream = calloc(1, sizeof *stream);
	if (!stream)
		return NULL;

	stream->name = name;
	stream->avstream = avstream;
	stream->flags = qap_flags;

	pthread_cond_init(&stream->cond, NULL);
	pthread_mutex_init(&stream->lock, NULL);

	memset(&qap_mod_cfg, 0, sizeof (qap_mod_cfg));
	qap_mod_cfg.module_type = QAP_MODULE_DECODER;
	qap_mod_cfg.flags = qap_flags;
	qap_mod_cfg.format = qap_format;

	if (format_is_raw(qap_format)) {
		qap_mod_cfg.channels = codecpar->channels;
		qap_mod_cfg.is_interleaved = true;
		qap_mod_cfg.sample_rate = codecpar->sample_rate;
		qap_mod_cfg.bit_width = codecpar->bits_per_coded_sample;
	}

	if (qap_module_init(qap_session, &qap_mod_cfg, &stream->module)) {
		err("qap: failed to init module");
		goto fail;
	}

	if (qap_module_set_callback(stream->module,
				    handle_qap_module_event, stream)) {
		err("qap: failed to set module callback");
		goto fail;
	}

	if (stream_start(stream))
		goto fail;

	return stream;

fail:
	stream_destroy(stream);
	return NULL;
}

static void
ffmpeg_src_destroy(struct ffmpeg_src *src)
{
	if (!src)
		return;

	for (int i = 0; i < MAX_STREAMS; i++)
		stream_destroy(src->streams[i]);

	if (src->avctx)
		avformat_free_context(src->avctx);

	free(src);
}

static struct ffmpeg_src *
ffmpeg_src_create(const char *url)
{
	struct ffmpeg_src *src;
	int ret;

	src = calloc(1, sizeof *src);
	if (!src)
		return NULL;

	ret = avformat_open_input(&src->avctx, url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", url);
		goto fail;
	}

	ret = avformat_find_stream_info(src->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(src->avctx, -1, url, 0);

	return src;

fail:
	ffmpeg_src_destroy(src);
	return NULL;
}

static uint64_t
ffmpeg_src_get_duration(struct ffmpeg_src *src)
{
	AVRational qap_timebase = { 1, 1000000 };
	return av_rescale_q(src->avctx->duration, AV_TIME_BASE_Q, qap_timebase);
}

static AVStream *
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

static int
ffmpeg_src_map_stream(struct ffmpeg_src *src, int index,
		      qap_module_flags_t qap_flags)
{
	struct stream *stream;

	if (index < 0 || (unsigned int)index >= src->avctx->nb_streams) {
		err("stream index %d does not exist", index);
		return -1;
	}

	if (src->n_streams >= MAX_STREAMS) {
		err("too many streams");
		return -1;
	}

	stream = stream_create(src->avctx->streams[index], qap_flags);
	if (!stream)
		return -1;

	src->streams[src->n_streams++] = stream;
	info("using stream %d as %s", stream->avstream->index, stream->name);

	return 0;
}

static struct stream *
ffmpeg_src_find_stream_by_index(struct ffmpeg_src *src, int index)
{
	for (int i = 0; i < src->n_streams; i++) {
		if (src->streams[i] &&
		    src->streams[i]->avstream->index == index)
			return src->streams[i];
	}

	return NULL;
}

static int
ffmpeg_src_read_frame(struct ffmpeg_src *src)
{
	qap_audio_buffer_t qap_buffer;
	struct stream *stream;
	AVPacket pkt;
	int ret;

	av_init_packet(&pkt);

	/* get next audio frame from ffmpeg */
	ret = av_read_frame(src->avctx, &pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF)
			av_err(ret, "failed to read frame from input");
		return ret;
	}

	/* find out which stream the frame belongs to */
	stream = ffmpeg_src_find_stream_by_index(src, pkt.stream_index);
	if (!stream) {
		av_packet_unref(&pkt);
		return 0;
	}

	/* push the audio frame to the decoder */
	memset(&qap_buffer, 0, sizeof (qap_buffer));
	qap_buffer.common_params.data = pkt.data;
	qap_buffer.common_params.size = pkt.size;
	qap_buffer.common_params.offset = 0;

	if (pkt.pts == AV_NOPTS_VALUE) {
		qap_buffer.common_params.timestamp = 0;
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_NO_TSTAMP;
	} else {
		AVRational av_timebase = stream->avstream->time_base;
		AVRational qap_timebase = { 1, 1000000 };
		qap_buffer.common_params.timestamp =
			av_rescale_q(pkt.pts, av_timebase, qap_timebase);
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_TSTAMP;
	}

	dbg(" in: %s: buffer size=%d pts=%" PRIi64 " -> %" PRIi64,
	    stream->name, pkt.size, pkt.pts,
	    qap_buffer.common_params.timestamp);

	while (qap_buffer.common_params.offset <
	       qap_buffer.common_params.size) {

		pthread_mutex_lock(&stream->lock);
		stream->buffer_full = true;
		pthread_mutex_unlock(&stream->lock);

		ret = qap_module_process(stream->module, &qap_buffer);
		if (ret < 0) {
			if (stream->flags & QAP_MODULE_FLAG_SECONDARY) {
				av_packet_unref(&pkt);
				err("in: %s full, drop", stream->name);
				break;
			}
			wait_buffer_available(stream);
		} else if (ret == 0) {
			err("dec: %s: decoder returned zero size",
			    stream->name);
			break;
		} else {
			qap_buffer.common_params.offset += ret;
			dbg("dec: %s: written %d bytes",
			    stream->name, ret);
		}
	}

	ret = pkt.size;
	av_packet_unref(&pkt);

	return ret;
}

enum output_type {
	OUTPUT_NONE,
	OUTPUT_STEREO,
	OUTPUT_5DOT1,
	OUTPUT_7DOT1,
	OUTPUT_AC3,
	OUTPUT_EAC3,
};

static void usage(void)
{
	fprintf(stderr, "usage: qapdec [OPTS] <input>\n"
		"Where OPTS is a combination of:\n"
		"  -v, --verbose                increase debug verbosity\n"
		"  -p, --primary-stream=<n>     audio primary stream number to decode\n"
		"  -s, --secondary-stream=<n>   audio secondary stream number to decode\n"
		"  -t, --session-type=<type>    session type (broadcast, decode, encode, ott)\n"
		"  -o, --output=<path>          output data to file instead of stdout\n"
		"  -c, --channels=<channels>    maximum number of channels to output\n"
		"  -k, --kvpairs=<kvpairs>      pass kvpairs string to the decoder backend\n"
		"  -l, --loops=<count>          number of times the stream will be decoded\n"
		"\n");
}

static const struct option long_options[] = {
	{ "help",              no_argument,       0, 'h' },
	{ "verbose",           no_argument,       0, 'v' },
	{ "channels",          required_argument, 0, 'c' },
	{ "kvpairs",           required_argument, 0, 'k' },
	{ "loops",             required_argument, 0, 'l' },
	{ "output",            required_argument, 0, 'o' },
	{ "session-type",      required_argument, 0, 't' },
	{ "primary-stream",    required_argument, 0, 'p' },
	{ "secondary-stream",  required_argument, 0, 's' },
	{ 0,                   0,                 0,  0  }
};

int main(int argc, char **argv)
{
	const char *url;
	int opt;
	int ret;
	int decode_err = 0;
	int loops = 1;
	int primary_stream_index = -1;
	int secondary_stream_index = -1;
	enum output_type outputs[MAX_OUTPUTS] = { };
	int num_outputs = 0;
	char *kvpairs = NULL;
	const char *qap_lib_name;
	FILE *output_stream = NULL;
	struct ffmpeg_src *src;
	uint64_t src_duration;
	uint64_t start_time;
	uint64_t end_time;
	uint32_t outputs_present;
	int bs_mode;
	qap_session_outputs_config_t qap_session_cfg;
	qap_session_t qap_session_type;
	AVStream *avstream;

	qap_session_type = QAP_SESSION_BROADCAST;

	while ((opt = getopt_long(argc, argv, "c:hk:l:o:p:s:t:v",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (num_outputs >= MAX_OUTPUTS) {
				err("too many outputs");
				usage();
				return 1;
			}
			if (!strcmp(optarg, "dd") || !strcmp(optarg, "ac3"))
				outputs[num_outputs++] = OUTPUT_AC3;
			else if (!strcmp(optarg, "ddp") || !strcmp(optarg, "eac3"))
				outputs[num_outputs++] = OUTPUT_EAC3;
			else if (!strcmp(optarg, "stereo") || atoi(optarg) == 2)
				outputs[num_outputs++] = OUTPUT_STEREO;
			else if (!strcmp(optarg, "5.1") || atoi(optarg) == 6)
				outputs[num_outputs++] = OUTPUT_5DOT1;
			else if (!strcmp(optarg, "7.1") || atoi(optarg) == 8)
				outputs[num_outputs++] = OUTPUT_7DOT1;
			else {
				err("invalid output %s", optarg);
				usage();
				return 1;
			}
			break;
		case 'v':
			debug_level++;
			break;
		case 'k':
			kvpairs = optarg;
			break;
		case 'l':
			loops = atoi(optarg);
			break;
		case 'p':
			primary_stream_index = atoi(optarg);
			break;
		case 's':
			secondary_stream_index = atoi(optarg);
			break;
		case 'o':
			if (!strcmp(optarg, "-"))
				output_stream = stdout;
			else
				output_stream = fopen(optarg, "w");

			if (!output_stream) {
				err("cannot open file `%s' for writing: %m",
				    optarg);
				return 1;
			}
			break;
		case 't':
			if (!strncmp(optarg, "br", 2))
				qap_session_type = QAP_SESSION_BROADCAST;
			else if (!strncmp(optarg, "dec", 3))
				qap_session_type = QAP_SESSION_DECODE_ONLY;
			else if (!strncmp(optarg, "enc", 3))
				qap_session_type = QAP_SESSION_ENCODE_ONLY;
			else if (!strncmp(optarg, "ott", 3))
				qap_session_type = QAP_SESSION_MS12_OTT;
			else {
				err("invalid session type %s", optarg);
				usage();
				return 1;
			}
			break;
		case 'h':
			usage();
			return 0;
		default:
			err("unknown option %c", opt);
			usage();
			return 1;
		}
	}

	if (outputs[0] == OUTPUT_NONE)
		outputs[0] = OUTPUT_7DOT1;

	if (optind >= argc) {
		err("missing url to play\n");
		usage();
		return 1;
	}

	url = argv[optind];

	av_log_set_level(get_av_log_level());
	avformat_network_init();

again:
	info("QAP library version %u", qap_get_version());

	memset(qap_outputs, 0, sizeof (qap_outputs));
	qap_outputs[0].stream = output_stream;
	qap_eos_received = false;

	/* init ffmpeg source and demuxer */
	src = ffmpeg_src_create(url);
	if (!src)
		return 1;

	src_duration = ffmpeg_src_get_duration(src);

	avstream = ffmpeg_src_get_avstream(src, primary_stream_index);
	if (!avstream) {
		err("primary stream not found in %s", url);
		return 1;
	}

	/* load QAP library */
	switch (avstream->codecpar->codec_id) {
	case AV_CODEC_ID_AC3:
	case AV_CODEC_ID_EAC3:
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_AAC_LATM:
	case AV_CODEC_ID_PCM_S16LE:
	case AV_CODEC_ID_PCM_S24LE:
	case AV_CODEC_ID_PCM_S32LE:
		qap_lib_name = QAP_LIB_DOLBY_MS12;
		break;
	case AV_CODEC_ID_DTS:
		qap_lib_name = QAP_LIB_DTS_M8;
		break;
	default:
		err("cannot decode %s format",
		    avcodec_get_name(avstream->codecpar->codec_id));
		return 1;
	}

	if (!qap_lib) {
		qap_lib = qap_load_library(qap_lib_name);
		if (!qap_lib) {
			err("qap: failed to load library %s", qap_lib_name);
			return 1;
		}
	}

	/* init QAP session */
	qap_session = qap_session_open(qap_session_type, qap_lib);
	if (!qap_session) {
		err("qap: failed to open session");
		return 1;
	}

	qap_session_set_callback(qap_session, handle_qap_session_event, NULL);

	outputs_present = 0;
	memset(&qap_session_cfg, 0, sizeof (qap_session_cfg));
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		struct qap_output_ctx *output =
			&qap_outputs[qap_session_cfg.num_output];
		qap_output_config_t *output_cfg =
			&qap_session_cfg.output_config[qap_session_cfg.num_output];
		switch (outputs[i]) {
		case OUTPUT_NONE:
			continue;
		case OUTPUT_STEREO:
			output->name = "STEREO";
			output_cfg->channels = 2;
			break;
		case OUTPUT_5DOT1:
			output->name = "5DOT1";
			output_cfg->channels = 6;
			break;
		case OUTPUT_7DOT1:
			output->name = "7DOT1";
			output_cfg->channels = 8;
			break;
		case OUTPUT_AC3:
			output->name = "AC3";
			output_cfg->format = QAP_AUDIO_FORMAT_AC3;
			break;
		case OUTPUT_EAC3:
			output->name = "EAC3";
			output_cfg->format = QAP_AUDIO_FORMAT_EAC3;
			break;
		}

		outputs_present |= 1 << outputs[i];
		output_cfg->id = AUDIO_OUTPUT_ID_BASE +
			qap_session_cfg.num_output++;
	}

	/* set BS mode for MS12 */
	if (!strcmp(qap_lib_name, QAP_LIB_DOLBY_MS12)) {
		char params[32];

		if (outputs_present & (1 << OUTPUT_EAC3))
			bs_mode = MS12_BS_OUT_MODE_DDP;
		else if (outputs_present & (1 << OUTPUT_AC3))
			bs_mode = MS12_BS_OUT_MODE_DD;
		else
			bs_mode = MS12_BS_OUT_MODE_PCM;

		sprintf(params, "%s=%d", MS12_KEY_BS_OUT_MODE, bs_mode);

		ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_KVPAIRS,
				      strlen(params) + 1, params, NULL, NULL);
		if (ret) {
			err("qap: QAP_SESSION_CMD_SET_KVPAIRS command failed");
			return 1;
		}
	}

	/* setup outputs */
	ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_OUTPUTS,
			      sizeof (qap_session_cfg), &qap_session_cfg,
			      NULL, NULL);
	if (ret) {
		err("qap: QAP_SESSION_CMD_SET_CONFIG command failed");
		return 1;
	}

	/* apply user kvpairs */
	if (kvpairs) {
		ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_KVPAIRS,
				      strlen(kvpairs) + 1, kvpairs, NULL, NULL);
		if (ret) {
			err("qap: QAP_SESSION_CMD_SET_KVPAIRS command failed");
			return 1;
		}
	}

	start_time = get_time();

	/* create primary QAP module */
	ret = ffmpeg_src_map_stream(src, avstream->index,
				    QAP_MODULE_FLAG_PRIMARY);
	if (ret)
		return 1;

	/* create secondary QAP module */
	if (secondary_stream_index != -1) {
		ret = ffmpeg_src_map_stream(src, secondary_stream_index,
					    QAP_MODULE_FLAG_SECONDARY);
		if (ret)
			return 1;
	}

	signal(SIGINT, handle_quit);
	signal(SIGTERM, handle_quit);

	while (!quit) {
		ret = ffmpeg_src_read_frame(src);
		if (ret == AVERROR_EOF)
			break;

		if (ret < 0) {
			quit = 1;
			decode_err = 1;
			break;
		}
	}

	if (!decode_err) {
		/* send EOS and stop all streams */
		for (int i = 0; i < MAX_STREAMS; i++) {
			if (!src->streams[i])
				continue;
			stream_send_eos(src->streams[i]);
			stream_stop(src->streams[i]);
		}

		info(" in: sent EOS");

		/* wait EOS */
		pthread_mutex_lock(&qap_lock);
		while (!qap_eos_received)
			pthread_cond_wait(&qap_cond, &qap_lock);
		pthread_mutex_unlock(&qap_lock);
	}

	/* cleanup */
	ffmpeg_src_destroy(src);
	src = NULL;

	if (qap_session_close(qap_session))
		err("qap: failed to close session");

	end_time = get_time();

	for (int i = 0; i < MAX_OUTPUTS; i++) {
		struct qap_output_ctx *output = &qap_outputs[i];
		uint64_t frames;
		uint64_t duration;

		if (output->total_bytes == 0)
			continue;

		duration = end_time - start_time;
		frames = output->total_bytes / (output->config.channels *
						output->config.bit_width / 8);

		info("out: %s: %" PRIu64 " bytes, %" PRIu64 " frames, "
		     "speed: %" PRIu64 "kB/sec, %" PRIu64 " frames/sec",
		     output->name, output->total_bytes, frames,
		     output->total_bytes * 1000 / duration,
		     frames * 1000000 / duration);
	}

	if (!decode_err) {
		info("render speed: %.2fx realtime",
		     (float)src_duration / (float)(end_time - start_time));
	}

	if (!quit && --loops > 0)
		goto again;

	if (qap_unload_library(qap_lib))
		err("qap: failed to unload library");

	if (output_stream && output_stream != stdout)
		fclose(output_stream);

	return decode_err;
}
