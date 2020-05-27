#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#define MUNIT_ENABLE_ASSERT_ALIASES
#include "munit/munit.h"

#include "qd.h"

static const char *
resolve_test_file(const char *filename)
{
	static char buf[PATH_MAX];
	const char *tests_dir;

	tests_dir = getenv("TESTS_DIR");
	if (!tests_dir)
		tests_dir = ".";

	snprintf(buf, sizeof (buf), "%s/%s", tests_dir, filename);
	return buf;
}

struct file_alias {
	const char *alias;
	const char *filename;
};

static const char *
find_filename(const struct file_alias *files, const char *alias)
{
	for (const struct file_alias *f = files; f->alias; f++) {
		if (!strcmp(f->alias, alias))
			return resolve_test_file(f->filename);
	}
	return NULL;
}

static inline bool
int16_is_silence(int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (samples[i] < INT16_MIN * 0.0001 ||
		    samples[i] > INT16_MAX * 0.0001)
			return false;
	}
	return true;
}

/*
 * Helper to measure peak frequency and gain of a single channel of audio data
 */

enum window {
	WIN_RECT = 0,
	WIN_HANN,
	WIN_HAMMING,
};

static double hann(double v, unsigned int n)
{
	return 0.5 * (1.0 - cos(2.0 * M_PI * v / (double)(n - 1)));
}

static double hamming(double v, unsigned int n)
{
	return 0.54 - 0.46 * cos(2.0 * M_PI * v / (double)(n - 1));
}

struct peak_analyzer {
	int sample_rate;
	size_t n_samples;
	size_t max_samples;
	enum window window;;
	double *rdata;
	complex double *idata;
	fftw_plan plan;
};

static void
peak_analyzer_cleanup(struct peak_analyzer *pa)
{
	free(pa->plan);
	free(pa->rdata);
	free(pa->idata);
}

static int
peak_analyzer_init(struct peak_analyzer *pa, int sample_rate, size_t n_samples,
		   enum window window)
{
	memset(pa, 0, sizeof (*pa));

	pa->sample_rate = sample_rate;
	pa->max_samples = n_samples;
	pa->rdata = fftw_malloc(n_samples * sizeof (*pa->rdata));
	pa->idata = fftw_malloc(n_samples * sizeof (*pa->idata));
	pa->window = window;

	if (!pa->rdata || !pa->idata)
		goto fail;

	pa->plan = fftw_plan_dft_r2c_1d(n_samples, pa->rdata, pa->idata,
					FFTW_ESTIMATE);
	if (!pa->plan)
		goto fail;

	return 0;

fail:
	peak_analyzer_cleanup(pa);
	return -1;
}

static size_t
peak_analyzer_add_samples(struct peak_analyzer *pa,
			  const int16_t *samples, size_t n_samples)
{
	size_t avail;

	avail = pa->max_samples - pa->n_samples;
	n_samples = QD_MIN(n_samples, avail);

	for (size_t i = 0; i < n_samples; i++) {
		double d = samples[i] / 32768.0;
		switch (pa->window) {
		case WIN_RECT:
			break;
		case WIN_HANN:
			d *= hann(pa->n_samples, pa->max_samples);
			break;
		case WIN_HAMMING:
			d *= hamming(pa->n_samples, pa->max_samples);
			break;
		}
		pa->rdata[pa->n_samples++] = d;
	}

	return n_samples;
}

static bool
peak_analyzer_run(struct peak_analyzer *pa,
		  double *out_peak_freq,
		  double *out_peak_gain)
{
	double peak_freq = 0, peak_gain = -INFINITY;
	int n_samples;

	assert(pa->n_samples == pa->max_samples);
	fftw_execute(pa->plan);

	n_samples = pa->n_samples / 2;
	for (size_t i = 1; i < n_samples; i++) {
		double sq, gain;
		complex double di = pa->idata[i];

		/* normalize fft data */
		di /= n_samples;
		sq = creal(di) * creal(di) + cimag(di) * cimag(di);
		gain = 10 * log10(sq);

		if (gain > peak_gain) {
			peak_gain = gain;
			peak_freq = i * pa->sample_rate / (double)pa->n_samples;
		}
	}

	if (peak_gain < -60.0)
		return false;

	if (out_peak_freq)
		*out_peak_freq = peak_freq;
	if (out_peak_gain)
		*out_peak_gain = peak_gain;

	return true;
}

//*****************************************************************************
// MS12 Tests
//*****************************************************************************

/* common parameters */
static char *parm_ms12_sessions_all[] = {
	"ott", "broadcast", NULL
};

static char *parm_ms12_sessions_ott_only[] = {
	"ott", NULL
};

static char *parm_ms12_outputs_pcm_single[] = {
	"2.0", "5.1", "7.1", NULL
};

static char *parm_ms12_outputs_pcm_all[] = {
	"2.0", "5.1", "7.1", "2.0+5.1", "2.0+7.1", NULL
};

