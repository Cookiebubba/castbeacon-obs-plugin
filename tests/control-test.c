/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * Unit test for the shadow output's state machine (src/shadow-control.c), with OBS faked: a fake
 * shadow (start/stop counted, can be made to fail), a fake UI thread (a queue this test pumps), a
 * scripted judge whose answers and timing the test controls, a failing thread starter and a fake pin.
 */

#include "shadow-control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		checks++;                                           \
		if (!(cond)) {                                      \
			failures++;                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                        \
			printf("\n");                               \
		}                                                   \
	} while (0)

/* ------------------------------------------------------------------ the fake shadow */

static bool shadow_up = false;
static int starts = 0, stops = 0;
static bool start_fails = false;

static void fake_start(void *ctx)
{
	(void)ctx;
	starts++;
	if (!start_fails)
		shadow_up = true;
}

static void fake_stop(void *ctx)
{
	(void)ctx;
	stops++;
	shadow_up = false;
}

static bool fake_running(void *ctx)
{
	(void)ctx;
	return shadow_up;
}

/* ------------------------------------------------------------------ the fake UI thread */

struct task {
	void (*fn)(void *);
	void *arg;
	struct task *next;
};

static cb_mutex *queue_lock;
static struct task *queue_head = NULL, *queue_tail = NULL;

static void fake_post(void *ctx, void (*fn)(void *), void *arg)
{
	(void)ctx;
	struct task *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->fn = fn;
	t->arg = arg;
	cb_mutex_lock(queue_lock);
	if (queue_tail)
		queue_tail->next = t;
	else
		queue_head = t;
	queue_tail = t;
	cb_mutex_unlock(queue_lock);
}

/* Run everything queued so far, on this ("UI") thread. Returns how many ran. */
static int pump(void)
{
	int ran = 0;
	for (;;) {
		cb_mutex_lock(queue_lock);
		struct task *t = queue_head;
		if (t) {
			queue_head = t->next;
			if (!queue_head)
				queue_tail = NULL;
		}
		cb_mutex_unlock(queue_lock);
		if (!t)
			return ran;
		t->fn(t->arg);
		free(t);
		ran++;
	}
}

/* Pump until cond() holds or ms run out. */
static bool pump_until(bool (*cond)(void), unsigned long ms)
{
	unsigned long long until = cb_now_ms() + ms;
	for (;;) {
		pump();
		if (cond())
			return true;
		if (cb_now_ms() >= until)
			return false;
		cb_sleep_ms(5);
	}
}

/* Pump for a while regardless (lets stragglers land). */
static void pump_for(unsigned long ms)
{
	unsigned long long until = cb_now_ms() + ms;
	while (cb_now_ms() < until) {
		pump();
		cb_sleep_ms(5);
	}
	pump();
}

/* ------------------------------------------------------------------ the log */

static char log_text[16384];
static int warnings = 0;

static void fake_log(void *ctx, enum cb_log_level level, const char *line)
{
	(void)ctx;
	if (level == CB_LOG_WARNING)
		warnings++;
	size_t used = strlen(log_text);
	snprintf(log_text + used, sizeof(log_text) - used, "%s\n", line);
}

/* ------------------------------------------------------------------ the scripted judge
 *
 * The server string is the script:
 *   "run" / "aside"           FINAL answer at once
 *   "slow-run" / "slow-aside" PROVISIONAL (aside) at once, then waits for the GATE, then LATE
 *   "block-run"               waits for the GATE, then FINAL run
 * Every judge call is counted. */

static cb_event *gate = NULL;
static volatile long judge_calls = 0;

static void scripted_judge(const char *server, const char *key, const char *relay_server, const char *relay_key,
			   unsigned long timebox_ms, cb_report_fn report, void *ctx)
{
	(void)key;
	(void)relay_server;
	(void)relay_key;
	(void)timebox_ms;
	judge_calls++;
	if (strcmp(server, "run") == 0) {
		report(ctx, CB_PHASE_FINAL, false, "main output check: test - run");
	} else if (strcmp(server, "aside") == 0) {
		report(ctx, CB_PHASE_FINAL, true, "main output check: test - aside");
	} else if (strncmp(server, "slow-", 5) == 0) {
		report(ctx, CB_PHASE_PROVISIONAL, true, "main output check: test - provisional");
		cb_event_wait(gate, CB_WAIT_FOREVER);
		bool aside = strcmp(server + 5, "aside") == 0;
		report(ctx, CB_PHASE_LATE, aside,
		       aside ? "main output check: test - late aside" : "main output check: test - late run");
	} else if (strcmp(server, "block-run") == 0) {
		cb_event_wait(gate, CB_WAIT_FOREVER);
		report(ctx, CB_PHASE_FINAL, false, "main output check: test - run after block");
	}
}

/* ------------------------------------------------------------------ thread starter + pin fakes */

static int thread_failures_left = 0;

