/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * The shadow output's state machine - see shadow-control.h.
 */

#include "shadow-control.h"

#include <stdlib.h>
#include <string.h>

struct decider {
	cb_thread *thread;
	cb_event *done;
	long generation;
	char *server;
	char *key;
};

enum msg_kind {
	MSG_VERDICT, /* one of the judge's answers */
	MSG_DONE,    /* a decider finished: its slot can be reaped */
};

struct msg {
	enum msg_kind kind;
	long generation;
	enum cb_phase phase;
	bool aside;
	char *line;
};

static struct cb_ctl_ops ops;

/* Guards `exiting` against a decider posting while OBS closes. Created once; destroyed at shutdown
 * only if no decider was ever left running (a detached one may still take it). */
static cb_mutex *post_lock = NULL;
static bool ever_detached = false;
static bool exiting = false;

/* UI thread only. */
static long generation = 0; /* never reset: an answer from before a re-init stays stale */
static bool live = false;
static struct decider *slots[CB_CTL_MAX_DECIDERS];
static bool pending = false; /* a stream waiting for a free decider */
static long pending_generation = 0;
static char *pending_server = NULL;
static char *pending_key = NULL;
static unsigned long long last_hash = 0;
static bool have_hash = false;

static char *dup_str(const char *s)
{
	if (!s)
		s = "";
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

static void log_line(enum cb_log_level level, const char *line)
{
	if (ops.log)
		ops.log(ops.ctx, level, line);
}

/* The decision is logged once per CHANGE: compared by a hash of the WHOLE line. */
static void log_decision_once(const char *line)
{
	unsigned long long h = cb_hash64(line);
	if (have_hash && h == last_hash)
		return;
	have_hash = true;
	last_hash = h;
	log_line(CB_LOG_INFO, line);
}

static bool running(void)
{
	return ops.shadow_running && ops.shadow_running(ops.ctx);
}

static void start_if_down(void)
{
	if (!running() && ops.start_shadow)
		ops.start_shadow(ops.ctx);
}

static void stop_if_up(void)
{
	if (running() && ops.stop_shadow)
		ops.stop_shadow(ops.ctx);
}

/* ------------------------------------------------------------------ deciders */

static void apply(void *arg);

static void post(struct msg *m) /* any thread */
{
	cb_mutex_lock(post_lock);
	bool ok = !exiting && ops.post_ui;
	if (ok)
		ops.post_ui(ops.ctx, apply, m);
	cb_mutex_unlock(post_lock);
	if (!ok) {
		free(m->line);
		free(m);
	}
}

static void report(void *ctx, enum cb_phase phase, bool aside, const char *line) /* decider */
{
	struct decider *dc = ctx;
	struct msg *m = calloc(1, sizeof(*m));
	if (!m)
		return;
	m->kind = MSG_VERDICT;
	m->generation = dc->generation;
	m->phase = phase;
	m->aside = aside;
	m->line = dup_str(line);
	post(m);
}

static void decider_main(void *arg) /* decider */
{
	struct decider *dc = arg;
	cb_judge_fn judge = ops.judge ? ops.judge : cb_judge;
	judge(dc->server, dc->key, ops.relay_server, ops.relay_key, ops.timebox_ms, report, dc);

	long gen = dc->generation;
	cb_event_signal(dc->done); /* from here the UI may join and free dc: touch it no more */
	struct msg *m = calloc(1, sizeof(*m));
	if (m) {
		m->kind = MSG_DONE;
		m->generation = gen;
		post(m);
	}
}

static void decider_free(struct decider *dc)
{
	cb_event_destroy(dc->done);
	free(dc->server);
	free(dc->key);
	free(dc);
}

static void reap(void)
{
	for (size_t i = 0; i < CB_CTL_MAX_DECIDERS; i++) {
		struct decider *dc = slots[i];
		if (dc && cb_event_wait(dc->done, 0)) {
			cb_thread_join(dc->thread);
			decider_free(dc);
			slots[i] = NULL;
		}
	}
}

static bool any_busy(void)
{
	for (size_t i = 0; i < CB_CTL_MAX_DECIDERS; i++)
		if (slots[i])
			return true;
	return false;
}

static bool launch(const char *server, const char *key)
{
	size_t slot = CB_CTL_MAX_DECIDERS;
	for (size_t i = 0; i < CB_CTL_MAX_DECIDERS && slot == CB_CTL_MAX_DECIDERS; i++)
		if (!slots[i])
			slot = i;
	if (slot == CB_CTL_MAX_DECIDERS)
		return false;

	struct decider *dc = calloc(1, sizeof(*dc));
	if (!dc)
		return false;
	dc->generation = generation;
	dc->server = dup_str(server);
	dc->key = dup_str(key);
	dc->done = cb_event_create();
	cb_thread *(*start)(cb_thread_fn, void *) = ops.thread_start ? ops.thread_start : cb_thread_start;
	if (dc->server && dc->key && dc->done)
		dc->thread = start(decider_main, dc);
	if (!dc->thread) {
		decider_free(dc);
		return false;
	}
	slots[slot] = dc;
	return true;
}

static void clear_pending(void)
{
	free(pending_server);
	free(pending_key);
	pending_server = NULL;
	pending_key = NULL;
	pending = false;
}

/* Start a decider for this generation; reap and retry once; failing that, stand aside - and, when
 * a decider is still running whose end will free a slot, decide again then. Never silent. */
static void decide(const char *server, const char *key)
{
	if (launch(server, key))
		return;
	reap();
	if (launch(server, key))
		return;

	stop_if_up();
	if (any_busy()) {
		clear_pending();
		pending = true;
		pending_generation = generation;
		pending_server = dup_str(server);
		pending_key = dup_str(key);
		log_line(CB_LOG_WARNING,
			 "main output check: no decider free - shadow output stands aside until one is, "
			 "then decides again");
	} else {
		log_line(CB_LOG_WARNING, "main output check could not start - shadow output stands aside for this "
					 "stream (decided again at the next stream start)");
	}
}

static void apply(void *arg) /* UI */
{
	struct msg *m = arg;

	if (!exiting) {
		if (m->kind == MSG_VERDICT) {
			log_decision_once(m->line);
			if (m->generation == generation && live) {
				if (m->aside) {
					if (m->phase == CB_PHASE_FINAL || running())
						log_line(CB_LOG_INFO,
							 "main output already feeds Cast Beacon - shadow output "
							 "standing down");
					stop_if_up();
				} else {
					start_if_down();
				}
			}
		} else {
			reap();
			if (pending && pending_generation == generation && live) {
				char *server = pending_server;
				char *key = pending_key;
				pending_server = NULL;
				pending_key = NULL;
				pending = false;
				decide(server, key);
				free(server);
				free(key);
			}
		}
	}

	free(m->line);
	free(m);
}

/* ------------------------------------------------------------------ the interface */

bool cb_ctl_init(const struct cb_ctl_ops *o)
{
	if (!o)
		return false;
	ops = *o;
	if (!post_lock)
		post_lock = cb_mutex_create();
	if (!post_lock)
		return false;
	cb_mutex_lock(post_lock);
	exiting = false;
	cb_mutex_unlock(post_lock);
	live = false;
	have_hash = false;
	clear_pending();
	return true;
}

void cb_ctl_stream_started(const char *server, const char *key)
{
	reap();
	generation++;
	live = true;
	clear_pending();

	if (running()) {
		/* a STOPPING/STOPPED never came for the last stream: that shadow decided for another */
		log_line(CB_LOG_INFO, "main output check: a shadow output from an earlier stream start was still "
				      "running - stopped, deciding again");
		ops.stop_shadow(ops.ctx);
	}

	if (!server) {
		log_decision_once("main output check: OBS has no stream service to read - shadow output proceeds");
		start_if_down();
		return;
	}
	decide(server, key);
}

void cb_ctl_stream_stopped(void)
{
	live = false;
	generation++; /* every answer still on its way is now stale */
	clear_pending();
	stop_if_up();
	reap();
}

void cb_ctl_exit(void)
{
	if (!post_lock)
		return;
	cb_mutex_lock(post_lock);
	exiting = true;
	cb_mutex_unlock(post_lock);
}

void cb_ctl_shutdown(void)
{
	cb_ctl_exit();
	clear_pending();
	live = false;

	bool (*pin)(void) = ops.pin_module ? ops.pin_module : cb_pin_module;
	unsigned long long deadline = cb_now_ms() + ops.unload_wait_ms;
	bool tried_pin = false, pinned = false;

	for (size_t i = 0; i < CB_CTL_MAX_DECIDERS; i++) {
		struct decider *dc = slots[i];
		if (!dc)
			continue;
		slots[i] = NULL;

		unsigned long long now = cb_now_ms();
		unsigned long left = now < deadline ? (unsigned long)(deadline - now) : 0;
		if (cb_event_wait(dc->done, left)) {
			cb_thread_join(dc->thread);
			decider_free(dc);
			continue;
		}

		if (!tried_pin) {
			tried_pin = true;
			pinned = pin();
		}
		if (pinned) {
			log_line(CB_LOG_WARNING, "main output check still waiting on a lookup at exit - left to finish "
						 "(module kept loaded)");
			cb_thread_detach(dc->thread); /* dc is leaked on purpose: its thread still uses it */
			ever_detached = true;
		} else {
			log_line(CB_LOG_WARNING, "main output check still waiting on a lookup at exit, and the module "
						 "cannot be kept loaded - waiting for it");
			cb_thread_join(dc->thread);
			decider_free(dc);
		}
	}

	if (!ever_detached && post_lock) {
		cb_mutex_destroy(post_lock);
		post_lock = NULL;
	}
}