static char *parm_ms12_outputs_pcm_stereo[] = {
	"2.0", NULL
};

/*
 * MS12 testing helpers
 */

/* common test prologue, initializing qd at INFO debug level */
static void *
pretest_ms12(const MunitParameter params[], void* user_data)
{
	qd_debug_level = 2;
	qd_init();

	return NULL;
}

/* setup an MS12 session with common input parameters:
 *   - session type
 *   - outputs configuration
 */
static struct qd_session *
setup_ms12_session(const MunitParameter params[])
{
	struct qd_session *session;
	qap_session_t session_type = QAP_SESSION_BROADCAST;
	enum qd_output_id outputs[4] = {};
	int n_outputs = 0;
	const char *v;
	char *dump_path;
	size_t n;

	v = munit_parameters_get(params, "t");
	if (!strcmp(v, "broadcast"))
		session_type = QAP_SESSION_BROADCAST;
	else if (!strcmp(v, "ott"))
		session_type = QAP_SESSION_MS12_OTT;
	else if (!strcmp(v, "decode"))
		session_type = QAP_SESSION_DECODE_ONLY;

	v = munit_parameters_get(params, "o");
	while (*v) {
		enum qd_output_id id = QD_OUTPUT_NONE;

		n = strcspn(v, "+");
		if (n == 0) {
			v++;
			continue;
		}

		if (!strncmp(v, "2.0", n))
			id = QD_OUTPUT_STEREO;
		else if (!strncmp(v, "5.1", n))
			id = QD_OUTPUT_5DOT1;
		else if (!strncmp(v, "7.1", n))
			id = QD_OUTPUT_7DOT1;
		else if (!strncmp(v, "ac3", n))
			id = QD_OUTPUT_AC3;
		else if (!strncmp(v, "eac3", n))
			id = QD_OUTPUT_EAC3;

		v += n;

		/* ignore 7.1 output in OTT mode */
		if (session_type == QAP_SESSION_MS12_OTT &&
		    id == QD_OUTPUT_7DOT1)
			return NULL;

		outputs[n_outputs++] = id;
	}

	assert_not_null((session = qd_session_create(QD_MODULE_DOLBY_MS12,
						     session_type)));

	assert_int(qd_session_configure_outputs(session, n_outputs,
						outputs), ==, 0);

	if ((dump_path = getenv("DUMP_DIR")) != NULL) {
		char buf[PATH_MAX];
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);

		snprintf(buf, sizeof (buf), "%s/qaptest-%lu%03lu",
			 dump_path, ts.tv_sec, ts.tv_nsec / 1000000);

		qd_session_set_dump_path(session, buf);
	}

	return session;
}

/*
 * MS12: test runtime input channel config changes
 *
 * Input files include multiple channel configuration, changing every few
 * seconds. Verify the sequence of reported input configurations matches the
 * actual data in the files. Each reported configuration is recorded and the
 * result is checked after the full file has been played out.
 */

/* FIXME: the 3/4/5 channels mappings are incorrect */
static const qap_input_config_t chid_swp_configs[] = {
	{ .channels = 1, .ch_map = { QAP_AUDIO_PCM_CHANNEL_C } },
	{ .channels = 2, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R } },
	{ .channels = 3, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R,
				     QAP_AUDIO_PCM_CHANNEL_LFE } },
	{ .channels = 4, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R,
				     QAP_AUDIO_PCM_CHANNEL_LS,
				     QAP_AUDIO_PCM_CHANNEL_RS } },
	{ .channels = 3, .ch_map = { QAP_AUDIO_PCM_CHANNEL_C,
				     QAP_AUDIO_PCM_CHANNEL_LS,
				     QAP_AUDIO_PCM_CHANNEL_RS } },
	{ .channels = 4, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R,
				     QAP_AUDIO_PCM_CHANNEL_C,
				     QAP_AUDIO_PCM_CHANNEL_LFE } },
	{ .channels = 5, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R,
				     QAP_AUDIO_PCM_CHANNEL_LS,
				     QAP_AUDIO_PCM_CHANNEL_RS,
				     QAP_AUDIO_PCM_CHANNEL_LFE } },
	{ .channels = 6, .ch_map = { QAP_AUDIO_PCM_CHANNEL_L,
				     QAP_AUDIO_PCM_CHANNEL_R,
				     QAP_AUDIO_PCM_CHANNEL_C,
				     QAP_AUDIO_PCM_CHANNEL_LS,
				     QAP_AUDIO_PCM_CHANNEL_RS,
				     QAP_AUDIO_PCM_CHANNEL_LFE } },
};

struct chid_swp_file {
	const char *alias;
	const char *filename;
	int format;
	int profile;
	int n_configs;
	const qap_input_config_t *configs;
};

