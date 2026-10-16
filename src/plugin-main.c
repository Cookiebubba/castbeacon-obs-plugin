/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * Adds a hidden second output that pushes OBS's program to Cast Beacon's local relay over RTMP
 * (rtmp://127.0.0.1:1935/service). From there the relay (mediamtx) fans it out as HLS to the
 * Chromecast overflow TVs, and go2rtc reads the same RTMP and serves the operator's low-latency
 * monitor over MSE-over-WebSocket. RTMP is plain TCP, so the publish is rock-solid.
 *
 * The overflow feed gets its OWN video encoder with a low keyframe interval (default 1s) so the
 * TVs + monitor run at low latency INDEPENDENT of your broadcast (which keeps its own
 * keyframe/quality for Twitch/Facebook). Optionally it runs at its own bitrate/resolution, read
 * from <module-config>/settings.json (written by the Cast Beacon app). Audio is shared with the
 * broadcast (audio has no keyframe), so this is one extra video encode.
 *
 * OBS performs all encoding — this plugin only asks OBS to create/configure an encoder.
 *
 * Self-suppression: the app also supports feeding it with OBS's MAIN stream output (the operator
 * picks the app's service in OBS and the app fans out to the platforms). In that setup the shadow
 * output would publish to the SAME relay path as the main output, and the relay only allows one
 * publisher per path — the two outputs then kick each other loose every couple of seconds. So on
 * every stream start we look at OBS's own stream service: if it points at THIS machine (by
 * loopback, any of its own addresses or its hostname) on the relay's RTMP port, the operator has
 * chosen "OBS sends straight to Cast Beacon" and the shadow output stands aside - on ANY path, so it
 * never publishes into "service" behind the operator's back (stand-aside.c holds that decision).
 * The decision runs on a worker thread (it may need a DNS lookup), and the shadow output is only
 * ever started from the UI thread once that decision is in (see "The main output check" below).
 * Everything else behaves exactly as before.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/dstr.h>

#include <util/platform.h>
#include <util/threading.h>

#include "stand-aside.h"
#include "local-host.h"
#include "cb-thread.h"

#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *SHADOW_SERVER = "rtmp://127.0.0.1:1935";
static const char *SHADOW_KEY = "service";

/* How long the decision waits on a host-name lookup before deciding "another machine, for now" and
 * letting the shadow start. A lookup is made only when the name is neither loopback nor this
 * machine's hostname AND the output's port is the relay's. It runs on a worker thread, never the UI. */
#define DECISION_TIMEBOX_MS 3000

/* How long unload waits for decisions still running before it pins the module and leaves them. */
#define UNLOAD_WAIT_MS 2000

#define MAX_DECIDERS 8

static obs_output_t *shadow_output = NULL;
static obs_service_t *shadow_service = NULL;
static obs_encoder_t *local_venc = NULL; /* our dedicated low-latency encoder */

/* THE SETTINGS CONTRACT (app -> plugin), and why there are two paths.
 *
 * OBS derives a module's config directory from the module's BINARY NAME, so this plugin reads
 *   %APPDATA%\obs-studio\plugin_config\castbeacon\settings.json
 * and everything before the 1.2.0 -> 1.3.0 rename read
 *   %APPDATA%\obs-studio\plugin_config\castpilot\settings.json
 *
 * The app is the writer and it ships on its own schedule: Cast Beacon 2.1.1 and older know only
 * the legacy path. So the read order is NEW FIRST, LEGACY AS FALLBACK — a 1.3.0 plugin under an
 * old app still gets its settings, and the moment a dual-writing app appears the new file wins
 * with no user action. The legacy file is never written, never deleted, and never merged: it is
 * a whole-file fallback, so a present-but-empty new file legitimately means "defaults", not
 * "go look at the old one".
 *
 * REMOVAL CONDITION: drop the fallback once the app enforces a plugin-version floor of 1.3.0
 * (at which point the app can stop dual-writing too). Until then both halves must stay.
 *
 * Keys (all optional; missing file or 0 values fall back to defaults):
 *   keyint_sec   : keyframe interval in seconds (default 1 = low latency)
 *   bitrate_kbps : 0 -> match the broadcast encoder's bitrate
 *   width,height : 0 -> match the canvas (no rescale)
 *   shadow_enabled : absent/true -> normal; false -> never start the shadow output (manual
 *                    escape hatch for operators who drive the relay some other way) */
#define LEGACY_MODULE_NAME "castpilot"

