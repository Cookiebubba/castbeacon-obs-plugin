/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * Unit test for the stand-aside decision (src/stand-aside.c). Builds without libobs or OBS:
 *   cmake -S tests -B build_tests && cmake --build build_tests --config Release
 *   ctest --test-dir build_tests -C Release --output-on-failure
 * Every machine in here is made up: no interface is enumerated and no name is looked up.
 */

#include "stand-aside.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                    \
	do {                                                \
		checks++;                                   \
		if (!(cond)) {                              \
			failures++;                         \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                \
			printf("\n");                       \
		}                                           \
	} while (0)

/* ------------------------------------------------------------------ the made-up machine */

#define RELAY_PORT 1935
#define RELAY_PATH "service"

static struct cb_addr local_addrs[5];
static struct cb_local this_pc;

static struct cb_addr ip(const char *s)
{
	struct cb_addr a;
	if (!cb_parse_ip(s, strlen(s), &a)) {
		printf("FAIL fixture: %s is not an IP\n", s);
		failures++;
	}
	return a;
}

static void setup_machine(void)
{
	local_addrs[0] = ip("127.0.0.1");
	local_addrs[1] = ip("192.168.1.20");
	local_addrs[2] = ip("::1");
	local_addrs[3] = ip("fe80::1c2d:3e4f:5a6b:7c8d");
	local_addrs[4] = ip("2001:db8::20");
	this_pc.addrs = local_addrs;
	this_pc.n_addrs = 5;
	this_pc.hostname = "Studio-PC";
}

/* Decide for a server URL + key on the made-up machine, with an optional DNS answer. */
static struct cb_decision decide_rs(const char *server, const char *key, enum cb_resolve rs,
				    const struct cb_addr *resolved, size_t n_resolved)
{
	struct cb_url u;
	bool parsed = cb_url_parse(server, key, &u);
	struct cb_decision d;
	cb_decide(&u, parsed, &this_pc, rs, resolved, n_resolved, RELAY_PORT, RELAY_PATH, &d);
	return d;
}

static struct cb_decision decide(const char *server, const char *key)
{
	return decide_rs(server, key, CB_RESOLVE_NOT_TRIED, NULL, 0);
}

#define EXPECT(server, key, want_verdict, want_host)                                                  \
	do {                                                                                          \
		struct cb_decision d_ = decide(server, key);                                          \
		CHECK(d_.verdict == (want_verdict) && d_.host == (want_host),                         \
		      "%s + key \"%s\": got verdict %d (%s) host %d (%s), want %d host %d", server ? server : "(null)", \
		      key ? key : "(null)", d_.verdict, cb_verdict_text(d_.verdict), d_.host,           \
		      cb_host_kind_text(d_.host), (want_verdict), (want_host));                       \
	} while (0)

/* ------------------------------------------------------------------ cases */

static void test_relay_target(void)
{
	struct cb_url u;
	CHECK(cb_url_parse("rtmp://127.0.0.1:1935", "service", &u), "the shadow's own target parses");
	CHECK(u.port == 1935, "shadow port %d", u.port);
	CHECK(strcmp(u.path, "service") == 0, "shadow path '%s'", u.path);
	CHECK(strcmp(u.host, "127.0.0.1") == 0, "shadow host '%s'", u.host);
}