static const struct chid_swp_file chid_swp_files[] = {
	{ "dd",
	  "Elementary_Streams/ChID/ChID_voices/ChID_voices_swp_dd.ac3",
	  QAP_AUDIO_FORMAT_AC3, 0,
	  QD_N_ELEMENTS(chid_swp_configs), chid_swp_configs },
	{ "ddp",
	  "Elementary_Streams/ChID/ChID_voices/ChID_voices_swp_ddp.ec3",
	  QAP_AUDIO_FORMAT_EAC3, 0,
	  QD_N_ELEMENTS(chid_swp_configs), chid_swp_configs },
	{ "aac_adts",
	  "Elementary_Streams/ChID/ChID_voices/ChID_voices_swp_heaac.adts",
	  QAP_AUDIO_FORMAT_AAC_ADTS, QAP_PROFILE_AAC_LOW_COMPLEXITY,
	  QD_N_ELEMENTS(chid_swp_configs), chid_swp_configs },
	{ "aac_loas",
	  "Elementary_Streams/ChID/ChID_voices/ChID_voices_swp_heaac.loas",
	  QAP_AUDIO_FORMAT_AAC_ADTS, QAP_PROFILE_AAC_MAIN,
	  QD_N_ELEMENTS(chid_swp_configs), chid_swp_configs
	},
};

struct channel_sweep_ctx {
	int n_configs;
	const struct chid_swp_file *f;
};

static void
input_event_cb_record(struct qd_input *input, enum qd_input_event ev,
		      void *userdata)
{
	struct channel_sweep_ctx *ctx = userdata;
	int i;

	if (ev != QD_INPUT_CONFIG_CHANGED)
		return;

	/* check we are not receiving more input reconfigs than expected */
	assert_int(ctx->n_configs, <, ctx->f->n_configs);

	/* check the input config matches the file description */
	i = ctx->n_configs++;
	assert_int(input->config.format, ==, ctx->f->format);
	assert_int(input->config.profile, ==, ctx->f->profile);
	assert_int(input->config.channels, ==, ctx->f->configs[i].channels);
	assert_memory_equal(sizeof (input->config.ch_map),
			    input->config.ch_map, ctx->f->configs[i].ch_map);
}

static MunitResult
test_ms12_channel_sweep(const MunitParameter params[],
			void *user_data_or_fixture)
{
	struct qd_session *session;
	struct qd_input *input;
	struct ffmpeg_src *src;
	struct channel_sweep_ctx ctx = {};
	const char *v;

	if (!(session = setup_ms12_session(params)))
		return MUNIT_SKIP;

	v = munit_parameters_get(params, "f");
	for (size_t i = 0; i < QD_N_ELEMENTS(chid_swp_files); i++) {
		if (!strcmp(v, chid_swp_files[i].alias))
			ctx.f = &chid_swp_files[i];
	}

	if (!ctx.f)
		return MUNIT_ERROR;

	assert_not_null((src = ffmpeg_src_create(resolve_test_file(ctx.f->filename), NULL)));
	assert_not_null((input = ffmpeg_src_add_input(src, 0, session, QD_INPUT_MAIN)));
	qd_input_set_event_cb(input, input_event_cb_record, &ctx);
	assert_int(0, ==, ffmpeg_src_thread_start(src));
	assert_int(0, ==, ffmpeg_src_thread_join(src));

	/* check we received the expected number of input configuration events */
	assert_int(ctx.n_configs, ==, ctx.f->n_configs);

	ffmpeg_src_destroy(src);
	qd_session_destroy(session);

	return MUNIT_OK;
}

static char *parm_ms12_files_channel_sweep[] = {
	"dd", "ddp", "aac_adts", "aac_loas", NULL
};

static MunitParameterEnum parms_ms12_channel_sweep[] = {
	{ "t", parm_ms12_sessions_all },
	{ "o", parm_ms12_outputs_pcm_all },
	{ "f", parm_ms12_files_channel_sweep },
	{ NULL, NULL },
};

/*
 * MS12: test Main+Assoc mixing
 *
 * Input files feature a single channel 997Hz tone at -20dBFS. Main is in left
 * channel and Assoc is in right channel.
 *
 * Compute an FFT on 1s chunks of audio output data and verify we get the
 * correct peak frequency and gain, testing at multiple "xu" kvpairs values.
 */

struct assoc_mix_ctx {
	struct {
		struct peak_analyzer pa[2];
	} outputs[QD_MAX_OUTPUTS];
	int gain[2];
};

