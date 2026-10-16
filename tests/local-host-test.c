/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * Harness for local-host.c: the threaded lookup and cb_judge(), the whole decision run end to end,
 * against a FAKE resolver (instant, slow, failing) - no DNS is ever asked. Also a smoke test of the
 * real interface enumeration and hostname, which need no network.
 */

#include "local-host.h"
#include "cb-thread.h"

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

#define RELAY "rtmp://127.0.0.1:1935"
#define SLOW_MS 600UL

/* ------------------------------------------------------------------ the fake resolver
 *
 * The host name says how to answer:  [slow-]<what>.test
 *   here-*    127.0.0.1 (this PC)        any-*    0.0.0.0 (this PC)
 *   mine-*    one of this machine's REAL interface addresses (found by test_this_machine)
 *   there-*   203.0.113.9 (elsewhere)    fail-*   no answer
 * "slow-" first adds SLOW_MS of delay. Every call is counted. */

static volatile long resolver_calls = 0;
static struct cb_addr my_address;
static bool have_my_address = false;

static enum cb_resolve fake_resolve(const char *host, struct cb_addr **out, size_t *n)
{
	resolver_calls++;
	*out = NULL;
	*n = 0;
	if (strncmp(host, "slow-", 5) == 0) {
		cb_sleep_ms(SLOW_MS);
		host += 5;
	}
	if (strncmp(host, "mine-", 5) == 0) {
		if (!have_my_address)
			return CB_RESOLVE_FAILED;
		struct cb_addr *m = malloc(sizeof(*m));
		if (!m)
			return CB_RESOLVE_FAILED;
		*m = my_address;
		*out = m;
		*n = 1;
		return CB_RESOLVE_OK;
	}
	const char *ip = NULL;
	if (strncmp(host, "here-", 5) == 0)
		ip = "127.0.0.1";
	else if (strncmp(host, "any-", 4) == 0)
		ip = "0.0.0.0";
	else if (strncmp(host, "there-", 6) == 0)
		ip = "203.0.113.9";
	if (!ip)
		return CB_RESOLVE_FAILED;
	struct cb_addr *a = calloc(1, sizeof(*a));
	if (!a || !cb_parse_ip(ip, strlen(ip), a)) {
		free(a);
		return CB_RESOLVE_FAILED;
	}
	*out = a;
	*n = 1;
	return CB_RESOLVE_OK;
}

/* ------------------------------------------------------------------ recording what cb_judge says */

struct answer {
	enum cb_phase phase;
	bool stand_aside;
	char line[768];
};

struct record {
	int n;
	struct answer a[4];
};

static void record_report(void *ctx, enum cb_phase phase, bool stand_aside, const char *line)
{
	struct record *r = ctx;
	if (r->n < 4) {
		r->a[r->n].phase = phase;
		r->a[r->n].stand_aside = stand_aside;
		snprintf(r->a[r->n].line, sizeof(r->a[r->n].line), "%s", line);
	}
	r->n++;
}

static struct record judge(const char *server, const char *key, unsigned long timebox_ms)
{
	struct record r;
	memset(&r, 0, sizeof(r));
	cb_judge(server, key, RELAY, "service", timebox_ms, record_report, &r);
	return r;
}

#define EXPECT_FINAL(server, key, want_aside)                                                              \
	do {                                                                                               \
		struct record r_ = judge(server, key, 2000);                                               \
		CHECK(r_.n == 1 && r_.a[0].phase == CB_PHASE_FINAL && r_.a[0].stand_aside == (want_aside), \
		      "%s: %d answers, first phase %d aside %d (%s)", server, r_.n, r_.a[0].phase,       \
		      r_.a[0].stand_aside, r_.a[0].line);                                                  \
	} while (0)

/* ------------------------------------------------------------------ cases */