/* Swap the module-name segment of an OBS module-config path for the legacy one, i.e.
 * ".../plugin_config/<module>/settings.json" -> ".../plugin_config/castpilot/settings.json".
 * Derived from the path OBS actually handed us rather than rebuilt from scratch, so it follows
 * OBS's own config root on every platform. Caller bfree()s the result. */
static char *legacy_config_path(const char *modern)
{
	if (!modern)
		return NULL;

	/* libobs joins these segments with '/' on every platform, whatever the root looks like. */
	const char *file = strrchr(modern, '/');
	if (!file || file == modern)
		return NULL;

	const char *seg = file - 1;
	while (seg > modern && *seg != '/')
		seg--;
	if (*seg != '/')
		return NULL;

	struct dstr out = {0};
	dstr_ncopy(&out, modern, (size_t)(seg - modern) + 1); /* up to and including that '/' */
	dstr_cat(&out, LEGACY_MODULE_NAME);
	dstr_cat(&out, file); /* "/settings.json" */
	return out.array;
}

static void load_local_cfg(int *keyint, int *bitrate_kbps, int *width, int *height, int *fps, bool *shadow_enabled)
{
	*keyint = 1;
	*bitrate_kbps = 0;
	*width = 0;
	*height = 0;
	*fps = 0;
	*shadow_enabled = true;

	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d) {
		char *legacy = legacy_config_path(path);
		if (legacy) {
			d = obs_data_create_from_json_file(legacy);
			if (d)
				obs_log(LOG_INFO, "settings: read the pre-rename file %s (upgrade the app to move it)",
					legacy);
			bfree(legacy);
		}
	}
	bfree(path);
	if (!d)
		return;

	if (obs_data_has_user_value(d, "keyint_sec"))
		*keyint = (int)obs_data_get_int(d, "keyint_sec");
	*bitrate_kbps = (int)obs_data_get_int(d, "bitrate_kbps");
	*width = (int)obs_data_get_int(d, "width");
	*height = (int)obs_data_get_int(d, "height");
	/* fps: absolute target framerate for the overflow feed (0 = match canvas).
	 * The Cast Beacon app writes an absolute value (60/30/24/15); the encoder can
	 * only run at an integer divisor of the canvas fps, so start_shadow snaps it. */
	*fps = (int)obs_data_get_int(d, "fps");
	if (*fps < 0 || *fps > 240) /* sanity bound against a corrupt config */
		*fps = 0;
	if (obs_data_has_user_value(d, "shadow_enabled"))
		*shadow_enabled = obs_data_get_bool(d, "shadow_enabled");
	obs_data_release(d);
}

static void stop_shadow(void)
{
	if (shadow_output) {
		obs_output_stop(shadow_output);
		obs_output_release(shadow_output);
		shadow_output = NULL;
	}
	if (local_venc) {
		obs_encoder_release(local_venc);
		local_venc = NULL;
	}
	if (shadow_service) {
		obs_service_release(shadow_service);
		shadow_service = NULL;
	}
}

/* Build and start the shadow output. UI thread only, and only once the main output check has said
 * the shadow should run (apply_verdict), or when there is nothing to check. */