static void
assoc_mix_output_cb(struct qd_output *output, qap_audio_buffer_t *buffer,
		    void *userdata)
{
	struct assoc_mix_ctx *ctx = userdata;
	struct peak_analyzer *pa;
	size_t frame_size;

	/* check output config */
	assert_int(output->config.sample_rate, ==, 48000);
	assert_int(output->config.bit_width, ==, 16);

	frame_size = output->config.channels * output->config.bit_width / 8;
	assert_int(buffer->common_params.size % frame_size, ==, 0);

	/* feed left and right channel data */
	pa = ctx->outputs[output->id].pa;

	for (size_t i = 0; i < buffer->common_params.size; i += frame_size) {
		int16_t *frame = buffer->common_params.data + i;
		peak_analyzer_add_samples(&pa[0], frame + 0, 1);
		peak_analyzer_add_samples(&pa[1], frame + 1, 1);
	}

	for (int i = 0; i < 2; i++) {
		double freq, gain;
		bool mute;

		/* fill window before running fft and analyzing data */
		if (pa[i].n_samples != pa[i].max_samples)
			continue;

		/* verify peak frequency and gain are correct */
		mute = ctx->gain[i] <= -32;
		assert_true(mute == !peak_analyzer_run(&pa[i], &freq, &gain));
		if (!mute) {
			assert_double(freq, ==, 997);
			assert_double(gain, >, -24 + ctx->gain[i] - 1.0);
			assert_double(gain, <, -24 + ctx->gain[i] + 1.0);
		}

		/* reset window */
		pa[i].n_samples = 0;
	}
}

static const struct file_alias assoc_mix_main_files[] = {
	{ "ddp", "Elementary_Streams/Mix_Fader/Mix_fader_neutral_2PID_ddp_main.ec3" },
	{ "aac", "Elementary_Streams/Mix_Fader/Mix_fader_neutral_2PID_heaac_main.loas" },
	{ NULL, NULL }
};

static const struct file_alias assoc_mix_assoc_files[] = {
	{ "ddp", "Elementary_Streams/Mix_Fader/Mix_fader_neutral_2PID_ddp_assoc.ec3" },
	{ "aac", "Elementary_Streams/Mix_Fader/Mix_fader_neutral_2PID_heaac_assoc.loas" },
	{ NULL, NULL }
};

static MunitResult
test_ms12_assoc_mix(const MunitParameter params[],
		    void *user_data_or_fixture)
{
	struct qd_session *session;
	struct qd_input *input_main, *input_assoc;
	struct ffmpeg_src *src_main, *src_assoc;
	const char *v, *f_main, *f_assoc;
	struct assoc_mix_ctx ctx;
	int xu;

	if (!(session = setup_ms12_session(params)))
		return MUNIT_SKIP;

	qd_session_set_output_cb(session, assoc_mix_output_cb, &ctx);

	/* setup fft to determine peak freq/gain, with a 1s window */
	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.outputs); i++) {
		for (size_t j = 0; j < QD_N_ELEMENTS(ctx.outputs[i].pa); j++)
			peak_analyzer_init(&ctx.outputs[i].pa[j],
					48000, 48000, WIN_RECT);
	}

	/* set main/assoc mixing gain */
	v = munit_parameters_get(params, "xu");
	xu = v ? atoi(v) : 0;
	ctx.gain[0] = xu < 0 ? xu : 0;
	ctx.gain[1] = xu > 0 ? -xu : 0;
	qd_session_set_kvpairs(session, "xu=%d", xu);

	/* create main input */
	v = munit_parameters_get(params, "f");
	if (!(f_main = find_filename(assoc_mix_main_files, v)))
		return MUNIT_ERROR;

	assert_not_null((src_main = ffmpeg_src_create(f_main, NULL)));
	assert_not_null((input_main = ffmpeg_src_add_input(src_main,
							   0, session,
							   QD_INPUT_MAIN)));

	/* create assoc input */
	v = munit_parameters_get(params, "f");
	if (!(f_assoc = find_filename(assoc_mix_assoc_files, v)))
		return MUNIT_ERROR;

	assert_not_null((src_assoc = ffmpeg_src_create(f_assoc, NULL)));
	assert_not_null((input_assoc = ffmpeg_src_add_input(src_assoc,
							    0, session,
							    QD_INPUT_ASSOC)));

	/* skip silence and ref tone at beginning of input files */
	assert_int(0, ==, ffmpeg_src_seek(src_main, 35000));
	assert_int(0, ==, ffmpeg_src_seek(src_assoc, 35000));

	/* skip 1s of output data, to skip audio ramping up */
	qd_session_set_output_discard_ms(session, 1000);

	/* play */
	assert_int(0, ==, ffmpeg_src_thread_start(src_main));
	assert_int(0, ==, ffmpeg_src_thread_start(src_assoc));
	assert_int(0, ==, ffmpeg_src_thread_join(src_main));
	assert_int(0, ==, ffmpeg_src_thread_join(src_assoc));

	ffmpeg_src_destroy(src_main);
	ffmpeg_src_destroy(src_assoc);
	qd_session_destroy(session);

	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.outputs); i++) {
		for (size_t j = 0; j < QD_N_ELEMENTS(ctx.outputs[i].pa); j++)
			peak_analyzer_cleanup(&ctx.outputs[i].pa[j]);
	}

	return MUNIT_OK;
}

static char *parm_ms12_files_assoc_mix[] = {
	"ddp", "aac", NULL
};

static char *parm_ms12_xu_assoc_mix[] = {
	"0", "-16", "16", "-32", "32", NULL
};