static cb_thread *flaky_thread_start(cb_thread_fn fn, void *arg)
{
	if (thread_failures_left > 0) {
		thread_failures_left--;
		return NULL;
	}
	return cb_thread_start(fn, arg);
}

static bool pin_result = true;
static int pins = 0;

static bool fake_pin(void)
{
	pins++;
	return pin_result;
}

/* ------------------------------------------------------------------ setup */

static void reset(void)
{
	pump();
	shadow_up = false;
	starts = stops = 0;
	start_fails = false;
	log_text[0] = '\0';
	warnings = 0;
	thread_failures_left = 0;
	pin_result = true;
	pins = 0;
	judge_calls = 0;
	if (gate)
		cb_event_destroy(gate);
	gate = cb_event_create();

	struct cb_ctl_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.start_shadow = fake_start;
	ops.stop_shadow = fake_stop;
	ops.shadow_running = fake_running;
	ops.post_ui = fake_post;
	ops.log = fake_log;
	ops.relay_server = "rtmp://127.0.0.1:1935";
	ops.relay_key = "service";
	ops.timebox_ms = 100;
	ops.unload_wait_ms = 150;
	ops.judge = scripted_judge;
	ops.thread_start = flaky_thread_start;
	ops.pin_module = fake_pin;
	CHECK(cb_ctl_init(&ops), "init");
}

/* Release the gate, let every decider finish, then shut down cleanly (no stuck threads). */
static void finish(void)
{
	cb_event_signal(gate);
	pump_for(100);
	cb_ctl_shutdown();
	pump();
}

static bool is_up(void)
{
	return shadow_up;
}

static bool started_once(void)
{
	return starts >= 1;
}

/* ------------------------------------------------------------------ cases */

static void test_final_answers(void)
{
	reset();
	cb_ctl_stream_started("run", "k");
	CHECK(!shadow_up, "nothing starts before the decision is in");
	CHECK(pump_until(is_up, 2000), "FINAL run starts the shadow");
	CHECK(starts == 1, "once (%d)", starts);
	finish();

	reset();
	cb_ctl_stream_started("aside", "k");
	pump_for(100);
	CHECK(!shadow_up && starts == 0, "FINAL aside never starts it");
	CHECK(strstr(log_text, "standing down") != NULL, "and says so: %s", log_text);
	finish();

	reset();
	cb_ctl_stream_started(NULL, NULL); /* OBS has no stream service */
	CHECK(shadow_up && judge_calls == 0, "no service: starts at once, no decider");
	finish();
}

static void test_provisional_then_late(void)
{
	/* L1: a slow name stands aside until its late answer - the shadow starts only on "elsewhere" */
	reset();
	cb_ctl_stream_started("slow-run", "k");
	pump_for(100);
	CHECK(!shadow_up && starts == 0, "PROVISIONAL never starts the shadow");
	cb_event_signal(gate);
	CHECK(pump_until(is_up, 2000), "the LATE 'elsewhere' answer starts it");
	finish();

	reset();
	cb_ctl_stream_started("slow-aside", "k");
	pump_for(100);
	cb_event_signal(gate);
	pump_for(150);
	CHECK(!shadow_up && starts == 0, "a LATE 'this PC' answer leaves it off, and it never ran");
	finish();
}

static void test_stale_answers(void)
{
	/* stop before the late answer: the answer is stale, nothing starts */
	reset();
	cb_ctl_stream_started("slow-run", "k");
	pump_for(50);
	cb_ctl_stream_stopped();
	cb_event_signal(gate);
	pump_for(150);
	CHECK(!shadow_up && starts == 0, "a late answer after STOPPING is ignored");
	finish();

	/* an answer for an EARLIER start lands while a newer stream is live: it must not act */
	reset();
	cb_ctl_stream_started("block-run", "k"); /* start A: its "run" answer is held at the gate */
	cb_ctl_stream_started("aside", "k");     /* start B (no stop between): stand aside */
	pump_for(50);
	CHECK(!shadow_up, "B stands aside");
	cb_event_signal(gate); /* A's late "run" arrives now */
	pump_for(150);
	CHECK(!shadow_up && starts == 0, "A's stale answer is ignored (starts %d)", starts);
	finish();

	/* STOPPING then STOPPED (L2): both stop; the second is harmless */
	reset();
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(is_up, 2000), "running");
	cb_ctl_stream_stopped(); /* STOPPING */
	cb_ctl_stream_stopped(); /* STOPPED */
	CHECK(!shadow_up && stops == 1, "stopped once (%d)", stops);
	finish();

	/* a stream that failed may only send STOPPED: that alone stops the shadow */
	reset();
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(is_up, 2000), "running");
	cb_ctl_stream_stopped(); /* STOPPED only */
	CHECK(!shadow_up && stops == 1, "STOPPED alone stops it");
	finish();

	/* the shadow fails to start: not running, and a later stop is quiet */
	reset();
	start_fails = true;
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(started_once, 2000), "a start was tried");
	CHECK(!shadow_up, "and it failed");
	cb_ctl_stream_stopped();
	CHECK(stops == 0, "nothing to stop (%d)", stops);
	finish();
}