static void start_shadow_now(void)
{
	if (shadow_output)
		return; /* already running */

	int keyint, bitrate, width, height, fps;
	bool shadow_enabled;
	load_local_cfg(&keyint, &bitrate, &width, &height, &fps, &shadow_enabled);
	if (keyint <= 0)
		keyint = 1;

	if (!shadow_enabled) {
		obs_log(LOG_INFO, "overflow feed disabled in settings - shadow output standing down");
		return;
	}
	obs_output_t *stream = obs_frontend_get_streaming_output();
	if (!stream) {
		obs_log(LOG_WARNING, "no streaming output; cannot start the overflow feed");
		return;
	}

	obs_encoder_t *main_venc = obs_output_get_video_encoder(stream);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(stream, 0);
	if (!main_venc || !aenc) {
		obs_log(LOG_WARNING, "broadcast encoders not ready");
		obs_output_release(stream);
		return;
	}

	/* Replicate the broadcast encoder (same codec + quality), then override the
	 * keyframe interval (and optionally bitrate) so the overflow feed is low-latency
	 * without changing anything the broadcast sends. */
	const char *enc_id = obs_encoder_get_id(main_venc);
	obs_data_t *settings = obs_encoder_get_settings(main_venc);
	if (settings) {
		obs_data_set_int(settings, "keyint_sec", keyint);
		/* No B-frames: they add latency and browser decoders smear/ghost on them. */
		obs_data_set_int(settings, "bf", 0);
		obs_data_set_bool(settings, "lookahead", false);
		/* Constrained-baseline H.264. The browser monitor (go2rtc MSE) reliably decodes
		 * baseline; NVENC's default High profile was received-but-never-decoded over the
		 * browser lane (black video). Baseline is also the most Chromecast-compatible
		 * profile for the HLS TVs, so the overflow loses nothing. */
		obs_data_set_string(settings, "profile", "baseline");
		if (bitrate > 0) {
			obs_data_set_int(settings, "bitrate", bitrate);
			obs_data_set_int(settings, "max_bitrate", bitrate);
		}
		local_venc = obs_video_encoder_create(enc_id ? enc_id : "obs_x264", "castbeacon_venc", settings, NULL);
		obs_data_release(settings);
	}

	obs_data_t *svc = obs_data_create();
	obs_data_set_string(svc, "server", SHADOW_SERVER);
	obs_data_set_string(svc, "key", SHADOW_KEY);
	shadow_service = obs_service_create("rtmp_custom", "castbeacon_service", svc, NULL);
	obs_data_release(svc);

	shadow_output = obs_output_create("rtmp_output", "castbeacon_shadow", NULL, NULL);
	obs_output_set_service(shadow_output, shadow_service);
	obs_output_set_audio_encoder(shadow_output, aenc, 0); /* share audio - no keyframe there */

	int applied_fps = 0; /* actual overflow fps after snapping (0 = unchanged / match canvas) */
	if (local_venc) {
		obs_encoder_set_video(local_venc, obs_get_video());
		if (width > 0 && height > 0)
			obs_encoder_set_scaled_size(local_venc, (uint32_t)width, (uint32_t)height);
		/* Optional framerate reduction for the overflow feed ONLY (canvas + broadcast
		 * untouched). OBS encoders run at an integer divisor of the canvas fps, so snap
		 * the requested absolute fps to the nearest achievable divisor (60->30 = 2,
		 * 60->20 = 3, ...). keyint_sec is in seconds, so the GOP stays correct at the
		 * reduced rate automatically. Must be set before the encoder starts. A target
		 * that snaps to divisor 1 (e.g. 24 on a 30fps canvas) is unachievable, so we
		 * leave the feed at canvas rate and log the real fps rather than claim a change. */
		if (fps > 0) {
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi) && ovi.fps_den > 0) {
				double canvas_fps = (double)ovi.fps_num / (double)ovi.fps_den;
				uint32_t divisor = (uint32_t)(canvas_fps / (double)fps + 0.5);
				if (divisor > 1) {
					obs_encoder_set_frame_rate_divisor(local_venc, divisor);
					applied_fps = (int)(canvas_fps / (double)divisor + 0.5);
				}
			}
		}
		obs_output_set_video_encoder(shadow_output, local_venc);
	} else {
		/* Couldn't create a dedicated encoder (e.g. NVENC session limit) -> fall back
		 * to sharing the broadcast encoder so the overflow feed still works. */
		obs_log(LOG_WARNING, "dedicated encoder unavailable; sharing the broadcast encoder");
		obs_output_set_video_encoder(shadow_output, main_venc);
	}

	if (obs_output_start(shadow_output)) {
		if (applied_fps > 0)
			obs_log(LOG_INFO, "overflow feed -> %s/%s (%s, keyint %ds%s, %dfps)", SHADOW_SERVER, SHADOW_KEY,
				local_venc ? "dedicated encoder" : "shared encoder", keyint,
				(local_venc && bitrate > 0) ? ", custom bitrate" : "", applied_fps);
		else
			obs_log(LOG_INFO, "overflow feed -> %s/%s (%s, keyint %ds%s)", SHADOW_SERVER, SHADOW_KEY,
				local_venc ? "dedicated encoder" : "shared encoder", keyint,
				(local_venc && bitrate > 0) ? ", custom bitrate" : "");
	} else {
		const char *err = obs_output_get_last_error(shadow_output);
		obs_log(LOG_WARNING, "overflow feed failed: %s", err ? err : "unknown");
		stop_shadow();
	}

	obs_output_release(stream); /* frontend get returns a new ref */
}