static MunitParameterEnum parms_ms12_assoc_mix[] = {
	{ "t", parm_ms12_sessions_all },
	{ "o", parm_ms12_outputs_pcm_all },
	{ "f", parm_ms12_files_assoc_mix },
	{ "xu", parm_ms12_xu_assoc_mix },
	{ NULL, NULL },
};

/*
 * MS12: test stereo downmix modes
 *
 * Input files will render a different, known fixed frequency based on the
 * stereo downmix mode.
 *
 * Compute an FFT on 1s chunks of audio output data and verify we get the
 * correct peak frequency and gain, testing with Lo/Ro and Lt/Rt downmix modes.
 */

struct stereo_downmix_ctx {
	struct peak_analyzer pa[2];
	int dmx;
};

static void
stereo_downmix_output_cb(struct qd_output *output, qap_audio_buffer_t *buffer,
			 void *userdata)
{
	struct stereo_downmix_ctx *ctx = userdata;
	size_t frame_size;

	/* check output config */
	assert_int(output->config.sample_rate, ==, 48000);
	assert_int(output->config.bit_width, ==, 16);

	frame_size = output->config.channels * output->config.bit_width / 8;
	assert_int(buffer->common_params.size % frame_size, ==, 0);

	/* feed left and right channel data */
	for (size_t i = 0; i < buffer->common_params.size; i += frame_size) {
		int16_t *frame = buffer->common_params.data + i;
		peak_analyzer_add_samples(&ctx->pa[0], frame + 0, 1);
		peak_analyzer_add_samples(&ctx->pa[1], frame + 1, 1);
	}

	for (int i = 0; i < 2; i++) {
		double freq, gain;

		/* fill window before running fft and analyzing data */
		if (ctx->pa[i].n_samples != ctx->pa[i].max_samples)
			continue;

		/* verify peak frequency is correct */
		assert_true(peak_analyzer_run(&ctx->pa[i], &freq, &gain));

		info("test/output: ts=%" PRIi64 " ch=%c %lgHz %lgdB",
		     output->pts, i == 0 ? 'l' : 'r', freq, gain);

		if (ctx->dmx)
			assert_double(freq, ==, 997);
		else {
			/* should be 403Hz, AAC is off by 2Hz so accept 405Hz */
			assert_double(freq, >=, 403);
			assert_double(freq, <=, 405);
		}

		/* reset window */
		ctx->pa[i].n_samples = 0;
	}
}

static const struct file_alias stereo_downmix_files[] = {
	{ "ddp", "Elementary_Streams/Downmix/Downmix_ddp.ec3" },
	{ "aac_adts", "Elementary_Streams/Downmix/Downmix_heaac.adts" },
	{ "aac_loas", "Elementary_Streams/Downmix/Downmix_heaac.loas" },
	{ NULL, NULL }
};

static MunitResult
test_ms12_stereo_downmix(const MunitParameter params[],
			 void *user_data_or_fixture)
{
	struct qd_session *session;
	struct qd_input *input;
	struct ffmpeg_src *src;
	const char *v, *f;
	struct stereo_downmix_ctx ctx;

	if (!(session = setup_ms12_session(params)))
		return MUNIT_SKIP;

	qd_session_set_output_cb(session, stereo_downmix_output_cb, &ctx);

	/* setup fft to determine peak freq, with a 1s window */
	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_init(&ctx.pa[i], 48000, 48000, WIN_RECT);

	/* set stereo downmix mode */
	v = munit_parameters_get(params, "dmx");
	ctx.dmx = v ? atoi(v) : 0;
	qd_session_set_kvpairs(session, "dmx=%d", ctx.dmx);

	/* create main input */
	v = munit_parameters_get(params, "f");
	if (!(f = find_filename(stereo_downmix_files, v)))
		return MUNIT_ERROR;

	assert_not_null((src = ffmpeg_src_create(f, NULL)));
	assert_not_null((input = ffmpeg_src_add_input(src, 0, session,
						      QD_INPUT_MAIN)));

	/* skip silence and ref tone at beginning of input files */
	assert_int(0, ==, ffmpeg_src_seek(src, 25000));

	/* skip 1s of output data, to skip audio ramping up */
	qd_session_set_output_discard_ms(session, 1000);

	/* play */
	assert_int(0, ==, ffmpeg_src_thread_start(src));
	assert_int(0, ==, ffmpeg_src_thread_join(src));

	ffmpeg_src_destroy(src);
	qd_session_destroy(session);

	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_cleanup(&ctx.pa[i]);

	return MUNIT_OK;
}

static char *parm_ms12_files_stereo_downmix[] = {
	"ddp", "aac_adts", "aac_loas", NULL
};

static char *parm_ms12_dmx[] = {
	"0", "1", NULL
};

static MunitParameterEnum parms_ms12_stereo_downmix[] = {
	{ "t", parm_ms12_sessions_all },
	{ "o", parm_ms12_outputs_pcm_stereo },
	{ "f", parm_ms12_files_stereo_downmix },
	{ "dmx", parm_ms12_dmx },
	{ NULL, NULL },
};