static void test_lookup_api(void)
{
	struct cb_addr *a = NULL;
	size_t n = 0;

	struct cb_lookup *l = cb_lookup_start("here-a.test");
	CHECK(l != NULL, "a lookup starts");
	CHECK(cb_lookup_wait(l, 2000), "an instant answer arrives within the wait");
	CHECK(cb_lookup_finish(l, &a, &n) == CB_RESOLVE_OK && n == 1 && cb_addr_is_loopback(&a[0]),
	      "and it is the fake's loopback");
	free(a);

	l = cb_lookup_start("fail-a.test");
	CHECK(cb_lookup_finish(l, &a, &n) == CB_RESOLVE_FAILED && a == NULL && n == 0, "a failure is FAILED, no data");

	l = cb_lookup_start("slow-here-a.test");
	CHECK(!cb_lookup_wait(l, 50), "a slow answer is not in after 50 ms");
	CHECK(cb_lookup_wait(l, 5000), "but arrives in the end");
	CHECK(cb_lookup_finish(l, &a, &n) == CB_RESOLVE_OK && n == 1, "and finish still hands it over");
	free(a);

	/* finish on a lookup still running waits for it rather than losing it */
	l = cb_lookup_start("slow-there-a.test");
	CHECK(cb_lookup_finish(l, &a, &n) == CB_RESOLVE_OK && n == 1 && !cb_addr_is_loopback(&a[0]),
	      "finish waits out a slow lookup");
	free(a);

	CHECK(cb_lookup_start(NULL) == NULL && cb_lookup_start("") == NULL, "no host, no lookup");
}

static void test_judge_without_lookup(void)
{
	long before = resolver_calls;
	EXPECT_FINAL("rtmp://127.0.0.1:1935", "service", true);
	EXPECT_FINAL("rtmp://127.0.0.1:1935", "other", true);
	EXPECT_FINAL("rtmp://localhost:1935", "service", true);
	EXPECT_FINAL("rtmp://0.0.0.0:1935", "service", true);
	EXPECT_FINAL("rtmp://[::]:1935", "service", true);
	EXPECT_FINAL("rtmp://[::1]:1935", "service", true);
	EXPECT_FINAL("rtmp://127.0.0.1:1936", "service", false);
	EXPECT_FINAL("rtmp://203.0.113.9:1935", "service", false);
	EXPECT_FINAL("not a url", "service", false);
	EXPECT_FINAL("rtmps://there-x.test:443/app", "key", false); /* another port: no lookup needed */
	EXPECT_FINAL("rtmp://there-x.test:1936", "service", false);
	CHECK(resolver_calls == before, "none of these asked the resolver (%ld calls)", resolver_calls - before);
}

static void test_judge_with_lookup(void)
{
	long before = resolver_calls;
	EXPECT_FINAL("rtmp://here-x.test:1935", "service", true);
	EXPECT_FINAL("rtmp://here-x.test:1935", "other", true); /* any path */
	EXPECT_FINAL("rtmp://any-x.test:1935", "service", true);
	EXPECT_FINAL("rtmp://there-x.test:1935", "service", false);
	EXPECT_FINAL("rtmp://fail-x.test:1935", "service", false); /* unresolved = another machine */
	CHECK(resolver_calls - before == 5, "each asked the resolver once (%ld)", resolver_calls - before);
}

static void test_judge_late_answer(void)
{
	/* A slow name: PROVISIONAL is always stand aside (it may be this PC - a stray publish into the
	 * shadow's path could release that stream's pushes). The LATE answer decides. */
	struct record r = judge("rtmp://slow-here-x.test:1935", "service", 100);
	CHECK(r.n == 2, "two answers, got %d", r.n);
	CHECK(r.a[0].phase == CB_PHASE_PROVISIONAL && r.a[0].stand_aside, "first: provisional aside (%s)", r.a[0].line);
	CHECK(r.a[1].phase == CB_PHASE_LATE && r.a[1].stand_aside, "then: late stand-aside (%s)", r.a[1].line);
	CHECK(strstr(r.a[0].line, "still being looked up") != NULL, "provisional line says so: %s", r.a[0].line);
	CHECK(strstr(r.a[0].line, "standing aside for now") != NULL, "provisional line says so: %s", r.a[0].line);
	CHECK(strstr(r.a[1].line, "staying aside") != NULL, "late line says so: %s", r.a[1].line);

	/* a slow name that is elsewhere: provisional aside, then the late answer starts the shadow */
	r = judge("rtmp://slow-there-x.test:1935", "service", 100);
	CHECK(r.n == 2 && r.a[0].phase == CB_PHASE_PROVISIONAL && r.a[1].phase == CB_PHASE_LATE && r.a[0].stand_aside &&
		      !r.a[1].stand_aside,
	      "slow elsewhere: %d answers", r.n);
	CHECK(strstr(r.a[1].line, "starting the shadow output") != NULL, "late line says so: %s", r.a[1].line);

	/* a slow name that fails: provisional aside, late answer "unresolved" = another machine */
	r = judge("rtmp://slow-fail-x.test:1935", "service", 100);
	CHECK(r.n == 2 && r.a[0].stand_aside && !r.a[1].stand_aside, "slow failure: %d answers", r.n);

	/* the same slow name inside a generous time box is one FINAL answer */
	r = judge("rtmp://slow-here-x.test:1935", "service", 5000);
	CHECK(r.n == 1 && r.a[0].phase == CB_PHASE_FINAL && r.a[0].stand_aside, "slow but in time: %d answers", r.n);
}

