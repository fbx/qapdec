#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <assert.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>

#include <dolby_ms12.h>
#include <dts_m8.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>

#define print(l, msg, ...)						\
	do {								\
		if (debug_level >= l)					\
			fprintf(stderr, "[%08" PRIu64 "] " msg, (get_time() - clock_base_time) / 1000, ##__VA_ARGS__);		\
	} while (0)

#define err(msg, ...) \
	print(1, "error: " msg "\n", ##__VA_ARGS__)

#define notice(msg, ...) \
	print(1, msg "\n", ##__VA_ARGS__)

#define info(msg, ...) \
	print(2, msg "\n", ##__VA_ARGS__)

#define dbg(msg, ...) \
	print(3, msg "\n", ##__VA_ARGS__)

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

#define CASESTR(ENUM) case ENUM: return #ENUM;

#define ARRAY_SIZE(x)	(sizeof (x) / sizeof (*(x)))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#ifndef QAP_LIB_DTS_M8
# define QAP_LIB_DTS_M8 "libdts_m8_wrapper.so"
#endif

#ifndef QAP_LIB_DOLBY_MS12
# define QAP_LIB_DOLBY_MS12 "/usr/lib64/libdolby_ms12_wrapper_prod.so"
#endif

#define AUDIO_OUTPUT_ID_BASE 0x100

#define ADTS_HEADER_SIZE 7

/* MS12 bitstream output mode */
#define MS12_KEY_BS_OUT_MODE		"bs_out_mode"
#define MS12_BS_OUT_MODE_PCM		0
#define MS12_BS_OUT_MODE_DD		1
#define MS12_BS_OUT_MODE_DDP		2
#define MS12_BS_OUT_MODE_ALL		3

qap_lib_handle_t qap_lib;
qap_session_handle_t qap_session;
unsigned int qap_outputs_configure_count;
uint64_t clock_base_time;

int debug_level = 1;
bool opt_render_realtime;
char *opt_output_dir;
volatile bool quit;
volatile bool primary_done;

enum output_type {
	OUTPUT_NONE = -1,
	OUTPUT_STEREO = 0,
	OUTPUT_5DOT1,
	OUTPUT_7DOT1,
	OUTPUT_AC3,
	OUTPUT_EAC3,
	MAX_OUTPUTS,
};

enum module_type {
	MODULE_PRIMARY,
	MODULE_SECONDARY,
	MODULE_SYSTEM_SOUND,
	MODULE_APP_SOUND,
	MODULE_OTT_SOUND,
	MAX_MODULES,
};

static struct qap_output_ctx {
	const char *name;
	qap_output_config_t config;
	qap_output_delay_t delay;
	bool enabled;
	bool discont;
	bool wav_enabled;
	int wav_channel_count;
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	uint64_t last_ts;
	uint64_t total_bytes;
	uint64_t start_time;
	FILE *stream;
} qap_outputs[MAX_OUTPUTS];

static pthread_mutex_t qap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qap_cond = PTHREAD_COND_INITIALIZER;

static bool qap_eos_received;
static const char *qap_lib_name;
static bool qap_chmod_locking;

enum kbd_command {
	KBD_NONE,
	KBD_PLAYPAUSE,
	KBD_STOP,
	KBD_BLOCK,
};

static int kbd_ev = -1;
static enum kbd_command kbd_pending_command = KBD_NONE;

enum stream_state {
	STREAM_STATE_STOPPED,
	STREAM_STATE_STARTED,
	STREAM_STATE_PAUSED,
};

struct stream {
	const char *name;
	AVStream *avstream;
	AVFormatContext *avmux;
	uint8_t adts_header[ADTS_HEADER_SIZE];
	bool insert_adts_header;
	qap_module_handle_t module;
	qap_module_flags_t flags;
	qap_input_config_t config;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int buffer_size;
	bool buffer_full;
	bool terminated;
	bool blocked;
	enum stream_state state;
	uint64_t start_time;
	uint64_t state_change_time;
	uint64_t written_bytes;
};

#define MAX_STREAMS	2

struct ffmpeg_src {
	AVFormatContext *avctx;
	struct stream *streams[MAX_STREAMS];
	int n_streams;
	pthread_t tid;
};

static struct ffmpeg_src *qap_inputs[MAX_MODULES];

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

static const char *audio_format_extension(qap_audio_format_t format)
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

static int
format_is_pcm(qap_audio_format_t format)
{
	return format == QAP_AUDIO_FORMAT_PCM_16_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_32_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_8_24_BIT ||
		format == QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED;
}

static int
format_is_raw(qap_audio_format_t format)
{
	return format_is_pcm(format) ||
		format == QAP_AUDIO_FORMAT_AAC;
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

static int output_write_header(struct qap_output_ctx *out)
{
	qap_output_config_t *cfg = &out->config;
	char filename[PATH_MAX];
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	int wav_channel_count = 0;
	struct wav_header hdr;
	uint32_t channel_mask = 0;

	if (!opt_output_dir)
		return 0;

	if (out->discont) {
		if (out->stream)
			fclose(out->stream);
		out->stream = NULL;
		out->discont = false;
	}

	if (out->stream)
		return 0;

	snprintf(filename, sizeof (filename),
		 "%s/%03u.%s.%s", opt_output_dir,
		 qap_outputs_configure_count, out->name,
		 audio_format_extension(out->config.format));

	out->stream = fopen(filename, "w");
	if (!out->stream) {
		err("failed to create output file %s: %m", filename);
		return -1;
	}

	if (!format_is_pcm(out->config.format)) {
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

static void handle_buffer(qap_audio_buffer_t *buffer)
{
	int id = buffer->buffer_parms.output_buf_params.output_id;
	struct qap_output_ctx *output = get_qap_output(id);

	dbg("out: %s: pcm buffer size=%u pts=%" PRIi64
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

	/* only wait on PCM outputs */
	if (opt_render_realtime && format_is_pcm(output->config.format)) {
		uint64_t now, pts;
		int64_t delay;

		now = get_time() - output->start_time;
		pts = output->total_bytes * 1000000 /
			(output->config.bit_width / 8 *
			 output->config.channels) /
			output->config.sample_rate;
		delay = pts - now;

		if (delay <= 0)
			return;

		dbg("out: %s: wait %" PRIi64 "us for sync",
		    output->name, delay);

		usleep(delay);
	}
}

static void handle_output_config(qap_output_buff_params_t *out_buffer)
{
	qap_output_config_t *cfg = &out_buffer->output_config;
	struct qap_output_ctx *output = get_qap_output(out_buffer->output_id);

	info("out: %s: config: id=0x%x format=%s sr=%d ss=%d "
	     "interleaved=%d channels=%d chmap[%s]",
	     output->name, cfg->id,
	     audio_format_to_str(cfg->format),
	     cfg->sample_rate, cfg->bit_width, cfg->is_interleaved,
	     cfg->channels, audio_chmap_to_str(cfg->channels, cfg->ch_map));

	output->config = *cfg;

	if (!output->start_time)
		output->start_time = get_time();

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
		log_level = 4;
	else
		log_level = 3;

	print(log_level, "out: %s: delay: "
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
	info(" in: %s: format sr=%u ss=%u channels=%u",
	     stream->name, cfg->sample_rate, cfg->bit_width, cfg->channels);

	stream->config = *cfg;
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
			dbg(" in: %s: %u bytes avail", stream->name,
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
	dbg(" in: %s: wait buffer", stream->name);

	pthread_mutex_lock(&stream->lock);
	while (!stream->terminated && stream->buffer_full) {
		struct timespec delay;

		clock_gettime(CLOCK_REALTIME, &delay);
		delay.tv_sec++;

		if (pthread_cond_timedwait(&stream->cond, &stream->lock,
					   &delay) == ETIMEDOUT &&
		    stream->state == STREAM_STATE_STARTED) {
			err("%s: stalled, buffer has been full for 1 second",
			    stream->name);
		}
	}
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

static int
stream_start(struct stream *stream)
{
	int ret;

	if (stream->state == STREAM_STATE_STARTED) {
		info(" in: %s: already started", stream->name);
		return 0;
	}

	info(" in: %s: start", stream->name);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_START,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_START command failed");
		return 1;
	}

	stream->state = STREAM_STATE_STARTED;
	stream->state_change_time = get_time();

	return 0;
}

static int
stream_pause(struct stream *stream)
{
	int ret;

	if (stream->state != STREAM_STATE_STARTED) {
		info(" in: %s: cannot pause, not started", stream->name);
		return 0;
	}

	info(" in: %s: pause", stream->name);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_PAUSE,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_PAUSE command failed");
		return 1;
	}

	stream->state = STREAM_STATE_PAUSED;
	stream->state_change_time = get_time();

	return 0;
}

static int
stream_stop(struct stream *stream)
{
	int ret;

	if (stream->state == STREAM_STATE_STOPPED) {
		info(" in: %s: already stopped", stream->name);
		return 0;
	}

	info(" in: %s: stop", stream->name);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_STOP,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_STOP command failed");
		return 1;
	}

	stream->state = STREAM_STATE_STOPPED;
	stream->state_change_time = get_time();

	return 0;
}

static int
stream_block(struct stream *stream, bool block)
{
	pthread_mutex_lock(&stream->lock);
	stream->blocked = block;
	pthread_cond_signal(&stream->cond);
	pthread_mutex_unlock(&stream->lock);

	return 0;
}

static int
stream_send_eos(struct stream *stream)
{
	qap_audio_buffer_t qap_buffer;
	int ret;

	if (stream->state == STREAM_STATE_STOPPED)
		return 0;

	memset(&qap_buffer, 0, sizeof (qap_buffer));
	qap_buffer.buffer_parms.input_buf_params.flags = QAP_BUFFER_EOS;

	ret = qap_module_process(stream->module, &qap_buffer);
	if (ret) {
		err("%s: failed to send eos, err %d",
		    stream->name, ret);
		return 1;
	}

	return 0;
}

static uint32_t
stream_get_buffer_size(struct stream *stream)
{
	uint32_t param_id = MS12_STREAM_GET_PCM_INPUT_BUF_SIZE;
	uint32_t buffer_size = 0;
	uint32_t reply_size = sizeof (buffer_size);
	int ret;

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_GET_PARAM,
			     sizeof (param_id), &param_id,
			     &reply_size, &buffer_size);
	if (ret < 0) {
		err("%s: failed to get buffer size", stream->name);
		return 0;
	}

	return buffer_size;
}

static int
stream_set_buffer_size(struct stream *stream, uint32_t buffer_size)
{
	uint32_t params[] = { MS12_STREAM_SET_PCM_INPUT_BUF_SIZE, buffer_size };
	uint32_t new_buffer_size;
	int ret;

	info(" in: %s: set buffer size %u bytes", stream->name, buffer_size);

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_SET_PARAM,
			     sizeof (params), params, NULL, NULL);
	if (ret < 0) {
		err("%s: failed to set buffer size %u", stream->name,
		    buffer_size);
		return -1;
	}

	new_buffer_size = stream_get_buffer_size(stream);
	if (buffer_size != new_buffer_size) {
		err("%s: buffer size %u not set, actual size is %u bytes",
		    stream->name, buffer_size, new_buffer_size);
		return -1;
	}

	return 0;
}

static uint64_t
stream_get_decoded_frames(struct stream *stream)
{
	uint32_t param_id = MS12_STREAM_GET_DECODER_OUTPUT_FRAME;
	uint32_t bytes_consumed = 0;
	uint32_t reply_size = sizeof (bytes_consumed);
	int ret;

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_GET_PARAM,
			     sizeof (param_id), &param_id,
			     &reply_size, &bytes_consumed);
	if (ret < 0) {
		err("%s: failed to get decoded frames", stream->name);
		return 0;
	}

	assert(reply_size == sizeof (bytes_consumed));

	return bytes_consumed / stream->config.sample_rate;
}

static int
stream_get_input_markers(struct stream *stream, qap_marker *marker)
{
	uint32_t param_id = MS12_STREAM_GET_CONSUMED_FRAMES;
	uint32_t reply_size = sizeof (*marker);
	int ret;

	if (!marker)
		return -1;

	ret = qap_module_cmd(stream->module, QAP_MODULE_CMD_GET_PARAM,
			     sizeof (param_id), &param_id,
			     &reply_size, marker);
	if (ret < 0) {
		err("%s: failed to get consumed frames", stream->name);
		return -1;
	}

	assert(reply_size == sizeof (*marker));

	return 0;
}

static void
stream_terminate(struct stream *stream)
{
	pthread_mutex_lock(&stream->lock);
	stream->terminated = true;
	pthread_cond_signal(&stream->cond);
	pthread_mutex_unlock(&stream->lock);
}

static void
stream_destroy(struct stream *stream)
{
	if (!stream)
		return;

	if (stream->module && qap_module_deinit(stream->module))
		err("failed to deinit %s module", stream->name);

	if (stream->avmux)
		avformat_free_context(stream->avmux);

	pthread_cond_destroy(&stream->cond);
	pthread_mutex_destroy(&stream->lock);
	free(stream);
}

static struct stream *
stream_create(AVStream *avstream, qap_module_flags_t qap_flags)
{
	struct stream *stream;
	const char *name;
	char channel_layout_desc[32];
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

	av_get_channel_layout_string(channel_layout_desc,
				     sizeof (channel_layout_desc),
				     codecpar->channels,
				     codecpar->channel_layout);

	if (format_is_pcm(qap_format)) {
		notice("use stream %d as %s, %s, %d Hz, %s, %d bits, %" PRIi64 " kb/s",
		       avstream->id, stream->name,
		       avcodec_get_name(codecpar->codec_id),
		       codecpar->sample_rate,
		       channel_layout_desc,
		       codecpar->bits_per_coded_sample,
		       codecpar->bit_rate / 1000);
	} else {
		notice("use stream %d as %s, %s, %d Hz, %s, %" PRIi64 " kb/s",
		       avstream->id, stream->name,
		       avcodec_get_name(codecpar->codec_id),
		       codecpar->sample_rate,
		       channel_layout_desc,
		       codecpar->bit_rate / 1000);
	}

	if (format_is_raw(qap_format)) {
		qap_mod_cfg.channels = codecpar->channels;
		qap_mod_cfg.is_interleaved = true;
		qap_mod_cfg.sample_rate = codecpar->sample_rate;
		qap_mod_cfg.bit_width = codecpar->bits_per_coded_sample;
	}

	if (qap_module_init(qap_session, &qap_mod_cfg, &stream->module)) {
		err("failed to init module");
		goto fail;
	}

	if (qap_module_set_callback(stream->module,
				    handle_qap_module_event, stream)) {
		err("failed to set module callback");
		goto fail;
	}

	if (codecpar->codec_id == AV_CODEC_ID_AAC &&
	    codecpar->extradata_size >= 2) {
		uint16_t config;
		int obj_type, rate_idx, channels_idx;

		config = be16toh(*(uint16_t *)codecpar->extradata);
		obj_type = (config & 0xf800) >> 11;
		rate_idx = (config & 0x0780) >> 7;
		channels_idx = (config & 0x0078) >> 3;

		if (obj_type <= 3 && rate_idx == 15) {
			/* prepare ADTS header for MAIN, LC, SSR profiles */
			stream->adts_header[0] = 0xff;
			stream->adts_header[1] = 0xf9;
			stream->adts_header[2] = obj_type << 6;
			stream->adts_header[2] |= rate_idx << 2;
			stream->adts_header[2] |= (channels_idx & 4) >> 2;
			stream->adts_header[3] = (channels_idx & 3) << 6;
			stream->adts_header[4] = 0;
			stream->adts_header[5] = 0x1f;
			stream->adts_header[6] = 0x1c;
			stream->insert_adts_header = true;
		} else {
			/* otherwise try to use LATM muxer, which also supports
			 * SBR and ALS */
			AVStream *mux_stream;

			int ret = avformat_alloc_output_context2(&stream->avmux,
								 NULL, "latm",
								 NULL);
			if (ret < 0) {
				av_err(ret, "failed to create latm mux");
				goto fail;
			}

			mux_stream = avformat_new_stream(stream->avmux, NULL);
			if (!mux_stream) {
				err("failed to create latm stream");
				goto fail;
			}

			mux_stream->time_base = stream->avstream->time_base;
			avcodec_parameters_copy(mux_stream->codecpar,
						stream->avstream->codecpar);

			ret = avformat_write_header(stream->avmux, NULL);
			if (ret < 0) {
				av_err(ret, "failed to write latm header");
				goto fail;
			}
		}
	}

	if (format_is_pcm(qap_format)) {
		uint32_t buffer_size;

		buffer_size = stream_get_buffer_size(stream);
		if (buffer_size > 0) {
			info(" in: %s: default buffer size %u bytes",
			     stream->name, buffer_size);
		}

		/* set 32ms buffer size */
		buffer_size = qap_mod_cfg.sample_rate *
			qap_mod_cfg.channels * qap_mod_cfg.bit_width / 8 *
			32 / 1000;

		if (stream_set_buffer_size(stream, buffer_size))
			goto fail;

		stream->buffer_size = buffer_size;
	}

	if (stream_start(stream))
		goto fail;

	return stream;

fail:
	stream_destroy(stream);
	return NULL;
}

static int
stream_write(struct stream *stream, void *data, int size, int64_t pts)
{
	qap_audio_buffer_t qap_buffer;
	qap_marker marker;
	int offset = 0;
	int ret;

	if (stream->written_bytes == 0)
		stream->start_time = get_time();

	memset(&qap_buffer, 0, sizeof (qap_buffer));

	if (pts == AV_NOPTS_VALUE) {
		qap_buffer.common_params.timestamp = 0;
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_NO_TSTAMP;
	} else {
		if (stream->avstream->start_time != AV_NOPTS_VALUE)
			pts -= stream->avstream->start_time;
		AVRational av_timebase = stream->avstream->time_base;
		AVRational qap_timebase = { 1, 1000000 };
		qap_buffer.common_params.timestamp =
			av_rescale_q(pts, av_timebase, qap_timebase);
		qap_buffer.buffer_parms.input_buf_params.flags =
			QAP_BUFFER_TSTAMP;
	}

	if (opt_render_realtime &&
	    qap_buffer.buffer_parms.input_buf_params.flags == QAP_BUFFER_TSTAMP) {
		uint64_t now;
		int64_t delay;

		/* throttle input in real time mode */
		now = get_time() - stream->start_time;
		delay = qap_buffer.common_params.timestamp - now - 10000;

		if (delay > 0) {
			dbg(" in: %s: wait %" PRIi64 "us",
			    stream->name, delay);
			usleep(delay);
		}
	}

	dbg(" in: %s: buffer size=%d pts=%" PRIi64 " -> %" PRIi64,
	    stream->name, size, pts, qap_buffer.common_params.timestamp);

	while (!stream->terminated && offset < size) {
		uint64_t t;

		qap_buffer.common_params.offset = 0;
		qap_buffer.common_params.data = data + offset;
		qap_buffer.common_params.size = size - offset;

		if (stream->buffer_size > 0 &&
		    qap_buffer.common_params.size > stream->buffer_size)
			qap_buffer.common_params.size = stream->buffer_size;

		pthread_mutex_lock(&stream->lock);
		stream->buffer_full = true;
		pthread_mutex_unlock(&stream->lock);

		t = get_time();

		ret = qap_module_process(stream->module, &qap_buffer);
		if (ret < 0) {
			dbg(" in: %s: full, %" PRIu64 " bytes written",
			    stream->name, stream->written_bytes);
			wait_buffer_available(stream);
		} else if (ret == 0) {
			err("%s: decoder returned zero size",
			    stream->name);
			break;
		} else {
			offset += ret;
			stream->written_bytes += ret;

			dbg(" in: %s: written %d bytes in %dus, total %" PRIu64,
			    stream->name, ret, (int)(get_time() - t),
			    stream->written_bytes);

			qap_buffer.common_params.timestamp = 0;
			qap_buffer.buffer_parms.input_buf_params.flags =
				QAP_BUFFER_TSTAMP_CONTINUE;

			assert(offset <= size);
		}
	}

	if (stream->terminated)
		return -1;

	if (stream->flags == QAP_MODULE_FLAG_PRIMARY) {
		dbg(" in: %s: generated %" PRIu64 " frames", stream->name,
		    stream_get_decoded_frames(stream));
	}

	if (!stream_get_input_markers(stream, &marker)) {
		dbg(" in: %s: input marker from=%llu to=%llu", stream->name,
		    marker.from_marker, marker.to_marker);
	}

	return size;
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
	AVStream *avstream;
	struct stream *stream;

	avstream = ffmpeg_src_get_avstream(src, index);
	if (!avstream) {
		err("stream index %d is not usable", index);
		return -1;
	}

	if (src->n_streams >= MAX_STREAMS) {
		err("too many streams");
		return -1;
	}

	stream = stream_create(avstream, qap_flags);
	if (!stream)
		return -1;

	src->streams[src->n_streams++] = stream;

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
		ret = 0;
		goto out;
	}

	pthread_mutex_lock(&stream->lock);
	while (stream->blocked && !stream->terminated)
		pthread_cond_wait(&stream->cond, &stream->lock);
	pthread_mutex_unlock(&stream->lock);

	if (stream->insert_adts_header) {
		/* packets should have AV_INPUT_BUFFER_PADDING_SIZE padding in
		 * them, so we can use that to avoid a copy */
		memmove(pkt.data + ADTS_HEADER_SIZE, pkt.data, pkt.size);
		memcpy(pkt.data, stream->adts_header, ADTS_HEADER_SIZE);
		pkt.size += ADTS_HEADER_SIZE;

		/* patch ADTS header with frame size */
		pkt.data[3] |= pkt.size >> 11;
		pkt.data[4] |= pkt.size >> 3;
		pkt.data[5] |= (pkt.size & 0x07) << 5;

		/* push the audio frame to the decoder */
		ret = stream_write(stream, pkt.data, pkt.size, pkt.pts);

	} else if (stream->avmux) {
		AVIOContext *avio;
		uint8_t *data;
		int size;

		ret = avio_open_dyn_buf(&avio);
		if (ret < 0) {
			av_err(ret, "failed to create avio context");
			goto out;
		}

		stream->avmux->pb = avio;

		pkt.stream_index = 0;
		ret = av_write_frame(stream->avmux, &pkt);
		if (ret < 0) {
			av_err(ret, "failed to mux data");
			goto out;
		}

		size = avio_close_dyn_buf(stream->avmux->pb, &data);
		stream->avmux->pb = NULL;

		ret = stream_write(stream, data, size, pkt.pts);
		av_free(data);
	} else {
		/* push the audio frame to the decoder */
		ret = stream_write(stream, pkt.data, pkt.size, pkt.pts);
	}

	if (stream->state == STREAM_STATE_PAUSED &&
	    get_time() - stream->state_change_time > 1000000) {
		stream->state_change_time = get_time();
		err("%s: input still being consumed 1 second after pause",
		    stream->name);
	}

out:
	av_packet_unref(&pkt);
	return ret;
}

static void *
ffmpeg_src_thread_func(void *userdata)
{
	struct ffmpeg_src *src = userdata;
	int ret;

	while (!quit && !primary_done) {
		ret = ffmpeg_src_read_frame(src);
		if (ret == AVERROR_EOF) {
			info(" in: %s: EOS", src->streams[0]->name);
			break;
		}

		if (ret < 0)
			return (void *)1;
	}

	return 0;
}

static int
ffmpeg_src_thread_start(struct ffmpeg_src *src)
{
	return pthread_create(&src->tid, NULL, ffmpeg_src_thread_func, src);
}

static void
ffmpeg_src_thread_stop(struct ffmpeg_src *src)
{
	for (int i = 0; i < src->n_streams; i++)
		stream_terminate(src->streams[i]);
}

static int
ffmpeg_src_thread_join(struct ffmpeg_src *src)
{
	void *ret = (void *)1;

	pthread_join(src->tid, &ret);

	return (intptr_t)ret;
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

	print(dbg_level, "%.*s\n", len, msg);
}

static void
init_outputs(void)
{
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		struct qap_output_ctx *output = &qap_outputs[i];

		memset(output, 0, sizeof (*output));

		switch (i) {
		case OUTPUT_STEREO:
			output->name = "STEREO";
			break;
		case OUTPUT_5DOT1:
			output->name = "5DOT1";
			break;
		case OUTPUT_7DOT1:
			output->name = "7DOT1";
			break;
		case OUTPUT_AC3:
			output->name = "AC3";
			break;
		case OUTPUT_EAC3:
			output->name = "EAC3";
			break;
		}
	}
}

static void
shutdown_outputs(void)
{
	for (int i = 0; i < MAX_OUTPUTS; i++) {
		struct qap_output_ctx *output = &qap_outputs[i];

		if (output->stream)
			fclose(output->stream);
	}
}

static int
configure_outputs(int num_outputs, enum output_type *outputs)
{
	qap_session_outputs_config_t qap_session_cfg;
	uint32_t outputs_present = 0;
	int ret;

	memset(&qap_session_cfg, 0, sizeof (qap_session_cfg));

	for (int i = 0; i < num_outputs; i++) {
		enum output_type id = outputs[i];
		qap_output_config_t *output_cfg =
			qap_session_cfg.output_config +
			qap_session_cfg.num_output;

		switch (id) {
		case OUTPUT_STEREO:
			output_cfg->channels = 2;
			break;
		case OUTPUT_5DOT1:
			output_cfg->channels = 6;
			break;
		case OUTPUT_7DOT1:
			output_cfg->channels = 8;
			break;
		case OUTPUT_AC3:
			output_cfg->format = QAP_AUDIO_FORMAT_AC3;
			break;
		case OUTPUT_EAC3:
			output_cfg->format = QAP_AUDIO_FORMAT_EAC3;
			break;
		default:
			continue;
		}

		outputs_present |= 1 << id;
		output_cfg->id = AUDIO_OUTPUT_ID_BASE + id;
		qap_session_cfg.num_output++;
	}

	/* set BS mode for MS12 */
	if (!strcmp(qap_lib_name, QAP_LIB_DOLBY_MS12)) {
		int bs_mode;
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
			err("QAP_SESSION_CMD_SET_KVPAIRS command failed");
			return 1;
		}
	}

	qap_outputs_configure_count++;

	for (int i = 0; i < MAX_OUTPUTS; i++) {
		struct qap_output_ctx *output = &qap_outputs[i];
		bool enabled = outputs_present & (1 << i);

		output->discont = enabled != output->enabled;
		output->enabled = enabled;

		if (!output->enabled && output->stream)
			fflush(output->stream);
	}

	/* setup outputs */
	ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_OUTPUTS,
			      sizeof (qap_session_cfg), &qap_session_cfg,
			      NULL, NULL);
	if (ret) {
		err("QAP_SESSION_CMD_SET_CONFIG command failed");
		return 1;
	}

	return 0;
}

static struct stream *get_nth_stream(int n)
{
	struct ffmpeg_src *src;
	int index = -1;

	if (n < 0)
		return NULL;

	for (int i = 0; i < MAX_MODULES; i++) {
		src = qap_inputs[i];
		if (src == NULL)
			continue;

		index += src->n_streams;
		if (index >= n)
			return src->streams[index - n];
	}

	return NULL;
}

static void kbd_handle_key(char key[3])
{
	enum kbd_command cmd = kbd_pending_command;

	kbd_pending_command = KBD_NONE;

	if (key[0] >= '1' && key[0] <= '9') {
		struct stream *stream;
		int index;

		if (cmd == KBD_NONE)
			return;

		index = key[0] - '0';

		stream = get_nth_stream(index - 1);
		if (!stream) {
			err("stream %d not found", index);
			return;
		}

		switch (cmd) {
		case KBD_PLAYPAUSE:
			if (stream->state == STREAM_STATE_STARTED)
				stream_pause(stream);
			else
				stream_start(stream);
			break;
		case KBD_STOP:
			stream_stop(stream);
			break;
		case KBD_BLOCK:
			stream_block(stream, !stream->blocked);
			break;
		default:
			break;
		}
	}

	if (key[0] == 'p') {
		kbd_pending_command = KBD_PLAYPAUSE;
		notice("Enter stream number to send Play/Pause to");

	} else if (key[0] == 's') {
		kbd_pending_command = KBD_STOP;
		notice("Enter stream number to Stop");

	} else if (key[0] == 'b') {
		kbd_pending_command = KBD_BLOCK;
		notice("Enter stream number to Block/Unblock");

	} else if (key[0] == 'c') {
		char kvpairs[32];
		int val, ret;

		val = !qap_chmod_locking;
		snprintf(kvpairs, sizeof (kvpairs), "chmod_locking=%d", val);

		notice("%s chmod_locking", val ? "Enable" : "Disable");

		ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_KVPAIRS,
				      strlen(kvpairs) + 1, kvpairs, NULL, NULL);
		if (ret)
			err("qap: QAP_SESSION_CMD_SET_KVPAIRS command failed");
		else
			qap_chmod_locking = val;
	}
}

static void *kbd_thread(void *userdata)
{
	struct termios stdin_termios;
	struct termios newt;
	char key[3];
	struct pollfd fds[2];
	int ret;

	if (tcgetattr(STDIN_FILENO, &stdin_termios) < 0)
		return NULL;

	newt = stdin_termios;
	newt.c_lflag &= ~ICANON;
	newt.c_lflag &= ~ECHO;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0)
		return NULL;

	kbd_ev = eventfd(0, EFD_CLOEXEC);

	fds[0].events = POLLIN;
	fds[0].fd = STDIN_FILENO;

	fds[1].events = POLLIN;
	fds[1].fd = kbd_ev;

	while (!quit) {
		ret = poll(fds, 2, -1);
		if (ret <= 0)
			break;

		if (fds[0].revents) {
			ret = read(STDIN_FILENO, key, 3);
			if (ret <= 0)
				break;
			kbd_handle_key(key);
		}
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &stdin_termios);

	return NULL;
}

