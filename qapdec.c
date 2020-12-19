#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <assert.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "qd.h"

volatile bool quit;
static bool qap_chmod_locking;

enum kbd_command {
	KBD_NONE,
	KBD_PLAYPAUSE,
	KBD_STOP,
	KBD_BLOCK,
	KBD_FLUSH,
};

static int kbd_ev = -1;
static enum kbd_command kbd_pending_command = KBD_NONE;

static struct ffmpeg_src *g_ffmpeg_sources[QD_MAX_INPUTS];
static struct qd_session *g_session;

static uint64_t get_cpu_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / UINT64_C(1000);
}

static struct qd_input *get_nth_input(int n)
{
	struct ffmpeg_src *src;
	int index = -1;

	if (n < 0)
		return NULL;

	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		src = g_ffmpeg_sources[i];
		if (src == NULL)
			continue;

		index += src->n_streams;
		if (index >= n)
			return src->streams[index - n].input;
	}

	return NULL;
}

static void kbd_handle_key(char key[3])
{
	enum kbd_command cmd = kbd_pending_command;

	kbd_pending_command = KBD_NONE;

	if (key[0] >= '1' && key[0] <= '9') {
		struct qd_input *input;
		int index;

		if (cmd == KBD_NONE)
			return;

		index = key[0] - '0';

		input = get_nth_input(index - 1);
		if (!input) {
			err("stream %d not found", index);
			return;
		}

		switch (cmd) {
		case KBD_PLAYPAUSE:
			if (input->state == QD_INPUT_STATE_STARTED)
				qd_input_pause(input);
			else
				qd_input_start(input);
			break;
		case KBD_STOP:
			qd_input_stop(input);
			break;
		case KBD_BLOCK:
			qd_input_block(input, !input->blocked);
			break;
		case KBD_FLUSH:
			qd_input_flush(input);
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

	} else if (key[0] == 'f') {
		kbd_pending_command = KBD_FLUSH;
		notice("Enter stream number to Flush");

	} else if (key[0] == 'c') {
		char kvpairs[32];
		int val;

		if (!g_session) {
			err("no active session");
			return;
		}

		val = !qap_chmod_locking;
		snprintf(kvpairs, sizeof (kvpairs), "chmod_locking=%d", val);

		notice("%s chmod_locking", val ? "Enable" : "Disable");

		if (!qd_session_set_kvpairs(g_session, kvpairs))
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

static bool
parse_duration(const char *s, int64_t *duration_ms)
{
	char *end;
	int64_t d;

	if (!*s)
		return false;

	*duration_ms = 0;

	while (*s) {
		d = strtoll(s, &end, 10);
		if (d == LLONG_MIN || d == LLONG_MAX)
			return false;

		if (s == end)
			return false;

		if (!strncmp(end, "ms", 2)) {
			end += 2;
		} else if (*end == 'm') {
			d *= 60000LL;
			end++;
		} else if (*end == 's') {
			d *= 1000LL;
			end++;
		}

		*duration_ms += d;
		s = end;
	}

	return true;
}

static void handle_quit(int sig)
{
	quit = true;

	qd_session_terminate(g_session);

	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		struct ffmpeg_src *src = g_ffmpeg_sources[i];
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
		"                                use '-' as argument to output to stdout instead\n"
		"  -c, --channels=<channels>    maximum number of channels to output\n"
		"  -k, --kvpairs=<kvpairs>      pass kvpairs string to the decoder backend\n"
		"  -l, --loops=<count>          number of times the stream will be decoded\n"
		"      --realtime               sync input feeding and output render to pts\n"
		"      --seek=<pos>             seek inputs to specified position first\n"
		"      --discard=<duration>     duration of output buffers to discard\n"
		"      --sec-source=<url>       source for assoc/main2 module\n"
		"      --sys-source=<url>       source for system sound module\n"
		"      --app-source=<url>       source for app sound module\n"
		"      --ott-source=<url>       source for ott sound module\n"
		"      --ext-source=<url>       source for extern pcm module\n"
		"      --sys-format=<fmt>       format for system sound module\n"
		"      --app-format=<fmt>       format for app sound module\n"
		"      --ott-format=<fmt>       format for ott sound module\n"
		"      --ext-format=<fmt>       format for extern pcm module\n"
		"\n"
		"Example usage to feed generate sine wave audio and decode an AC3 file:\n"
		"  qapdec -c 2 --sys-format lavfi --sys-source sine /data/test.ac3\n"
		"\n");
}

enum {
	OPT_SEEK = 0x200,
	OPT_DISCARD,
};

static int current_long_opt;
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
	{ "seek",              required_argument, &current_long_opt, OPT_SEEK },
	{ "discard",           required_argument, &current_long_opt, OPT_DISCARD },
	{ "sec-source",        required_argument, 0, '1' },
	{ "sys-source",        required_argument, 0, '2' },
	{ "app-source",        required_argument, 0, '3' },
	{ "sys-format",        required_argument, 0, '4' },
	{ "app-format",        required_argument, 0, '5' },
	{ "ott-source",        required_argument, 0, '6' },
	{ "ott-format",        required_argument, 0, '7' },
	{ "ext-source",        required_argument, 0, '8' },
	{ "ext-format",        required_argument, 0, '9' },
	{ 0,                   0,                 0,  0  }
};

int main(int argc, char **argv)
{
	int opt;
	int decode_err = 0;
	int loops = 1;
	int primary_stream_index = -1;
	int secondary_stream_index = -1;
	char *output_dir = NULL;
	enum qd_output_id outputs[2];
	unsigned int num_outputs = 0;
	char *kvpairs = NULL;
	struct ffmpeg_src **src = g_ffmpeg_sources;
	const char *src_url[QD_MAX_INPUTS] = { };
	const char *src_format[QD_MAX_INPUTS] = { };
	uint64_t src_duration;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t cpu_time;
	int64_t seek_position = 0;
	int64_t discard_duration = 0;
	bool render_realtime = false;
	bool kbd_enable = false;
	enum qd_module_type module;
	qap_session_t qap_session_type;
	pthread_t kbd_tid;
	AVStream *avstream;

	qd_init();
	qap_session_type = QAP_SESSION_BROADCAST;

	while ((opt = getopt_long(argc, argv, "c:f:hik:l:o:p:s:t:v",
				  long_options, NULL)) != -1) {
		if (!opt)
			opt = current_long_opt;

		switch (opt) {
		case 'c':
			if (num_outputs >= QD_N_ELEMENTS(outputs)) {
				err("too many outputs");
				usage();
				return 1;
			}
			if (!strcmp(optarg, "dd") || !strcmp(optarg, "ac3"))
				outputs[num_outputs++] = QD_OUTPUT_AC3;
			else if (!strcmp(optarg, "ddp") || !strcmp(optarg, "eac3"))
				outputs[num_outputs++] = QD_OUTPUT_EAC3;
			else if (!strcmp(optarg, "dd_dec") || !strcmp(optarg, "ac3_dec"))
				outputs[num_outputs++] = QD_OUTPUT_AC3_DECODED;
			else if (!strcmp(optarg, "ddp_dec") || !strcmp(optarg, "eac3_dec"))
				outputs[num_outputs++] = QD_OUTPUT_EAC3_DECODED;
			else if (!strcmp(optarg, "stereo") || atoi(optarg) == 2)
				outputs[num_outputs++] = QD_OUTPUT_STEREO;
			else if (!strcmp(optarg, "5.1") || atoi(optarg) == 6)
				outputs[num_outputs++] = QD_OUTPUT_5DOT1;
			else if (!strcmp(optarg, "7.1") || atoi(optarg) == 8)
				outputs[num_outputs++] = QD_OUTPUT_7DOT1;
			else {
				err("invalid output %s", optarg);
				usage();
				return 1;
			}
			break;
		case 'v':
			qd_debug_level++;
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
			src_format[QD_INPUT_MAIN] = optarg;
			break;
		case 'p':
			primary_stream_index = atoi(optarg);
			break;
		case 's':
			secondary_stream_index = atoi(optarg);
			break;
		case 'o':
			output_dir = optarg;
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
			render_realtime = true;
			break;
		case '1':
			src_url[QD_INPUT_ASSOC] = optarg;
			break;
		case '2':
			src_url[QD_INPUT_SYS_SOUND] = optarg;
			break;
		case '3':
			src_url[QD_INPUT_APP_SOUND] = optarg;
			break;
		case '4':
			src_format[QD_INPUT_SYS_SOUND] = optarg;
			break;
		case '5':
			src_format[QD_INPUT_APP_SOUND] = optarg;
			break;
		case '6':
			src_url[QD_INPUT_OTT_SOUND] = optarg;
			break;
		case '7':
			src_format[QD_INPUT_OTT_SOUND] = optarg;
			break;
		case '8':
			src_url[QD_INPUT_EXT_PCM] = optarg;
			break;
		case '9':
			src_format[QD_INPUT_EXT_PCM] = optarg;
			break;
		case 'h':
			usage();
			return 0;
		case OPT_SEEK:
			if (!parse_duration(optarg, &seek_position)) {
				err("invalid seek position %s", optarg);
				return 1;
			}
			break;
		case OPT_DISCARD:
			if (!parse_duration(optarg, &discard_duration)) {
				err("invalid discard duration %s", optarg);
				return 1;
			}
			break;
		default:
			err("unknown option %c", opt);
			usage();
			return 1;
		}
	}

	if (src_url[QD_INPUT_ASSOC] &&
	    secondary_stream_index != -1) {
		err("cannot set both secondary stream index and url");
		return 1;
	}

	if (num_outputs == 0)
		outputs[num_outputs++] = QD_OUTPUT_5DOT1;

	if (output_dir) {
		if (!strcmp(output_dir, "-")) {
			if (num_outputs != 1) {
				err("writing to stdout requires exactly one output");
				return 1;
			}
		}
	}

	if (optind < argc)
		src_url[QD_INPUT_MAIN] = argv[optind];

	if (optind + 1 < argc)
		src_url[QD_INPUT_MAIN2] = argv[optind + 1];

	if (kbd_enable)
		kbd_enable = !pthread_create(&kbd_tid, NULL, kbd_thread, NULL);

again:
	/* init ffmpeg source and demuxer */
	for (int i = 0; i < QD_MAX_INPUTS; i++)
		src[i] = NULL;

	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		if (!src_url[i])
			continue;

		src[i] = ffmpeg_src_create(src_url[i], src_format[i]);
		if (!src[i])
			return 1;
	}

	if (src[QD_INPUT_MAIN]) {
		src_duration = ffmpeg_src_get_duration(src[QD_INPUT_MAIN]);
		avstream = ffmpeg_src_get_avstream(src[QD_INPUT_MAIN],
						   primary_stream_index);
		if (!avstream) {
			err("primary stream not found");
			return 1;
		}
	} else {
		avstream = NULL;
		src_duration = 0;
	}

	if (!g_session) {
		/* load QAP library */
		if (!avstream) {
			module = QD_MODULE_DOLBY_MS12;
		} else {
			switch (avstream->codecpar->codec_id) {
			case AV_CODEC_ID_AC3:
			case AV_CODEC_ID_EAC3:
			case AV_CODEC_ID_AAC:
			case AV_CODEC_ID_AAC_LATM:
			case AV_CODEC_ID_PCM_S16LE:
			case AV_CODEC_ID_PCM_S24LE:
			case AV_CODEC_ID_PCM_S32LE:
				module = QD_MODULE_DOLBY_MS12;
				break;
			case AV_CODEC_ID_DTS:
				module = QD_MODULE_DTS_M8;
				break;
			default:
				err("cannot decode %s format",
				    avcodec_get_name(avstream->codecpar->codec_id));
				return 1;
			}
		}

		g_session = qd_session_create(module, qap_session_type);
		if (!g_session)
			return 1;

		qd_session_configure_outputs(g_session, num_outputs, outputs);
		qd_session_set_buffer_size_ms(g_session, 32);
		qd_session_set_output_discard_ms(g_session, discard_duration);
		qd_session_set_realtime(g_session, render_realtime);
		qd_session_set_dump_path(g_session, output_dir);
		if (kvpairs && qd_session_set_kvpairs(g_session, kvpairs))
			return 1;
	}

	start_time = qd_get_time();

	/* setup primary source */
	if (src[QD_INPUT_MAIN]) {
		struct qd_input *input;

		/* create primary QAP module */
		input = ffmpeg_src_add_input(src[QD_INPUT_MAIN],
					     avstream->index,
					     g_session, QD_INPUT_MAIN);
		if (!input)
			return 1;

		/* create secondary QAP module */
		if (secondary_stream_index != -1) {
			input = ffmpeg_src_add_input(src[QD_INPUT_MAIN],
						     secondary_stream_index,
						     g_session, QD_INPUT_ASSOC);
			if (!input)
				return 1;
		}
	}

	/* setup additional sources */
	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		if (i == QD_INPUT_MAIN || !src[i])
			continue;
		if (!ffmpeg_src_add_input(src[i], -1, g_session, i))
			return 1;
	}

	if (seek_position > 0) {
		for (int i = 0; i < QD_MAX_INPUTS; i++) {
			if (src[i] && ffmpeg_src_seek(src[i], seek_position))
				return 1;
		}
	}

	/* setup signal handler */
	signal(SIGINT, handle_quit);
	signal(SIGTERM, handle_quit);

	/* start input threads */
	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		if (!src[i])
			continue;
		ffmpeg_src_thread_start(src[i]);
	}

	/* wait for input threads to finish */
	if (src[QD_INPUT_MAIN]) {
		decode_err = ffmpeg_src_thread_join(src[QD_INPUT_MAIN]);
		if (decode_err)
			quit = 1;
		else {
			/* wait EOS */
			if (ffmpeg_src_wait_eos(src[QD_INPUT_MAIN],
						true, 2 * QD_SECOND))
				err("failed to drain MAIN input");
		}
	}

	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		if (i == QD_INPUT_MAIN || !src[i])
			continue;
		if (src[QD_INPUT_MAIN])
			ffmpeg_src_thread_stop(src[i]);
		ffmpeg_src_thread_join(src[i]);
	}

	/* cleanup */
	for (int i = 0; i < QD_MAX_INPUTS; i++) {
		ffmpeg_src_destroy(src[i]);
		src[i] = NULL;
	}

	end_time = qd_get_time();
	cpu_time = get_cpu_time();

	for (int i = 0; i < QD_MAX_OUTPUTS; i++) {
		struct qd_output *output;
		uint64_t frames;
		uint64_t duration;

		output = qd_session_get_output(g_session, i);
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

	if (!quit && --loops > 0)
		goto again;

	qd_session_destroy(g_session);
	g_session = NULL;

	if (!quit) {
		if (src_duration > 0) {
			notice("Elapsed: %" PRIu64 ".%" PRIu64 "s, "
			       "CPU: %" PRIu64 ".%" PRIu64 "s, "
			       "render speed: %.2fx realtime",
			       end_time / QD_SECOND, end_time % QD_SECOND / QD_MSECOND,
			       cpu_time / QD_SECOND, cpu_time % QD_SECOND / QD_MSECOND,
			       (float)src_duration / (float)(end_time - start_time));
		} else {
			notice("Elapsed: %" PRIu64 ".%" PRIu64 "s, "
			       "CPU: %" PRIu64 ".%" PRIu64 "s",
			       end_time / QD_SECOND, end_time % QD_SECOND / QD_MSECOND,
			       cpu_time / QD_SECOND, cpu_time % QD_SECOND / QD_MSECOND);
		}
	}

	if (kbd_enable) {
		handle_quit(SIGTERM);
		pthread_join(kbd_tid, NULL);
	}

	return decode_err;
}