static void test_judge_never_leaks_key(void)
{
	struct record r = judge("rtmp://slow-here-x.test:1935/SECRETPATH", "SECRETKEY?pass=SECRETQ", 100);
	for (int i = 0; i < r.n && i < 4; i++)
		CHECK(!strstr(r.a[i].line, "SECRET"), "line %d leaks: %s", i, r.a[i].line);
}

static void test_this_machine(void)
{
	/* the real enumeration and hostname: no network needed */
	struct cb_addr *addrs = NULL;
	size_t n = cb_local_addresses(&addrs);
	printf("this machine: %zu interface addresses\n", n);
	CHECK(n == 0 || addrs != NULL, "a count comes with a list");

	/* one of this machine's own non-loopback addresses, as a literal, is this PC */
	for (size_t i = 0; i < n; i++) {
		if (addrs[i].family != 4 || cb_addr_is_loopback(&addrs[i]) || cb_addr_is_unspecified(&addrs[i]))
			continue;
		char url[64];
		snprintf(url, sizeof(url), "rtmp://%u.%u.%u.%u:1935", (unsigned)addrs[i].b[0], (unsigned)addrs[i].b[1],
			 (unsigned)addrs[i].b[2], (unsigned)addrs[i].b[3]);
		EXPECT_FINAL(url, "service", true);
		EXPECT_FINAL(url, "other", true);
		my_address = addrs[i];
		have_my_address = true;
		break;
	}
	free(addrs);

	/* a name that resolves to one of this machine's REAL addresses: the address list is read AFTER
	 * the lookup (lazily) and the name is this PC */
	if (have_my_address) {
		EXPECT_FINAL("rtmp://mine-x.test:1935", "service", true);
		EXPECT_FINAL("rtmp://slow-mine-x.test:1935", "service", true); /* in the 2 s box */
		struct record r = judge("rtmp://slow-mine-x.test:1935", "service", 100);
		CHECK(r.n == 2 && r.a[0].stand_aside && r.a[1].stand_aside, "slow mine: aside, then aside");
	} else {
		printf("no non-loopback IPv4 here - real-address lookup case skipped\n");
	}

	char host[256];
	if (cb_local_hostname(host, sizeof(host))) {
		char url[300];
		snprintf(url, sizeof(url), "rtmp://%s:1935", host);
		struct cb_url u;
		if (cb_url_parse(url, "service", &u) && !u.host_is_ip) {
			long before = resolver_calls;
			EXPECT_FINAL(url, "service", true);
			CHECK(resolver_calls == before, "the hostname needs no lookup");
		} else {
			printf("hostname '%s' is not a plain name - hostname case skipped\n", host);
		}
	} else {
		printf("no hostname readable here - hostname case skipped\n");
	}
}

int main(void)
{
	cb_set_resolver(fake_resolve);
	test_lookup_api();
	test_judge_without_lookup();
	test_judge_with_lookup();
	test_judge_late_answer();
	test_judge_never_leaks_key();
	test_this_machine();
#ifdef _WIN32
	/* the pin unload relies on (Windows: the test executable pins itself, harmlessly) */
	CHECK(cb_pin_module(), "GetModuleHandleExW PIN succeeds");
#endif

	printf("local-host: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