static void handle_quit(int sig)
{
	quit = true;

	for (int i = 0; i < MAX_MODULES; i++) {
		struct ffmpeg_src *src = qap_inputs[i];
		if (src != NULL)
			ffmpeg_src_thread_stop(src);
	}

	if (kbd_ev != -1) {
		uint64_t v = 1;
		write(kbd_ev, &v, sizeof v);
	}
}

static void usage(void)
{
	fprintf(stderr, "usage: qapdec [OPTS] <input>\n"
		"Where OPTS is a combination of:\n"
		"  -v, --verbose                increase debug verbosity\n"
		"  -i, --interactive            enable keyboard control on the tty\n"
		"  -f, --format                 force ffmpeg input format\n"
		"  -p, --primary-stream=<n>     audio primary stream number to decode\n"
		"  -s, --secondary-stream=<n>   audio secondary stream number to decode\n"
		"  -t, --session-type=<type>    session type (broadcast, decode, encode, ott)\n"
		"  -o, --output-dir=<path>      output data to files in the specified dir path\n"
		"  -c, --channels=<channels>    maximum number of channels to output\n"
		"  -k, --kvpairs=<kvpairs>      pass kvpairs string to the decoder backend\n"
		"  -l, --loops=<count>          number of times the stream will be decoded\n"
		"      --realtime               sync input feeding and output render to pts\n"
		"      --sec-source=<url>       source for assoc/main2 module\n"
		"      --sys-source=<url>       source for system sound module\n"
		"      --app-source=<url>       source for app sound module\n"
		"      --ott-source=<url>       source for ott sound module\n"
		"      --sys-format=<fmt>       format for system sound module\n"
		"      --app-format=<fmt>       format for app sound module\n"
		"      --ott-format=<fmt>       format for ott sound module\n"
		"\n"
		"Example usage to feed generate sine wave audio and decode an AC3 file:\n"
		"  qapdec -c 2 --sys-format lavfi --sys-source sine /data/test.ac3\n"
		"\n");
}