static void test_loopback(void)
{
	EXPECT("rtmp://127.0.0.1:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://127.0.0.1:1935/", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://127.0.0.1", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK); /* default port */
	EXPECT("rtmp://localhost:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://LOCALHOST.:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://studio.localhost:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://127.0.0.2:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);  /* all of 127/8 */
	EXPECT("rtmp://127.0.0.1:1935/service", "", CB_STAND_ASIDE, CB_HOST_LOOPBACK); /* path in URL */
	EXPECT("rtmp://127.0.0.1:1935/service/", NULL, CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmps://localhost:1935/service/", "", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("  rtmp://127.0.0.1:1935  ", " service ", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("127.0.0.1:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK); /* no scheme = rtmp */
	EXPECT("rtmp://user:secret@127.0.0.1:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	/* a query on the key (publish credentials) is not part of the relay path */
	EXPECT("rtmp://127.0.0.1:1935", "service?user=a&pass=b", CB_STAND_ASIDE, CB_HOST_LOOPBACK);

	/* credentials never reach the host the log line names */
	struct cb_url u;
	CHECK(cb_url_parse("rtmp://user:secret@127.0.0.1:1935", "service", &u) && strcmp(u.host, "127.0.0.1") == 0,
	      "host without credentials, got '%s'", u.host);
}

static void test_lan_ip(void)
{
	/* THE BUG: OBS pointed at this PC by its LAN address was not recognised. */
	EXPECT("rtmp://192.168.1.20:1935", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://192.168.1.20", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://192.168.1.20:1935/service", "", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://user:pw@192.168.1.20:1935/", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
}

static void test_ipv6(void)
{
	EXPECT("rtmp://[::1]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://[::1]", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://[0:0:0:0:0:0:0:1]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://[2001:db8::20]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://[2001:DB8:0:0:0:0:0:20]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://[fe80::1c2d:3e4f:5a6b:7c8d%25eth0]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://[::ffff:192.168.1.20]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOCAL_ADDR); /* v4-mapped */
	EXPECT("rtmp://[::ffff:127.0.0.1]:1935", "service", CB_STAND_ASIDE, CB_HOST_LOOPBACK);
	EXPECT("rtmp://[2001:db8::99]:1935", "service", CB_RUN_OTHER_HOST, CB_HOST_OTHER);
	EXPECT("rtmp://[2001:db8::20]:1935", "other", CB_RUN_OTHER_PATH, CB_HOST_LOCAL_ADDR);
}

static void test_hostname(void)
{
	EXPECT("rtmp://studio-pc:1935", "service", CB_STAND_ASIDE, CB_HOST_HOSTNAME);
	EXPECT("rtmp://STUDIO-PC.:1935", "service", CB_STAND_ASIDE, CB_HOST_HOSTNAME);

	/* A name that is not the hostname: only DNS can place it, so the decision asks for it... */
	EXPECT("rtmp://studio-pc.lan:1935", "service", CB_NEED_RESOLVE, CB_HOST_NOT_CHECKED);

	/* ...and with the answer in hand: */
	struct cb_addr lan = ip("192.168.1.20"), other = ip("10.0.0.5"), lo = ip("127.0.0.1");
	struct cb_decision d = decide_rs("rtmp://studio-pc.lan:1935", "service", CB_RESOLVE_OK, &lan, 1);
	CHECK(d.verdict == CB_STAND_ASIDE && d.host == CB_HOST_RESOLVES_LOCAL, "name -> LAN IP: verdict %d host %d",
	      d.verdict, d.host);
	struct cb_addr both[2] = {other, lan};
	d = decide_rs("rtmp://studio-pc.lan:1935", "service", CB_RESOLVE_OK, both, 2);
	CHECK(d.verdict == CB_STAND_ASIDE, "any local answer is enough: verdict %d", d.verdict);
	d = decide_rs("rtmp://my-alias:1935", "service", CB_RESOLVE_OK, &lo, 1);
	CHECK(d.verdict == CB_STAND_ASIDE && d.host == CB_HOST_RESOLVES_LOCAL, "name -> loopback: verdict %d host %d",
	      d.verdict, d.host);
	d = decide_rs("rtmp://other-pc.lan:1935", "service", CB_RESOLVE_OK, &other, 1);
	CHECK(d.verdict == CB_RUN_OTHER_HOST && d.host == CB_HOST_OTHER, "name -> elsewhere: verdict %d host %d",
	      d.verdict, d.host);

	/* An unresolved name (error or time box ran out) is NOT this PC. */
	d = decide_rs("rtmp://studio-pc.lan:1935", "service", CB_RESOLVE_FAILED, NULL, 0);
	CHECK(d.verdict == CB_RUN_OTHER_HOST && d.host == CB_HOST_UNRESOLVED, "unresolved: verdict %d host %d",
	      d.verdict, d.host);

	/* No hostname known: the name falls to DNS rather than matching anything. */
	const char *saved = this_pc.hostname;
	this_pc.hostname = NULL;
	EXPECT("rtmp://studio-pc:1935", "service", CB_NEED_RESOLVE, CB_HOST_NOT_CHECKED);
	this_pc.hostname = saved;
}

static void test_right_host_wrong_path(void)
{
	/* this PC's relay, a different path: no clash, so the shadow runs */
	EXPECT("rtmp://127.0.0.1:1935", "abc", CB_RUN_OTHER_PATH, CB_HOST_LOOPBACK);
	EXPECT("rtmp://192.168.1.20:1935", "abc", CB_RUN_OTHER_PATH, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://studio-pc:1935", "abc", CB_RUN_OTHER_PATH, CB_HOST_HOSTNAME);
	EXPECT("rtmp://127.0.0.1:1935", "Service", CB_RUN_OTHER_PATH, CB_HOST_LOOPBACK); /* paths are case-sensitive */
	EXPECT("rtmp://127.0.0.1:1935/live", "service", CB_RUN_OTHER_PATH, CB_HOST_LOOPBACK); /* live/service */
	EXPECT("rtmp://127.0.0.1:1935", "", CB_RUN_OTHER_PATH, CB_HOST_LOOPBACK);
	EXPECT("rtmp://127.0.0.1:1935", "service2", CB_RUN_OTHER_PATH, CB_HOST_LOOPBACK);
	/* a name on the right port but a different path never costs a lookup */
	EXPECT("rtmp://studio-pc.lan:1935", "abc", CB_RUN_OTHER_PATH, CB_HOST_NOT_CHECKED);
}

static void test_wrong_port(void)
{
	EXPECT("rtmp://127.0.0.1:1936", "service", CB_RUN_OTHER_PORT, CB_HOST_LOOPBACK);
	EXPECT("rtmp://192.168.1.20:8080", "service", CB_RUN_OTHER_PORT, CB_HOST_LOCAL_ADDR);
	EXPECT("rtmp://[::1]:19350", "service", CB_RUN_OTHER_PORT, CB_HOST_LOOPBACK);
	EXPECT("rtmps://localhost/service", "", CB_RUN_OTHER_PORT, CB_HOST_LOOPBACK); /* rtmps defaults to 443 */
	EXPECT("srt://127.0.0.1", "service", CB_RUN_OTHER_PORT, CB_HOST_LOOPBACK);    /* unknown default */
	EXPECT("rtmp://studio-pc.lan:1936", "service", CB_RUN_OTHER_PORT, CB_HOST_NOT_CHECKED);
}

static void test_other_machine(void)
{
	EXPECT("rtmp://192.168.1.99:1935", "service", CB_RUN_OTHER_HOST, CB_HOST_OTHER);
	EXPECT("rtmp://192.168.1.21:1935/service", "", CB_RUN_OTHER_HOST, CB_HOST_OTHER);
	EXPECT("rtmp://10.0.0.5", "service", CB_RUN_OTHER_HOST, CB_HOST_OTHER);
	/* a platform: right port, different path - decided without a lookup */
	EXPECT("rtmp://a.rtmp.youtube.com/live2", "abcd-efgh-ijkl-mnop", CB_RUN_OTHER_PATH, CB_HOST_NOT_CHECKED);
	EXPECT("rtmps://live-api-s.facebook.com:443/rtmp/", "FB-123", CB_RUN_OTHER_PORT, CB_HOST_NOT_CHECKED);
}

static void test_unparseable(void)
{
	EXPECT(NULL, "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("   ", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("not a url", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp:///service", "", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://user@", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://127.0.0.1:99999", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://127.0.0.1:0", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://127.0.0.1:19x5", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://[::1:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://[::1]x", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://[1.2.3.4]:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://[gggg::1]:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rtmp://bad host:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("://127.0.0.1:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
	EXPECT("rt mp://127.0.0.1:1935", "service", CB_RUN_UNPARSEABLE, CB_HOST_UNPARSEABLE);
}

static void test_ip_parser(void)
{
	struct cb_addr a;
#define IP_OK(s) CHECK(cb_parse_ip(s, strlen(s), &a), "should parse: %s", s)
#define IP_BAD(s) CHECK(!cb_parse_ip(s, strlen(s), &a), "should not parse: %s", s)
	IP_OK("0.0.0.0");
	IP_OK("255.255.255.255");
	IP_OK("::");
	IP_OK("::1");
	IP_OK("1::");
	IP_OK("1:2:3:4:5:6:7:8");
	IP_OK("1:2:3:4:5:6:1.2.3.4");
	IP_OK("::ffff:10.0.0.1");
	IP_OK("fe80::1%eth0");
	IP_BAD("256.1.1.1");
	IP_BAD("1.2.3");
	IP_BAD("1.2.3.4.5");
	IP_BAD("127.1");
	IP_BAD("010.0.0.1");
	IP_BAD("1.2.3.");
	IP_BAD(":::");
	IP_BAD("1::2::3");
	IP_BAD("1:2:3:4:5:6:7:8:9");
	IP_BAD("1:2:3:4:5:6:7");
	IP_BAD("1:2:3:4:5:6:7:8::");
	IP_BAD("12345::1");
	IP_BAD(":1::");
	IP_BAD("1:");
	IP_BAD("::1.2.3.4:5");
	IP_BAD("localhost");
#undef IP_OK
#undef IP_BAD

	struct cb_addr x = ip("::ffff:192.168.1.20"), y = ip("192.168.1.20");
	CHECK(cb_addr_equal(&x, &y), "a v4-mapped address equals its v4 form");
	x = ip("2001:db8::20");
	y = ip("2001:0db8:0000:0000:0000:0000:0000:0020");
	CHECK(cb_addr_equal(&x, &y), "two spellings of one v6 address are equal");
	x = ip("::");
	CHECK(!cb_addr_is_loopback(&x), ":: is not loopback");
}

int main(void)
{
	setup_machine();
	test_relay_target();
	test_loopback();
	test_lan_ip();
	test_ipv6();
	test_hostname();
	test_right_host_wrong_path();
	test_wrong_port();
	test_other_machine();
	test_unparseable();
	test_ip_parser();

	printf("stand-aside: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