/* ------------------------------------------------------------------ The main output check
 *
 * On every stream start a DECIDER thread runs cb_judge() (local-host.c) on a copy of OBS's output
 * URL + key. Its answers come back to the UI thread through obs_queue_task(OBS_TASK_UI), and only
 * there is the shadow started or stopped:
 *   FINAL       - stand aside, or start the shadow;
 *   PROVISIONAL - a host-name lookup overran DECISION_TIMEBOX_MS: another machine for now, so the
 *                 shadow starts;
 *   LATE        - that lookup's answer: if it says this PC after all, the shadow is stopped.
 * Every answer carries the stream GENERATION it was asked for; one that arrives after the stream
 * stopped (or restarted) is logged and otherwise ignored. */

struct decider {
	cb_thread *thread;
	cb_event *done;
	long generation;
	char *server;
	char *key;
};

struct verdict {
	long generation;
	enum cb_phase phase;
	bool stand_aside;
	char *line;
};

/* UI thread only. */
static struct decider *deciders[MAX_DECIDERS];
static long stream_generation = 0;
static bool stream_live = false;

/* Guards `shutting_down` against a decider posting to the UI while OBS is closing. */
static pthread_mutex_t post_lock;
static bool post_lock_ok = false;
static bool shutting_down = false;

/* The stand-aside decision is logged once per CHANGE. The dedupe compares a hash of the WHOLE line,
 * so a long line that changes past any buffer's end still logs. */
static unsigned long long last_decision_hash = 0;
static bool have_decision_hash = false;

static void log_decision_once(const char *line)
{
	unsigned long long h = cb_hash64(line);
	if (have_decision_hash && h == last_decision_hash)
		return;
	have_decision_hash = true;
	last_decision_hash = h;
	obs_log(LOG_INFO, "%s", line);
}

static void apply_verdict(void *param) /* UI thread */
{
	struct verdict *v = param;

	if (!shutting_down) {
		log_decision_once(v->line);
		if (v->generation == stream_generation && stream_live) {
			if (v->phase == CB_PHASE_LATE) {
				if (v->stand_aside && shadow_output) {
					obs_log(LOG_INFO,
						"main output already feeds Cast Beacon - shadow output standing down");
					stop_shadow();
				}
			} else if (v->stand_aside) {
				obs_log(LOG_INFO,
					"main output already feeds Cast Beacon - shadow output standing down");
			} else {
				start_shadow_now();
			}
		}
	}

	bfree(v->line);
	bfree(v);
}

static void decider_report(void *ctx, enum cb_phase phase, bool stand_aside, const char *line) /* decider */
{
	struct decider *dc = ctx;
	struct verdict *v = bzalloc(sizeof(*v));
	v->generation = dc->generation;
	v->phase = phase;
	v->stand_aside = stand_aside;
	v->line = bstrdup(line);

	pthread_mutex_lock(&post_lock);
	bool post = !shutting_down;
	if (post)
		obs_queue_task(OBS_TASK_UI, apply_verdict, v, false);
	pthread_mutex_unlock(&post_lock);

	if (!post) {
		bfree(v->line);
		bfree(v);
	}
}

static void decider_main(void *arg) /* decider thread */
{
	struct decider *dc = arg;
	cb_judge(dc->server, dc->key, SHADOW_SERVER, SHADOW_KEY, DECISION_TIMEBOX_MS, decider_report, dc);
	cb_event_signal(dc->done);
}

static void decider_free(struct decider *dc)
{
	cb_event_destroy(dc->done);
	bfree(dc->server);
	bfree(dc->key);
	bfree(dc);
}

/* Join and free every decider that has finished. UI thread. */
static void reap_deciders(void)
{
	for (size_t i = 0; i < MAX_DECIDERS; i++) {
		struct decider *dc = deciders[i];
		if (dc && cb_event_wait(dc->done, 0)) {
			cb_thread_join(dc->thread);
			decider_free(dc);
			deciders[i] = NULL;
		}
	}
}

/* OBS's own stream service as copies of its server URL and key (both bfree()d by the caller).
 * False when OBS has no stream service. */
static bool read_main_output(char **server, char **key)
{
	obs_service_t *svc = obs_frontend_get_streaming_service(); /* borrowed ref - do not release */
	if (!svc)
		return false;

	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *k = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);

	/* Service types that don't implement get_connect_info: read the raw settings instead. */
	obs_data_t *settings = NULL;
	if (!url || !*url || !k) {
		settings = obs_service_get_settings(svc);
		if (settings) {
			if (!url || !*url)
				url = obs_data_get_string(settings, "server");
			if (!k)
				k = obs_data_get_string(settings, "key");
		}
	}

	*server = bstrdup(url ? url : "");
	*key = bstrdup(k ? k : "");
	obs_data_release(settings);
	return true;
}