/*
 * MS12: test DRC modes
 *
 * Input files will render a 440Hz tone with volume incrementing monotonocally.
 *
 * Compute an FFT on 1s chunks of audio output data and verify we get the
 * correct frequency and increasing gain, testing with Line and RF DRC modes.
 */

struct drc_ctx {
	struct peak_analyzer pa[2];
	int drc;
};

static void
drc_output_cb(struct qd_output *output, qap_audio_buffer_t *buffer,
	      void *userdata)
{
	struct drc_ctx *ctx = userdata;
	size_t frame_size;

	/* check output config */
	assert_int(output->config.sample_rate, ==, 48000);
	assert_int(output->config.bit_width, ==, 16);

	frame_size = output->config.channels * output->config.bit_width / 8;
	assert_int(buffer->common_params.size % frame_size, ==, 0);

	/* skip data other than the 440Hz tone */
	if (output->pts <= 14 * QD_SECOND ||
	    output->pts >= 70 * QD_SECOND)
		return;

	/* feed left and right channel data */
	for (size_t i = 0; i < buffer->common_params.size; i += frame_size) {
		int16_t *frame = buffer->common_params.data + i;
		peak_analyzer_add_samples(&ctx->pa[0], frame + 0, 1);
		peak_analyzer_add_samples(&ctx->pa[1], frame + 1, 1);
	}

	for (int i = 0; i < 2; i++) {
		double freq, gain;
		double t;

		/* fill window before running fft and analyzing data */
		if (ctx->pa[i].n_samples != ctx->pa[i].max_samples)
			continue;

		assert_true(peak_analyzer_run(&ctx->pa[i], &freq, &gain));

		/* adjust gain in RF mode to match Line mode */
		if (ctx->drc)
			gain -= 11;

		info("test/output: ts=%" PRIi64 " ch=%c %lgHz %lgdB",
		     output->pts, i == 0 ? 'l' : 'r', freq, gain);

		/* check peak frequency */
		assert_double(freq, ==, 440);

		/* check gain matches reference curve */
		t = output->pts / (double)QD_SECOND;
		if (t <= 26) {
			assert_double(gain, >=, -52.5 + (t - 10) / 2.0);
			assert_double(gain, <=, -50.5 + (t - 10) / 2.0);
		} else if (t <= 46) {
			assert_double(gain, >=, -44.5 + (t - 26));
			assert_double(gain, <=, -42.5 + (t - 26));
		} else {
			assert_double(gain, >=, -24.5 + (t - 46) / 2.0);
			assert_double(gain, <=, -22.5 + (t - 46) / 2.0);
		}

		/* reset window */
		ctx->pa[i].n_samples = 0;
	}
}

static const struct file_alias drc_files[] = {
	{ "dd", "Elementary_Streams/DRC/DRC_ML_200_dd.ac3" },
	{ "ddp", "Elementary_Streams/DRC/DRC_ML_200_ddp.ec3" },
	{ "aac_adts", "Elementary_Streams/DRC/DRC_ML_200_aac.adts" },
	{ "aac_loas", "Elementary_Streams/DRC/DRC_ML_200_heaac.loas" },
	{ NULL, NULL }
};

static MunitResult
test_ms12_drc(const MunitParameter params[], void *user_data_or_fixture)
{
	struct qd_session *session;
	struct qd_input *input;
	struct ffmpeg_src *src;
	const char *v, *f;
	struct drc_ctx ctx;

	if (!(session = setup_ms12_session(params)))
		return MUNIT_SKIP;

	qd_session_set_output_cb(session, drc_output_cb, &ctx);

	/* setup fft to determine peak freq and gain, with a 1s window */
	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_init(&ctx.pa[i], 48000, 48000, WIN_RECT);

	/* set drc mode */
	v = munit_parameters_get(params, "drc");
	ctx.drc = v ? atoi(v) : 0;
	qd_session_set_kvpairs(session, "drc=%d;dap_drc=%d", ctx.drc, ctx.drc);

	/* create main input */
	v = munit_parameters_get(params, "f");
	if (!(f = find_filename(drc_files, v)))
		return MUNIT_ERROR;

	assert_not_null((src = ffmpeg_src_create(f, NULL)));
	assert_not_null((input = ffmpeg_src_add_input(src, 0, session,
						      QD_INPUT_MAIN)));

	/* play */
	assert_int(0, ==, ffmpeg_src_thread_start(src));
	assert_int(0, ==, ffmpeg_src_thread_join(src));

	ffmpeg_src_destroy(src);
	qd_session_destroy(session);

	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_cleanup(&ctx.pa[i]);

	return MUNIT_OK;
}

static char *parm_ms12_files_drc[] = {
	"dd", "ddp", "aac_adts", "aac_loas", NULL
};

static char *parm_ms12_drc[] = {
	"0", "1", NULL
};

