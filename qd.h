#ifndef QD_H_
# define QD_H_

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <qap_defs.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>

extern int qd_debug_level;

#define log(l, msg, ...)						\
	do {								\
		if (qd_debug_level >= l)				\
			fprintf(stderr, "[%08" PRIu64 "] " msg,		\
				qd_get_time() / 1000, ##__VA_ARGS__);	\
	} while (0)

#define err(msg, ...) \
	log(1, "error: " msg "\n", ##__VA_ARGS__)

#define notice(msg, ...) \
	log(1, msg "\n", ##__VA_ARGS__)

#define info(msg, ...) \
	log(2, msg "\n", ##__VA_ARGS__)

#define dbg(msg, ...) \
	log(3, msg "\n", ##__VA_ARGS__)

#define QD_N_ELEMENTS(x) (sizeof (x) / sizeof (*(x)))
#define QD_MIN(a, b)	((a) < (b) ? (a) : (b))
#define QD_MAX(a, b)	((a) > (b) ? (a) : (b))
#define QD_USECOND	__INT64_C(1)
#define QD_MSECOND	__INT64_C(1000)
#define QD_SECOND	__INT64_C(1000000)

struct qd_session;
struct qd_input;

enum qd_output_id {
	QD_OUTPUT_NONE = -1,
	QD_OUTPUT_STEREO = 0,
	QD_OUTPUT_5DOT1,
	QD_OUTPUT_7DOT1,
	QD_OUTPUT_AC3,
	QD_OUTPUT_EAC3,
	QD_OUTPUT_AC3_DECODED,
	QD_OUTPUT_EAC3_DECODED,
	QD_MAX_OUTPUTS,
};

enum qd_input_id {
	QD_INPUT_MAIN,
	QD_INPUT_MAIN2,
	QD_INPUT_ASSOC,
	QD_INPUT_SYS_SOUND,
	QD_INPUT_APP_SOUND,
	QD_INPUT_OTT_SOUND,
	QD_INPUT_EXT_PCM,
	QD_MAX_INPUTS,
};

struct qd_sw_decoder;

struct qd_output {
	const char *name;
	enum qd_output_id id;
	qap_output_config_t config;
	qap_output_delay_t delay;
	bool enabled;
	bool discont;
	bool wav_enabled;
	int wav_channel_count;
	int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
	uint64_t start_time;
	uint64_t last_ts;
	uint64_t pts;
	uint64_t total_bytes;
	uint64_t total_frames;
	FILE *stream;
	struct qd_session *session;
	struct qd_sw_decoder *swdec;
};

enum qd_input_state {
	QD_INPUT_STATE_STOPPED,
	QD_INPUT_STATE_STARTED,
	QD_INPUT_STATE_PAUSED,
};

#define ADTS_HEADER_SIZE 7

enum qd_input_event {
	QD_INPUT_CONFIG_CHANGED,
};

typedef void (*qd_input_event_func_t)(struct qd_input *input,
				      enum qd_input_event ev, void *userdata);

struct qd_input {
	const char *name;
	enum qd_input_id id;
	AVFormatContext *avmux;
	uint8_t adts_header[ADTS_HEADER_SIZE];
	bool insert_adts_header;
	qap_module_handle_t module;
	qap_input_config_t config;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int buffer_size;
	bool buffer_full;
	bool terminated;
	bool blocked;
	enum qd_input_state state;
	uint64_t start_time;
	uint64_t state_change_time;
	uint64_t written_bytes;
	struct qd_session *session;
	qd_input_event_func_t event_cb_func;
	void *event_cb_data;
};

enum qd_module_type {
	QD_MODULE_DOLBY_MS12,
	QD_MODULE_DTS_M8,
	QD_MAX_MODULES,
};

typedef void (*qd_output_func_t)(struct qd_output *output,
				 qap_audio_buffer_t *buffer,
				 void *userdata);

struct qd_session {
	enum qd_module_type module;
	qap_session_handle_t handle;
	struct qd_input *inputs[QD_MAX_INPUTS];
	struct qd_output outputs[QD_MAX_OUTPUTS];
	qap_session_t type;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t eos_inputs;
	bool chmod_locking;
	bool realtime;
	bool terminated;
	int ignore_timestamps;
	int outputs_configure_count;
	char *output_dir;
	int64_t output_discard_ms;
	uint32_t buffer_size_ms;
	qd_output_func_t output_cb_func;
	void *output_cb_data;
};

#define QD_MAX_STREAMS	2

struct ffmpeg_src_stream {
	int index;
	struct qd_input *input;
};

struct ffmpeg_src {
	AVFormatContext *avctx;
	struct ffmpeg_src_stream streams[QD_MAX_STREAMS];
	int n_streams;
	pthread_t tid;
	bool terminated;
};

int qd_init(void);
uint64_t qd_get_time(void);

struct qd_session *qd_session_create(enum qd_module_type module,
				     qap_session_t type);
void qd_session_destroy(struct qd_session *session);
int qd_session_set_kvpairs(struct qd_session *session,
			   char *kvpairs_format, ...);
void qd_session_set_realtime(struct qd_session *session, bool realtime);
void qd_session_set_output_discard_ms(struct qd_session *session,
				      int64_t discard_ms);
void qd_session_set_buffer_size_ms(struct qd_session *session,
				   uint32_t buffer_size_ms);
void qd_session_ignore_timestamps(struct qd_session *session, bool ignore);
int qd_session_configure_outputs(struct qd_session *session,
				 int num_outputs,
				 const enum qd_output_id *outputs);
void qd_session_set_dump_path(struct qd_session *session, const char *path);
void qd_session_wait_eos(struct qd_session *session, enum qd_input_id input_id);
struct qd_output *qd_session_get_output(struct qd_session *session,
					enum qd_output_id id);
void qd_session_set_output_cb(struct qd_session *session, qd_output_func_t func,
			      void *userdata);

int qd_input_start(struct qd_input *input);
int qd_input_pause(struct qd_input *input);
int qd_input_stop(struct qd_input *input);
int qd_input_block(struct qd_input *input, bool block);
int qd_input_send_eos(struct qd_input *input);
uint32_t qd_input_get_buffer_size(struct qd_input *input);
int qd_input_set_buffer_size(struct qd_input *input, uint32_t buffer_size);
uint64_t qd_input_get_decoded_frames(struct qd_input *input);
int qd_input_get_decoder_io_info(struct qd_input *input,
				 qap_report_frames_t *report);
int qd_input_get_latency(struct qd_input *input);
void qd_input_terminate(struct qd_input *input);
void qd_input_destroy(struct qd_input *input);
struct qd_input *qd_input_create(struct qd_session *session,
				 enum qd_input_id id,
				 qap_module_config_t *qap_config);
struct qd_input *qd_input_create_from_avstream(struct qd_session *session,
					       enum qd_input_id id,
					       AVStream *avstream);
int qd_input_write(struct qd_input *input, void *data, int size, int64_t pts);
void qd_input_set_event_cb(struct qd_input *input, qd_input_event_func_t func,
			   void *userdata);

void ffmpeg_src_destroy(struct ffmpeg_src *src);
struct ffmpeg_src *ffmpeg_src_create(const char *url, const char *format);
uint64_t ffmpeg_src_get_duration(struct ffmpeg_src *src);
AVStream *ffmpeg_src_get_avstream(struct ffmpeg_src *src, int index);
struct qd_input *ffmpeg_src_add_input(struct ffmpeg_src *src, int index,
				      struct qd_session *session,
				      enum qd_input_id input_id);
int ffmpeg_src_seek(struct ffmpeg_src *src, int64_t position_ms);
int ffmpeg_src_read_frame(struct ffmpeg_src *src);
int ffmpeg_src_thread_start(struct ffmpeg_src *src);
void ffmpeg_src_thread_stop(struct ffmpeg_src *src);
int ffmpeg_src_thread_join(struct ffmpeg_src *src);

#endif /* !QD_H_ */