static const struct option long_options[] = {
	{ "help",              no_argument,       0, 'h' },
	{ "verbose",           no_argument,       0, 'v' },
	{ "channels",          required_argument, 0, 'c' },
	{ "interactive",       no_argument,       0, 'i' },
	{ "kvpairs",           required_argument, 0, 'k' },
	{ "loops",             required_argument, 0, 'l' },
	{ "output-dir",        required_argument, 0, 'o' },
	{ "session-type",      required_argument, 0, 't' },
	{ "format",            required_argument, 0, 'f' },
	{ "primary-stream",    required_argument, 0, 'p' },
	{ "secondary-stream",  required_argument, 0, 's' },
	{ "realtime",          no_argument,       0, '0' },
	{ "sec-source",        required_argument, 0, '1' },
	{ "sys-source",        required_argument, 0, '2' },
	{ "app-source",        required_argument, 0, '3' },
	{ "sys-format",        required_argument, 0, '4' },
	{ "app-format",        required_argument, 0, '5' },
	{ "ott-source",        required_argument, 0, '6' },
	{ "ott-format",        required_argument, 0, '7' },
	{ 0,                   0,                 0,  0  }
};

int main(int argc, char **argv)
{
	int opt;
	int ret;
	int decode_err = 0;
	int loops = 1;
	int primary_stream_index = -1;
	int secondary_stream_index = -1;
	enum output_type outputs[2];
	unsigned int num_outputs = 0;
	char *kvpairs = NULL;
	struct ffmpeg_src **src = qap_inputs;
	const char *src_url[MAX_MODULES] = { };
	const char *src_format[MAX_MODULES] = { };
	uint64_t src_duration;
	uint64_t start_time;
	uint64_t end_time;
	bool kbd_enable = false;
	pthread_t kbd_tid;
	qap_session_t qap_session_type;
	AVStream *avstream;

	clock_base_time = get_time();
	qap_session_type = QAP_SESSION_BROADCAST;

	while ((opt = getopt_long(argc, argv, "c:f:hik:l:o:p:s:t:v",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (num_outputs >= ARRAY_SIZE(outputs)) {
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
		case 'i':
			kbd_enable = isatty(STDIN_FILENO);
			break;
		case 'k':
			kvpairs = optarg;
			break;
		case 'l':
			loops = atoi(optarg);
			break;
		case 'f':
			src_format[MODULE_PRIMARY] = optarg;
			break;
		case 'p':
			primary_stream_index = atoi(optarg);
			break;
		case 's':
			secondary_stream_index = atoi(optarg);
			break;
		case 'o':
			opt_output_dir = optarg;
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
		case '0':
			opt_render_realtime = 1;
			break;
		case '1':
			src_url[MODULE_SECONDARY] = optarg;
			break;
		case '2':
			src_url[MODULE_SYSTEM_SOUND] = optarg;
			break;
		case '3':
			src_url[MODULE_APP_SOUND] = optarg;
			break;
		case '4':
			src_format[MODULE_SYSTEM_SOUND] = optarg;
			break;
		case '5':
			src_format[MODULE_APP_SOUND] = optarg;
			break;
		case '6':
			src_url[MODULE_OTT_SOUND] = optarg;
			break;
		case '7':
			src_format[MODULE_OTT_SOUND] = optarg;
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

	if (src_url[MODULE_SECONDARY] &&
	    secondary_stream_index != -1) {
		err("cannot set both secondary stream index and url");
		return 1;
	}

	if (opt_output_dir && mkdir_p(opt_output_dir, 0777)) {
		err("failed to create output directory %s: %m", opt_output_dir);
		return 1;
	}

	if (num_outputs == 0)
		outputs[num_outputs++] = OUTPUT_5DOT1;

	if (optind < argc)
		src_url[MODULE_PRIMARY] = argv[optind];

	av_log_set_level(get_av_log_level());
	avformat_network_init();
	avdevice_register_all();

	init_outputs();

	if (kbd_enable)
		kbd_enable = !pthread_create(&kbd_tid, NULL, kbd_thread, NULL);

again:
	info("QAP library version %u", qap_get_version());

	qap_eos_received = false;

	/* init ffmpeg source and demuxer */
	for (int i = 0; i < MAX_MODULES; i++)
		src[i] = NULL;

	for (int i = 0; i < MAX_MODULES; i++) {
		if (!src_url[i])
			continue;

		src[i] = ffmpeg_src_create(src_url[i], src_format[i]);
		if (!src[i])
			return 1;
	}

	if (src[MODULE_PRIMARY]) {
		src_duration = ffmpeg_src_get_duration(src[MODULE_PRIMARY]);
		avstream = ffmpeg_src_get_avstream(src[MODULE_PRIMARY],
						   primary_stream_index);
		if (!avstream) {
			err("primary stream not found");
			return 1;
		}
	} else {
		avstream = NULL;
		src_duration = 0;
	}

	/* load QAP library */
	if (!avstream) {
		qap_lib_name = QAP_LIB_DOLBY_MS12;
	} else {
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
	}

	if (!qap_lib) {
		qap_lib = qap_load_library(qap_lib_name);
		if (!qap_lib) {
			err("failed to load library %s", qap_lib_name);
			return 1;
		}

		qap_lib_set_log_callback(qap_lib, handle_log_msg);
		qap_lib_set_log_level(qap_lib, debug_level - 3);
	}

	/* init QAP session */
	qap_session = qap_session_open(qap_session_type, qap_lib);
	if (!qap_session) {
		err("failed to open session");
		return 1;
	}

	qap_session_set_callback(qap_session, handle_qap_session_event, NULL);

	/* configure outputs */
	configure_outputs(num_outputs, outputs);

	/* apply user kvpairs */
	if (kvpairs) {
		ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_KVPAIRS,
				      strlen(kvpairs) + 1, kvpairs, NULL, NULL);
		if (ret) {
			err("QAP_SESSION_CMD_SET_KVPAIRS command failed");
			return 1;
		}
	}

	start_time = get_time();

	if (src[MODULE_PRIMARY]) {
		/* create primary QAP module */
		ret = ffmpeg_src_map_stream(src[MODULE_PRIMARY],
					    avstream->index,
					    QAP_MODULE_FLAG_PRIMARY);
		if (ret)
			return 1;

		/* create secondary QAP module */
		if (secondary_stream_index != -1) {
			ret = ffmpeg_src_map_stream(src[MODULE_PRIMARY],
						    secondary_stream_index,
						    QAP_MODULE_FLAG_SECONDARY);
			if (ret)
				return 1;
		}
	}

	if (src[MODULE_SECONDARY]) {
		/* create assoc/main2 QAP module */
		ret = ffmpeg_src_map_stream(src[MODULE_SECONDARY], -1,
					    QAP_MODULE_FLAG_SECONDARY);
		if (ret)
			return 1;
	}

	/* create PCM QAP modules */
	if (src[MODULE_SYSTEM_SOUND]) {
		ret = ffmpeg_src_map_stream(src[MODULE_SYSTEM_SOUND], -1,
					    QAP_MODULE_FLAG_SYSTEM_SOUND);
		if (ret)
			return 1;
	}

	if (src[MODULE_APP_SOUND]) {
		ret = ffmpeg_src_map_stream(src[MODULE_APP_SOUND], -1,
					    QAP_MODULE_FLAG_APP_SOUND);
		if (ret)
			return 1;
	}

	if (src[MODULE_OTT_SOUND]) {
		ret = ffmpeg_src_map_stream(src[MODULE_OTT_SOUND], -1,
					    QAP_MODULE_FLAG_OTT_SOUND);
		if (ret)
			return 1;
	}

	signal(SIGINT, handle_quit);
	signal(SIGTERM, handle_quit);

	primary_done = 0;

	/* start input threads */
	for (int i = 0; i < MAX_MODULES; i++) {
		if (!src[i])
			continue;
		ffmpeg_src_thread_start(src[i]);
	}

	/* wait for input threads to finish */
	if (src[MODULE_PRIMARY]) {
		decode_err = ffmpeg_src_thread_join(src[MODULE_PRIMARY]);
		if (decode_err)
			quit = 1;
		primary_done = 1;
	}

	for (int i = MODULE_SYSTEM_SOUND; i < MAX_MODULES; i++) {
		if (!src[i])
			continue;
		ffmpeg_src_thread_join(src[i]);
	}

	if (src[MODULE_PRIMARY] && !decode_err) {
		struct ffmpeg_src *psrc = src[MODULE_PRIMARY];
		/* send EOS and stop all streams */
		for (int i = 0; i < MAX_STREAMS; i++) {
			if (!psrc->streams[i])
				continue;
			stream_send_eos(psrc->streams[i]);
			stream_stop(psrc->streams[i]);
		}

		info(" in: sent EOS");

		/* wait EOS */
		pthread_mutex_lock(&qap_lock);
		while (!qap_eos_received)
			pthread_cond_wait(&qap_cond, &qap_lock);
		pthread_mutex_unlock(&qap_lock);
	}

	/* cleanup */
	for (int i = 0; i < MAX_MODULES; i++) {
		ffmpeg_src_destroy(src[i]);
		src[i] = NULL;
	}

	if (qap_session_close(qap_session))
		err("failed to close session");

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

	if (src_duration > 0 && !quit) {
		notice("render speed: %.2fx realtime",
		       (float)src_duration / (float)(end_time - start_time));
	}

	if (!quit && --loops > 0)
		goto again;

	if (kbd_enable) {
		handle_quit(SIGTERM);
		pthread_join(kbd_tid, NULL);
	}

	if (qap_unload_library(qap_lib))
		err("failed to unload library");

	shutdown_outputs();

	return decode_err;
}