static MunitParameterEnum parms_ms12_drc[] = {
	{ "t", parm_ms12_sessions_all },
	{ "o", parm_ms12_outputs_pcm_stereo },
	{ "f", parm_ms12_files_drc },
	{ "drc", parm_ms12_drc },
	{ NULL, NULL },
};

/*
 * MS12: test PAUSE/START module commands
 *
 * Render a fixed frequency tone by feeding data in realtime, and verify
 * sending the PAUSE/START commands works. We test in OTT mode so that
 * continuous audio is enabled, verifying silence is sent during pause and
 * checking the peak frequency and gain during playback.
 */

struct pause_ctx {
	struct peak_analyzer pa[2];
	int lr_silent_samples[2];
	bool lr_silent[2];
	struct qd_input *input;
	enum qd_input_state state;
	uint64_t state_change_time;
	int max_delay_ms;
	pthread_mutex_t lock;
};

static void
pause_output_cb(struct qd_output *output, qap_audio_buffer_t *buffer,
		     void *userdata)
{
	struct pause_ctx *ctx = userdata;
	size_t frame_size;
	uint64_t now, change_time;
	bool paused;

	/* check output config */
	assert_int(output->config.sample_rate, ==, 48000);
	assert_int(output->config.bit_width, ==, 16);
	assert_int(output->config.channels, >=, 2);

	frame_size = output->config.channels * output->config.bit_width / 8;
	assert_int(buffer->common_params.size % frame_size, ==, 0);

	/* record input state */
	pthread_mutex_lock(&ctx->lock);
	paused = ctx->state == QD_INPUT_STATE_PAUSED;
	change_time = ctx->state_change_time;
	pthread_mutex_unlock(&ctx->lock);

	now = qd_get_time();

	/* process data */
	for (size_t i = 0; i < buffer->common_params.size; i += frame_size) {
		int16_t *frame = buffer->common_params.data + i;

		/* record number of silent consecutive samples in L/R */
		for (int i = 0; i < 2; i++) {
			if (int16_is_silence(frame + i, 1))
				ctx->lr_silent_samples[i]++;
			else
				ctx->lr_silent_samples[i] = 0;
		}

		/* check if silence was detected */
		for (int i = 0; i < 2; i++) {
			bool silent = ctx->lr_silent_samples[i] > 48;
			if (silent == ctx->lr_silent[i])
				continue;

			info("test/output: ts=%" PRIu64 " %s channel is now %s, "
			     "%" PRIi64 "ms since last state change",
			     output->pts, i == 0 ? "left" : "right",
			     silent ? "silent" : "noisy",
			     (now - change_time) / 1000);

			ctx->lr_silent[i] = silent;
		}

		/* verify module state has taken effect: we expect silence when
		 * paused, sound when started, with delay threshold for the
		 * command to take effect */
		if (now > change_time + ctx->max_delay_ms * 1000) {
			if (paused != ctx->lr_silent[0] ||
			    paused != ctx->lr_silent[1]) {
				munit_logf(MUNIT_LOG_ERROR,
					   "state changed to %s %dms ago, expecting %s audio",
					   paused ? "paused" : "playing",
					   (int)((now - change_time) / 1000),
					   paused ? "silent" : "non-silent");
			}

			assert_int(paused, ==, ctx->lr_silent[0]);
			assert_int(paused, ==, ctx->lr_silent[1]);
		}

		/* feed left and right channels to fft */
		if (!ctx->lr_silent[0])
			peak_analyzer_add_samples(&ctx->pa[0], frame + 0, 1);
		if (!ctx->lr_silent[1])
			peak_analyzer_add_samples(&ctx->pa[1], frame + 1, 1);

		/* verify the rest is silence */
		assert_true(int16_is_silence(frame + 2,
					     output->config.channels - 2));
	}

	/* analyze fft */
	for (int i = 0; i < 2; i++) {
		double freq, gain;

		/* fill window before running fft and analyzing data */
		if (ctx->pa[i].n_samples != ctx->pa[i].max_samples)
			continue;

		assert_true(peak_analyzer_run(&ctx->pa[i], &freq, &gain));

		info("test/output: ts=%" PRIi64 " ch=%c %lgHz %lgdB",
		     output->pts, i == 0 ? 'l' : 'r', freq, gain);

		/* check peak frequency */
		assert_double(freq, >=, 992);
		assert_double(freq, <=, 1002);

		/* check gain is -20dB */
		assert_double(gain, >=, -22.0);
		assert_double(gain, <=, -18.0);

		/* reset window */
		ctx->pa[i].n_samples = 0;
	}
}

static const struct file_alias pause_files[] = {
	{ "ddp", "Elementary_Streams/Reference_Level/Ref_997_200_48k_20dB_ddp.ec3" },
	{ "aac_adts", "Elementary_Streams/Reference_Level/Ref_997_200_48k_20dB_heaac.adts" },
	{ "aac_loas", "Elementary_Streams/Reference_Level/Ref_997_200_48k_20dB_heaac.loas" },
	{ NULL, NULL }
};