/* STREAMING_STARTED. UI thread. */
static void begin_shadow(void)
{
	reap_deciders();
	if (shadow_output)
		return; /* already running */

	int keyint, bitrate, width, height, fps;
	bool shadow_enabled;
	load_local_cfg(&keyint, &bitrate, &width, &height, &fps, &shadow_enabled);
	if (!shadow_enabled) {
		obs_log(LOG_INFO, "overflow feed disabled in settings - shadow output standing down");
		return;
	}

	stream_generation++;
	stream_live = true;

	char *server = NULL, *key = NULL;
	if (!read_main_output(&server, &key)) {
		log_decision_once("main output check: OBS has no stream service to read - shadow output proceeds");
		start_shadow_now();
		return;
	}

	size_t slot = MAX_DECIDERS;
	for (size_t i = 0; i < MAX_DECIDERS && slot == MAX_DECIDERS; i++)
		if (!deciders[i])
			slot = i;

	struct decider *dc = NULL;
	if (slot < MAX_DECIDERS) {
		dc = bzalloc(sizeof(*dc));
		dc->generation = stream_generation;
		dc->server = server;
		dc->key = key;
		dc->done = cb_event_create();
		if (dc->done)
			dc->thread = cb_thread_start(decider_main, dc);
	}
	if (!dc || !dc->thread) {
		/* Never start blind: with no decision, a shadow could land on the operator's own path. */
		obs_log(LOG_WARNING, "main output check could not run - shadow output not started");
		if (dc)
			decider_free(dc);
		else {
			bfree(server);
			bfree(key);
		}
		return;
	}
	deciders[slot] = dc;
}

/* STREAMING_STOPPING. UI thread. */
static void end_shadow(void)
{
	stream_live = false;
	stream_generation++; /* any answer still on its way is for a stream that has ended */
	/* Stop BEFORE OBS frees the shared audio encoder (mandatory order). */
	stop_shadow();
	reap_deciders();
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	(void)data;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		begin_shadow();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		end_shadow();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* From here on nothing is posted to the UI: it is going away. */
		pthread_mutex_lock(&post_lock);
		shutting_down = true;
		pthread_mutex_unlock(&post_lock);
		break;
	default:
		break;
	}
}

/* Unload: give running decisions UNLOAD_WAIT_MS in all to finish. One still stuck in the system
 * resolver (which can take 10-30 s) is left to finish on its own - after the module is PINNED, so
 * its thread never runs code from an unloaded module. Its memory is leaked deliberately. */
static void wait_for_deciders(void)
{
	uint64_t deadline = os_gettime_ns() + (uint64_t)UNLOAD_WAIT_MS * 1000000ULL;
	bool pinned = false;
	for (size_t i = 0; i < MAX_DECIDERS; i++) {
		struct decider *dc = deciders[i];
		if (!dc)
			continue;
		uint64_t now = os_gettime_ns();
		unsigned long left = now < deadline ? (unsigned long)((deadline - now) / 1000000ULL) : 0;
		if (cb_event_wait(dc->done, left)) {
			cb_thread_join(dc->thread);
			decider_free(dc);
		} else {
			if (!pinned)
				pinned = cb_pin_module();
			obs_log(LOG_WARNING, "main output check still waiting on a lookup at exit - left to finish%s",
				pinned ? " (module kept loaded)" : " (could not keep the module loaded)");
			cb_thread_detach(dc->thread);
		}
		deciders[i] = NULL;
	}
}
/* OBS shows these in its module listing / log. The plugin has no other user-facing text —
 * there is no UI, no menu entry and no settings pane. */
MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("Name");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

bool obs_module_load(void)
{
	post_lock_ok = pthread_mutex_init(&post_lock, NULL) == 0;
	if (!post_lock_ok) {
		obs_log(LOG_WARNING, "could not create a lock - overflow feed unavailable");
		return false;
	}
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "loaded (v%s) - overflow feed ready", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (post_lock_ok) {
		pthread_mutex_lock(&post_lock);
		shutting_down = true;
		pthread_mutex_unlock(&post_lock);
	}

	stop_shadow();
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	wait_for_deciders(); /* never destroys post_lock: a detached decider may still take it */
	obs_log(LOG_INFO, "unloaded");
}
