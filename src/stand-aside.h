/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * The stand-aside decision: does OBS's MAIN stream output already publish to the exact relay path
 * the shadow output would publish to? If it does, the two would be two publishers on one path, and
 * the relay allows one, so the shadow output must not start.
 *
 * Everything in here is PURE: no libobs, no sockets, no DNS, no clock. The caller gathers this
 * machine's facts (its interface addresses, its hostname and, only when asked for, what a name
 * resolves to) and hands them in, so the whole decision runs in a unit test with made-up machines.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CB_HOST_MAX 256
#define CB_PATH_MAX 512

/* An IP address as raw bytes. family is 4 (b[0..3] used) or 6 (all 16). IPv4-mapped IPv6
 * (::ffff:a.b.c.d) is always stored as family 4, so the two spellings of one address compare
 * equal. */
struct cb_addr {
	int family;
	unsigned char b[16];
};

/* An output URL taken apart. */
struct cb_url {
	char host[CB_HOST_MAX]; /* no brackets, no credentials, no IPv6 zone, no trailing dot */
	int port;               /* the explicit port, else the scheme's default; 0 when unknown */
	char path[CB_PATH_MAX]; /* the relay path it publishes to: URL path + "/" + key, query
	                         * dropped, slashes collapsed and trimmed. Holds the stream key -
	                         * NEVER log it. */
	bool host_is_ip;
	struct cb_addr ip; /* valid when host_is_ip */
};

/* What the caller knows about this machine. */
struct cb_local {
	const struct cb_addr *addrs; /* every address on every interface (may include loopback) */
	size_t n_addrs;
	const char *hostname; /* this machine's name, or NULL/"" when unknown */
};

/* Whether the caller has looked the URL's host name up. */
enum cb_resolve {
	CB_RESOLVE_NOT_TRIED,
	CB_RESOLVE_FAILED, /* no answer, an error, or the time box ran out */
	CB_RESOLVE_OK,
};

/* How the URL's host was placed. */
enum cb_host_kind {
	CB_HOST_UNPARSEABLE,
	CB_HOST_LOOPBACK,       /* 127.0.0.0/8, ::1, localhost */
	CB_HOST_LOCAL_ADDR,     /* one of this machine's interface addresses */
	CB_HOST_HOSTNAME,       /* this machine's own hostname */
	CB_HOST_RESOLVES_LOCAL, /* a name that resolved to an address of this machine */
	CB_HOST_OTHER,          /* another machine (an address, or a name that resolved elsewhere) */
	CB_HOST_UNRESOLVED,     /* a name that did not resolve: treated as another machine */
	CB_HOST_NOT_CHECKED,    /* a name not looked up, because port or path already rule a clash out */
};

enum cb_verdict {
	CB_STAND_ASIDE,     /* same machine, same port, same path: the shadow must not start */
	CB_RUN_UNPARSEABLE, /* could not read the URL: no evidence of a clash */
	CB_RUN_OTHER_HOST,  /* another machine */
	CB_RUN_OTHER_PORT,  /* not the relay's RTMP port */
	CB_RUN_OTHER_PATH,  /* the relay's port but a different path: no clash */
	CB_NEED_RESOLVE,    /* only DNS can say whether the host is this machine: resolve, ask again */
};

struct cb_decision {
	enum cb_verdict verdict;
	enum cb_host_kind host;
	bool port_matches;
	bool path_matches; /* "key matches" in the log - the key itself is never logged */
};

/* Parse an IP literal ("192.168.1.20", "::1", "fe80::1%eth0" - the zone is ignored). Strict:
 * dotted-quad IPv4 only (no "127.1", no leading zeros), so anything looser is left to the
 * system resolver, which reads it the way the RTMP output will. */
bool cb_parse_ip(const char *s, size_t len, struct cb_addr *out);

bool cb_addr_is_loopback(const struct cb_addr *a);
bool cb_addr_equal(const struct cb_addr *a, const struct cb_addr *b);

/* Take OBS's output apart: `server` is the service's server URL, `key` its stream key (NULL or ""
 * when it has none). False when the server URL cannot be read. */
bool cb_url_parse(const char *server, const char *key, struct cb_url *out);

/* The decision. `relay_port`/`relay_path` describe what the shadow output publishes to.
 * `resolved` (n_resolved entries) is consulted only when rs == CB_RESOLVE_OK. */
void cb_decide(const struct cb_url *u, bool parsed, const struct cb_local *local, enum cb_resolve rs,
	       const struct cb_addr *resolved, size_t n_resolved, int relay_port, const char *relay_path,
	       struct cb_decision *out);

const char *cb_host_kind_text(enum cb_host_kind k);
const char *cb_verdict_text(enum cb_verdict v);

#ifdef __cplusplus
}
#endif