static MunitResult
test_ms12_pause(const MunitParameter params[], void *user_data_or_fixture)
{
	struct qd_session *session;
	struct qd_input *input;
	struct ffmpeg_src *src;
	const char *v, *f;
	struct pause_ctx ctx = {};

	if (!(session = setup_ms12_session(params)))
		return MUNIT_SKIP;

	qd_session_set_realtime(session, true);
	qd_session_set_output_cb(session, pause_output_cb, &ctx);

	/* setup fft to determine peak freq and gain, with a 1s window */
	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_init(&ctx.pa[i], 48000, 48000, WIN_RECT);

	/* get max allowed command delay from params */
	v = munit_parameters_get(params, "max_delay_ms");
	ctx.max_delay_ms = atoi(v);

	/* create main input */
	v = munit_parameters_get(params, "f");
	if (!(f = find_filename(pause_files, v)))
		return MUNIT_ERROR;

	assert_not_null((src = ffmpeg_src_create(f, NULL)));
	assert_not_null((input = ffmpeg_src_add_input(src, 0, session,
						      QD_INPUT_MAIN)));

	/* skip silence and ref tone at beginning of input files */
	assert_int(0, ==, ffmpeg_src_seek(src, 13000));

	/* discard 500ms of audio ramping up */
	qd_session_set_output_discard_ms(session, 500);

	pthread_mutex_init(&ctx.lock, NULL);
	ctx.input = input;
	ctx.state = QD_INPUT_STATE_STARTED;
	ctx.state_change_time = qd_get_time();

	/* play */
	assert_int(0, ==, ffmpeg_src_thread_start(src));
	usleep(1500000);

	for (int i = 0; i < 3; i++) {
		/* block input feeding, and pause */
		pthread_mutex_lock(&ctx.lock);
		assert_int(0, ==, qd_input_block(input, true));
		assert_int(0, ==, qd_input_pause(input));
		ctx.state = QD_INPUT_STATE_PAUSED;
		ctx.state_change_time = qd_get_time();
		pthread_mutex_unlock(&ctx.lock);
		usleep(1500000);

		/* resume input feeding while paused, this should not have any
		 * impact */
		assert_int(0, ==, qd_input_block(input, false));
		usleep(1500000);

		/* resume input playback */
		pthread_mutex_lock(&ctx.lock);
		assert_int(0, ==, qd_input_start(input));
		ctx.state = QD_INPUT_STATE_STARTED;
		ctx.state_change_time = qd_get_time();
		pthread_mutex_unlock(&ctx.lock);
		usleep(1500000);
	}

	ffmpeg_src_thread_stop(src);
	ffmpeg_src_thread_join(src);

	ffmpeg_src_destroy(src);
	qd_session_destroy(session);

	for (size_t i = 0; i < QD_N_ELEMENTS(ctx.pa); i++)
		peak_analyzer_cleanup(&ctx.pa[i]);

	return MUNIT_OK;
}

static char *parm_ms12_files_pause[] = {
	"ddp", "aac_adts", "aac_loas", NULL
};

static char *parm_ms12_max_delay_pause[] = {
	"100", NULL
};

static MunitParameterEnum parms_ms12_pause[] = {
	{ "t", parm_ms12_sessions_ott_only },
	{ "o", parm_ms12_outputs_pcm_single },
	{ "f", parm_ms12_files_pause },
	{ "max_delay_ms", parm_ms12_max_delay_pause },
	{ NULL, NULL },
};

/*
 * MS12 test suite
 */

static MunitTest ms12_tests[] = {
	{ "/ms12/channel_sweep",
	  test_ms12_channel_sweep,
	  pretest_ms12, NULL,
	  MUNIT_TEST_OPTION_NONE, parms_ms12_channel_sweep },
	{ "/ms12/assoc_mix",
	  test_ms12_assoc_mix,
	  pretest_ms12, NULL,
	  MUNIT_TEST_OPTION_NONE, parms_ms12_assoc_mix },
	{ "/ms12/stereo_downmix",
	  test_ms12_stereo_downmix,
	  pretest_ms12, NULL,
	  MUNIT_TEST_OPTION_NONE, parms_ms12_stereo_downmix },
	{ "/ms12/drc",
	  test_ms12_drc,
	  pretest_ms12, NULL,
	  MUNIT_TEST_OPTION_NONE, parms_ms12_drc },
	{ "/ms12/pause",
	  test_ms12_pause,
	  pretest_ms12, NULL,
	  MUNIT_TEST_OPTION_NONE, parms_ms12_pause },
	{ },
};

static const MunitSuite test_suite = {
	"",				/* name */
	ms12_tests,			/* tests */
	NULL,				/* suites */
	1,				/* iterations */
	MUNIT_SUITE_OPTION_NONE,	/* options */
};

int main(int argc, char **argv)
{
	return munit_suite_main(&test_suite, NULL, argc, argv);
}
