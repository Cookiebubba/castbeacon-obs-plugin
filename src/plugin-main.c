/*
 * CastPilot Bridge — OBS plugin (GPLv2).
 *
 * Adds a hidden second output that pushes OBS's program to CastPilot's local relay over RTMP
 * (rtmp://127.0.0.1:1935/service). From there the relay (mediamtx) fans it out as HLS to the
 * Chromecast overflow TVs, and go2rtc reads the same RTMP and serves the operator's low-latency
 * monitor over MSE-over-WebSocket. RTMP is plain TCP, so the publish is rock-solid.
 *
 * The overflow feed gets its OWN video encoder with a low keyframe interval (default 1s) so the
 * TVs + monitor run at low latency INDEPENDENT of your broadcast (which keeps its own
 * keyframe/quality for Twitch/Facebook). Optionally it runs at its own bitrate/resolution, read
 * from <module-config>/settings.json (written by the CastPilot app). Audio is shared with the
 * broadcast (audio has no keyframe), so this is one extra video encode.
 *
 * OBS performs all encoding — this plugin only asks OBS to create/configure an encoder.
 *
 * Self-suppression: the app also supports feeding it with OBS's MAIN stream output (the operator
 * picks the app's service in OBS and the app fans out to the platforms). In that setup the shadow
 * output would publish to the SAME relay path as the main output, and the relay only allows one
 * publisher per path — the two outputs then kick each other loose every couple of seconds. So on
 * every stream start we look at OBS's own stream service: if it already points at our local
 * ingest, the shadow output stands down. Everything else behaves exactly as before.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/dstr.h>

#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *SHADOW_SERVER = "rtmp://127.0.0.1:1935";
static const char *SHADOW_KEY = "service";
#define SHADOW_PORT 1935 /* the relay's RTMP ingest port (also RTMP's default port) */

static obs_output_t *shadow_output = NULL;
static obs_service_t *shadow_service = NULL;
static obs_encoder_t *local_venc = NULL; /* our dedicated low-latency encoder */

/* Independent overflow-feed settings, written by the CastPilot app to
 * <module config>/settings.json. Missing file or 0 values fall back to defaults:
 *   keyint_sec   : keyframe interval in seconds (default 1 = low latency)
 *   bitrate_kbps : 0 -> match the broadcast encoder's bitrate
 *   width,height : 0 -> match the canvas (no rescale)
 *   shadow_enabled : absent/true -> normal; false -> never start the shadow output (manual
 *                    escape hatch for operators who drive the relay some other way) */
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
	bfree(path);
	if (!d)
		return;

	if (obs_data_has_user_value(d, "keyint_sec"))
		*keyint = (int)obs_data_get_int(d, "keyint_sec");
	*bitrate_kbps = (int)obs_data_get_int(d, "bitrate_kbps");
	*width = (int)obs_data_get_int(d, "width");
	*height = (int)obs_data_get_int(d, "height");
	/* fps: absolute target framerate for the overflow feed (0 = match canvas).
	 * The CastPilot app writes an absolute value (60/30/24/15); the encoder can
	 * only run at an integer divisor of the canvas fps, so start_shadow snaps it. */
	*fps = (int)obs_data_get_int(d, "fps");
	if (*fps < 0 || *fps > 240) /* sanity bound against a corrupt config */
		*fps = 0;
	if (obs_data_has_user_value(d, "shadow_enabled"))
		*shadow_enabled = obs_data_get_bool(d, "shadow_enabled");
	obs_data_release(d);
}

/* Does this RTMP URL address our own local ingest? Compared structurally (host + port only) so
 * rtmp:// vs rtmps://, an app path or trailing slashes, a user:pass@ prefix and an omitted
 * (default) port all still match. */
static bool url_is_local_ingest(const char *url)
{
	if (!url || !*url)
		return false;

	const char *sep = strstr(url, "://");
	const char *p = sep ? sep + 3 : url;

	char auth[256];
	size_t n = strcspn(p, "/?"); /* the authority ends at the path or query */
	if (n == 0 || n >= sizeof(auth))
		return false;
	memcpy(auth, p, n);
	auth[n] = '\0';

	char *host = auth;
	char *at = strrchr(auth, '@'); /* drop any credentials */
	if (at)
		host = at + 1;

	const char *port = NULL;
	if (*host == '[') { /* bracketed IPv6 literal: [::1]:1935 */
		char *end = strchr(host, ']');
		if (!end)
			return false;
		*end = '\0';
		host++;
		if (end[1] == ':')
			port = end + 2;
	} else {
		char *colon = strchr(host, ':');
		if (colon) {
			*colon = '\0';
			port = colon + 1;
		}
	}

	if ((port && *port ? atoi(port) : SHADOW_PORT) != SHADOW_PORT)
		return false;

	return astrcmpi(host, "127.0.0.1") == 0 || astrcmpi(host, "localhost") == 0 || astrcmpi(host, "::1") == 0;
}

/* True when OBS's own stream output already publishes to our relay, i.e. the app is downstream of
 * the broadcast and a shadow output would just fight it for the path. Re-read on every stream
 * start, since the operator can switch services between streams. */
static bool main_output_feeds_us(void)
{
	obs_service_t *svc = obs_frontend_get_streaming_service(); /* borrowed ref - do not release */
	if (!svc)
		return false;

	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (url && *url)
		return url_is_local_ingest(url);

	/* Service types that don't implement get_connect_info: read the raw setting instead. */
	obs_data_t *settings = obs_service_get_settings(svc);
	if (!settings)
		return false;
	bool hit = url_is_local_ingest(obs_data_get_string(settings, "server"));
	obs_data_release(settings);
	return hit;
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

static void start_shadow(void)
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
	if (main_output_feeds_us()) {
		obs_log(LOG_INFO, "main output already feeds Cast Beacon - shadow output standing down");
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
		local_venc = obs_video_encoder_create(enc_id ? enc_id : "obs_x264", "castpilot_venc", settings, NULL);
		obs_data_release(settings);
	}

	obs_data_t *svc = obs_data_create();
	obs_data_set_string(svc, "server", SHADOW_SERVER);
	obs_data_set_string(svc, "key", SHADOW_KEY);
	shadow_service = obs_service_create("rtmp_custom", "castpilot_service", svc, NULL);
	obs_data_release(svc);

	shadow_output = obs_output_create("rtmp_output", "castpilot_shadow", NULL, NULL);
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

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	(void)data;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		start_shadow();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		/* Stop BEFORE OBS frees the shared audio encoder (mandatory order). */
		stop_shadow();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "loaded (v%s) - overflow feed ready", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	stop_shadow();
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "unloaded");
}