static void test_restart_without_stop(void)
{
	/* L2: STARTED again with no STOPPING/STOPPED - the old shadow is stopped, then decided again */
	reset();
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(is_up, 2000), "first stream runs");
	cb_ctl_stream_started("aside", "k");
	CHECK(!shadow_up && stops == 1, "the earlier shadow is stopped at once");
	CHECK(strstr(log_text, "earlier stream start") != NULL, "and that is logged");
	pump_for(100);
	CHECK(!shadow_up && starts == 1, "the new decision (aside) is honoured");
	finish();

	reset();
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(is_up, 2000), "first stream runs");
	cb_ctl_stream_started("run", "k");
	CHECK(!shadow_up, "stopped for the re-decision");
	CHECK(pump_until(is_up, 2000), "and started again on its answer");
	CHECK(starts == 2 && stops == 1, "starts %d stops %d", starts, stops);

	/* an answer for the FIRST start that lands now must not act */
	finish();
}

static void test_no_free_decider(void)
{
	/* L3: thread creation fails once - reaped and retried, the stream is decided */
	reset();
	thread_failures_left = 1;
	cb_ctl_stream_started("run", "k");
	CHECK(pump_until(is_up, 2000), "one failed start is retried");
	CHECK(warnings == 0, "silently, since it worked");
	finish();

	/* thread creation always fails, nothing running to free a slot: aside, and LOGGED */
	reset();
	thread_failures_left = 1000;
	cb_ctl_stream_started("run", "k");
	pump_for(50);
	CHECK(!shadow_up && warnings == 1, "stands aside with a warning (%d)", warnings);
	CHECK(strstr(log_text, "could not start") != NULL, "says so: %s", log_text);
	thread_failures_left = 0;
	finish();

	/* every slot busy: the stream stands aside, then is decided when a slot frees */
	reset();
	for (int i = 0; i < CB_CTL_MAX_DECIDERS; i++)
		cb_ctl_stream_started("block-run", "k"); /* each start makes the previous one stale */
	long before = judge_calls;
	cb_ctl_stream_started("run", "k");
	pump_for(50);
	CHECK(!shadow_up && warnings == 1, "all slots busy: aside + warning (%d)", warnings);
	CHECK(strstr(log_text, "no decider free") != NULL, "says so: %s", log_text);
	cb_event_signal(gate); /* the blocked deciders finish; their slots free up */
	CHECK(pump_until(is_up, 3000), "decided again once a slot freed, and started");
	CHECK(judge_calls > before, "a new decision ran (%ld)", judge_calls - before);
	CHECK(starts == 1, "only the current stream's answer started it (%d)", starts);
	finish();
}

static void test_exit_and_unload(void)
{
	/* after EXIT nothing posted reaches the UI */
	reset();
	cb_ctl_stream_started("slow-run", "k");
	pump_for(50);
	cb_ctl_exit();
	cb_event_signal(gate);
	pump_for(150);
	CHECK(!shadow_up && starts == 0, "an answer after EXIT does nothing");
	cb_ctl_shutdown();
	pump();

	/* L4: a decider stuck at unload, module pinned: left to finish (detached), shutdown is quick */
	reset();
	cb_ctl_stream_started("block-run", "k");
	unsigned long long t0 = cb_now_ms();
	cb_ctl_shutdown();
	unsigned long long took = cb_now_ms() - t0;
	CHECK(pins == 1, "pinned once (%d)", pins);
	CHECK(took < 1000, "did not wait for the stuck decider (%llu ms)", took);
	CHECK(strstr(log_text, "module kept loaded") != NULL, "logged: %s", log_text);
	cb_event_signal(gate); /* let the detached one finish */
	pump_for(150);
	CHECK(!shadow_up && starts == 0, "its answer never acted");
}

static void release_later(void *arg)
{
	(void)arg;
	cb_sleep_ms(300);
	cb_event_signal(gate);
}

static void test_unpinnable_unload(void)
{
	reset();
	pin_result = false;
	cb_ctl_stream_started("block-run", "k");
	cb_thread *t = cb_thread_start(release_later, NULL);
	CHECK(t != NULL, "helper thread");
	unsigned long long t0 = cb_now_ms();
	cb_ctl_shutdown();
	unsigned long long took = cb_now_ms() - t0;
	CHECK(pins == 1, "tried to pin (%d)", pins);
	CHECK(took >= 250, "waited for the stuck decider rather than detach it (%llu ms)", took);
	CHECK(strstr(log_text, "waiting for it") != NULL, "logged: %s", log_text);
	cb_thread_join(t);
	pump();
	CHECK(!shadow_up, "its answer never acted");
}

int main(void)
{
	queue_lock = cb_mutex_create();
	test_final_answers();
	test_provisional_then_late();
	test_stale_answers();
	test_restart_without_stop();
	test_no_free_decider();
	test_exit_and_unload();
	test_unpinnable_unload();

	printf("shadow-control: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
