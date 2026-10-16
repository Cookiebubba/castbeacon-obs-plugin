/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * The shadow output's state machine, with NO libobs in it: the plugin hands in callbacks (start/stop
 * the shadow, post a task to OBS's UI thread, log), the unit tests hand in fakes.
 *
 * On every stream start a DECIDER thread runs the judge (cb_judge) on a copy of OBS's output URL +
 * key. Its answers come back to the UI thread through post_ui, and only there is the shadow started
 * or stopped:
 *   FINAL       - stand aside, or start the shadow;
 *   PROVISIONAL - a host-name lookup overran the time box: stand aside FOR NOW;
 *   LATE        - that lookup's answer: start the shadow only if it says another machine.
 * Every answer carries the stream GENERATION it was asked for; one that arrives after the stream
 * stopped or restarted is logged and otherwise ignored. When no decider can run (all slots busy,
 * no thread), the stream stands aside - logged - and is decided again as soon as a slot frees.
 *
 * Threads: every cb_ctl_* call except cb_ctl_shutdown is made on the UI thread; so are the callbacks,
 * except post_ui (any thread) and log (also from cb_ctl_shutdown).
 */

#pragma once

#include "local-host.h"
#include "cb-thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CB_CTL_MAX_DECIDERS 8

enum cb_log_level {
	CB_LOG_INFO,
	CB_LOG_WARNING,
};

struct cb_ctl_ops {
	void *ctx;
	void (*start_shadow)(void *ctx);   /* build and start it; it may fail (then not running) */
	void (*stop_shadow)(void *ctx);    /* stop and release it; harmless when not running */
	bool (*shadow_running)(void *ctx); /* is a shadow output up? */
	void (*post_ui)(void *ctx, void (*task)(void *arg), void *arg); /* run task(arg) on the UI thread */
	void (*log)(void *ctx, enum cb_log_level level, const char *line);

	const char *relay_server; /* the shadow's own target */
	const char *relay_key;
	unsigned long timebox_ms;     /* how long a host-name lookup may take before PROVISIONAL */
	unsigned long unload_wait_ms; /* how long shutdown waits for deciders in all */

	/* Seams for the tests; NULL = the real thing. */
	cb_judge_fn judge;                                      /* cb_judge */
	cb_thread *(*thread_start)(cb_thread_fn fn, void *arg); /* cb_thread_start */
	bool (*pin_module)(void);                               /* cb_pin_module */
};

/* Set up (or, after cb_ctl_shutdown, set up again). False when a lock cannot be created. */
bool cb_ctl_init(const struct cb_ctl_ops *ops);

/* A stream started. server == NULL: OBS has no stream service to read - start the shadow. */
void cb_ctl_stream_started(const char *server, const char *key);

/* A stream is stopping or has stopped (both are safe to call, in any order, any number of times):
 * stop the shadow and make every answer still on its way stale. */
void cb_ctl_stream_stopped(void);

/* OBS is closing: nothing is posted to the UI thread from here on. */
void cb_ctl_exit(void);

/* Unload: wait up to unload_wait_ms in all for deciders still running. One that is stuck is left to
 * finish once the module is pinned - or, if it cannot be pinned, waited for (blocking) rather than
 * left to run code from an unloaded module. Implies cb_ctl_exit. */
void cb_ctl_shutdown(void);

#ifdef __cplusplus
}
#endif
