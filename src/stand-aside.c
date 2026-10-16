/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * The stand-aside decision - see stand-aside.h. Pure C: no libobs, no sockets, no allocation.
 */

#include "stand-aside.h"

#include <string.h>

/* ---------------------------------------------------------------- ASCII helpers (locale-free) */

static char lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = lower_ascii(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Case-insensitive compare of a counted string with a C string. */
static bool span_equals_ci(const char *s, size_t len, const char *lit)
{
	size_t n = strlen(lit);
	if (n != len)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (lower_ascii(s[i]) != lower_ascii(lit[i]))
			return false;
	}
	return true;
}

/* Hostname compare: ASCII case-insensitive, one trailing dot ignored on either side. */
static bool names_equal(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	size_t la = strlen(a), lb = strlen(b);
	if (la && a[la - 1] == '.')
		la--;
	if (lb && b[lb - 1] == '.')
		lb--;
	if (la == 0 || la != lb)
		return false;
	for (size_t i = 0; i < la; i++) {
		if (lower_ascii(a[i]) != lower_ascii(b[i]))
			return false;
	}
	return true;
}

/* ---------------------------------------------------------------- IP literals */

static bool parse_v4(const char *s, size_t len, unsigned char out[4])
{
	size_t i = 0;
	for (int part = 0; part < 4; part++) {
		if (part > 0) {
			if (i >= len || s[i] != '.')
				return false;
			i++;
		}
		size_t start = i;
		unsigned value = 0;
		while (i < len && is_digit(s[i]) && i - start < 3) {
			value = value * 10u + (unsigned)(s[i] - '0');
			i++;
		}
		size_t digits = i - start;
		if (digits == 0 || value > 255)
			return false;
		if (digits > 1 && s[start] == '0') /* "010": octal to some resolvers - not ours to read */
			return false;
		out[part] = (unsigned char)value;
	}
	return i == len;
}

static bool parse_v6(const char *s, size_t len, unsigned char out[16])
{
	unsigned groups[8] = {0};
	int ng = 0;
	int gap = -1; /* index in groups[] where "::" sits */
	size_t i = 0;

	if (len >= 2 && s[0] == ':' && s[1] == ':') {
		gap = 0;
		i = 2;
	} else if (len >= 1 && s[0] == ':') {
		return false;
	}

	while (i < len) {
		size_t j = i;
		while (j < len && s[j] != ':')
			j++;

		if (memchr(s + i, '.', j - i)) { /* an IPv4 tail: must be the last thing, worth 2 groups */
			unsigned char v4[4];
			if (j != len || ng > 6 || !parse_v4(s + i, j - i, v4))
				return false;
			groups[ng++] = ((unsigned)v4[0] << 8) | v4[1];
			groups[ng++] = ((unsigned)v4[2] << 8) | v4[3];
			i = len;
			break;
		}

		if (j == i || j - i > 4 || ng >= 8)
			return false;
		unsigned value = 0;
		for (size_t k = i; k < j; k++) {
			int h = hex_value(s[k]);
			if (h < 0)
				return false;
			value = value * 16u + (unsigned)h;
		}
		groups[ng++] = value;

		if (j == len)
			break;
		if (j + 1 < len && s[j + 1] == ':') {
			if (gap >= 0)
				return false; /* a second "::" */
			gap = ng;
			i = j + 2;
		} else {
			i = j + 1;
			if (i == len)
				return false; /* a single trailing ':' */
		}
	}

	unsigned full[8] = {0};
	if (gap < 0) {
		if (ng != 8)
			return false;
		memcpy(full, groups, sizeof(full));
	} else {
		if (ng > 7)
			return false; /* "::" must stand for at least one group */
		int tail = ng - gap;
		for (int k = 0; k < gap; k++)
			full[k] = groups[k];
		for (int k = 0; k < tail; k++)
			full[8 - tail + k] = groups[gap + k];
	}
	for (int k = 0; k < 8; k++) {
		out[2 * k] = (unsigned char)(full[k] >> 8);
		out[2 * k + 1] = (unsigned char)(full[k] & 0xff);
	}
	return true;
}

bool cb_parse_ip(const char *s, size_t len, struct cb_addr *out)
{
	if (!s || !out || len == 0)
		return false;
	memset(out, 0, sizeof(*out));

	if (memchr(s, ':', len)) {
		const char *zone = memchr(s, '%', len); /* fe80::1%eth0 - the zone never changes the host */
		if (zone)
			len = (size_t)(zone - s);
		unsigned char b[16];
		if (len == 0 || !parse_v6(s, len, b))
			return false;
		static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
		if (memcmp(b, mapped, sizeof(mapped)) == 0) {
			out->family = 4;
			memcpy(out->b, b + 12, 4);
		} else {
			out->family = 6;
			memcpy(out->b, b, 16);
		}
		return true;
	}

	if (!parse_v4(s, len, out->b))
		return false;
	out->family = 4;
	return true;
}

bool cb_addr_is_loopback(const struct cb_addr *a)
{
	if (!a)
		return false;
	if (a->family == 4)
		return a->b[0] == 127;
	if (a->family == 6) {
		for (int k = 0; k < 15; k++) {
			if (a->b[k] != 0)
				return false;
		}
		return a->b[15] == 1;
	}
	return false;
}

bool cb_addr_equal(const struct cb_addr *a, const struct cb_addr *b)
{
	if (!a || !b || a->family != b->family)
		return false;
	return memcmp(a->b, b->b, a->family == 4 ? 4 : 16) == 0;
}

static bool addr_is_local(const struct cb_addr *a, const struct cb_local *local)
{
	if (cb_addr_is_loopback(a))
		return true;
	if (!local || !local->addrs)
		return false;
	for (size_t i = 0; i < local->n_addrs; i++) {
		if (cb_addr_equal(a, &local->addrs[i]))
			return true;
	}
	return false;
}

/* ---------------------------------------------------------------- URLs */

/* The port an RTMP-family URL connects to when it names none - librtmp's rule (443 for TLS,
 * 80 for tunnelled HTTP, 1935 otherwise). 0 for a scheme we don't know. */
static int default_port(const char *scheme, size_t len)
{
	if (len == 0 || span_equals_ci(scheme, len, "rtmp") || span_equals_ci(scheme, len, "rtmpe"))
		return 1935;
	if (span_equals_ci(scheme, len, "rtmps") || span_equals_ci(scheme, len, "rtmpts") ||
	    span_equals_ci(scheme, len, "https"))
		return 443;
	if (span_equals_ci(scheme, len, "rtmpt") || span_equals_ci(scheme, len, "rtmpte") ||
	    span_equals_ci(scheme, len, "http"))
		return 80;
	return 0;
}

static bool name_char_ok(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '.' || c == '_';
}

/* Append the '/'-separated segments of s[0..len) to path, dropping empty ones. */
static bool append_segments(char *path, size_t *used, const char *s, size_t len)
{
	size_t i = 0;
	while (i < len) {
		while (i < len && s[i] == '/')
			i++;
		size_t start = i;
		while (i < len && s[i] != '/')
			i++;
		size_t seg = i - start;
		if (seg == 0)
			continue;
		size_t need = (*used ? 1 : 0) + seg;
		if (*used + need >= CB_PATH_MAX)
			return false;
		if (*used)
			path[(*used)++] = '/';
		memcpy(path + *used, s + start, seg);
		*used += seg;
		path[*used] = '\0';
	}
	return true;
}

bool cb_url_parse(const char *server, const char *key, struct cb_url *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));
	if (!server)
		return false;

	/* OBS keeps the string as typed: forgive surrounding whitespace, nothing else. */
	while (is_space(*server))
		server++;
	size_t total = strlen(server);
	while (total && is_space(server[total - 1]))
		total--;
	if (total == 0)
		return false;

	/* scheme (none = rtmp, as the old check allowed) */
	const char *p = server;
	const char *end = server + total;
	const char *scheme = server;
	size_t scheme_len = 0;
	const char *sep = NULL;
	for (const char *q = server; q + 2 < end; q++) {
		if (q[0] == ':' && q[1] == '/' && q[2] == '/') {
			sep = q;
			break;
		}
		if (q[0] == '/' || q[0] == '?' || q[0] == '#')
			break;
	}
	if (sep) {
		scheme_len = (size_t)(sep - server);
		if (scheme_len == 0)
			return false;
		for (size_t k = 0; k < scheme_len; k++) {
			char c = scheme[k];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '+' || c == '-' ||
			      c == '.'))
				return false;
		}
		p = sep + 3;
	}

	/* authority: up to the path, query or fragment */
	const char *auth = p;
	while (p < end && *p != '/' && *p != '?' && *p != '#')
		p++;
	const char *auth_end = p;

	/* credentials never take part (and are never logged) */
	for (const char *q = auth_end; q > auth; q--) {
		if (q[-1] == '@') {
			auth = q;
			break;
		}
	}
	if (auth == auth_end)
		return false;

	const char *host = auth;
	size_t host_len;
	const char *port_str = NULL;
	size_t port_len = 0;

	if (*host == '[') { /* [v6]:port */
		const char *close = memchr(host, ']', (size_t)(auth_end - host));
		if (!close)
			return false;
		host++;
		host_len = (size_t)(close - host);
		const char *after = close + 1;
		if (after < auth_end) {
			if (*after != ':')
				return false;
			port_str = after + 1;
			port_len = (size_t)(auth_end - port_str);
		}
		/* a bracket holds an IPv6 literal and nothing else (an IPv4-mapped one is stored as v4) */
		if (!memchr(host, ':', host_len) || !cb_parse_ip(host, host_len, &out->ip))
			return false;
		out->host_is_ip = true;
	} else {
		const char *colon = memchr(host, ':', (size_t)(auth_end - host));
		if (colon && memchr(colon + 1, ':', (size_t)(auth_end - colon - 1))) {
			/* two colons and no brackets: only a bare IPv6 literal (no port) reads sensibly */
			host_len = (size_t)(auth_end - host);
			if (!cb_parse_ip(host, host_len, &out->ip))
				return false;
			out->host_is_ip = true;
		} else {
			host_len = colon ? (size_t)(colon - host) : (size_t)(auth_end - host);
			if (colon) {
				port_str = colon + 1;
				port_len = (size_t)(auth_end - port_str);
			}
			if (host_len == 0)
				return false;
			if (cb_parse_ip(host, host_len, &out->ip)) {
				out->host_is_ip = true;
			} else {
				for (size_t k = 0; k < host_len; k++) {
					if (!name_char_ok(host[k]))
						return false;
				}
				if (host_len > 1 && host[host_len - 1] == '.')
					host_len--; /* "pc.lan." and "pc.lan" are one name */
				if (host[0] == '.' || host[0] == '-')
					return false;
			}
		}
	}

	/* keep the host for the log line: without brackets, credentials or zone */
	size_t keep = host_len;
	const char *zone = out->host_is_ip ? memchr(host, '%', host_len) : NULL;
	if (zone)
		keep = (size_t)(zone - host);
	if (keep == 0 || keep >= CB_HOST_MAX)
		return false;
	memcpy(out->host, host, keep);
	out->host[keep] = '\0';

	/* port */
	if (port_str && port_len) {
		long value = 0;
		if (port_len > 5)
			return false;
		for (size_t k = 0; k < port_len; k++) {
			if (!is_digit(port_str[k]))
				return false;
			value = value * 10 + (port_str[k] - '0');
		}
		if (value < 1 || value > 65535)
			return false;
		out->port = (int)value;
	} else {
		out->port = default_port(scheme, scheme_len);
	}

	/* the relay path: URL path + "/" + key, both without a query */
	size_t used = 0;
	out->path[0] = '\0';
	const char *path = auth_end;
	const char *path_end = path;
	while (path_end < end && *path_end != '?' && *path_end != '#')
		path_end++;
	if (!append_segments(out->path, &used, path, (size_t)(path_end - path)))
		return false;
	if (key && *key) {
		size_t key_len = strcspn(key, "?#");
		while (key_len && is_space(key[key_len - 1]))
			key_len--;
		const char *k = key;
		while (key_len && is_space(*k)) {
			k++;
			key_len--;
		}
		if (!append_segments(out->path, &used, k, key_len))
			return false;
	}
	return true;
}

/* ---------------------------------------------------------------- the decision */

static bool is_localhost_name(const char *host)
{
	/* RFC 6761: "localhost" and every name under it are the loopback. */
	size_t n = strlen(host);
	if (span_equals_ci(host, n, "localhost"))
		return true;
	return n > 10 && span_equals_ci(host + n - 10, 10, ".localhost");
}

void cb_decide(const struct cb_url *u, bool parsed, const struct cb_local *local, enum cb_resolve rs,
	       const struct cb_addr *resolved, size_t n_resolved, int relay_port, const char *relay_path,
	       struct cb_decision *out)
{
	if (!out)
		return;
	out->verdict = CB_RUN_UNPARSEABLE;
	out->host = CB_HOST_UNPARSEABLE;
	out->port_matches = false;
	out->path_matches = false;
	if (!parsed || !u)
		return;

	out->port_matches = u->port != 0 && u->port == relay_port;
	out->path_matches = relay_path != NULL && strcmp(u->path, relay_path) == 0;

	if (u->host_is_ip) {
		if (cb_addr_is_loopback(&u->ip))
			out->host = CB_HOST_LOOPBACK;
		else if (addr_is_local(&u->ip, local))
			out->host = CB_HOST_LOCAL_ADDR;
		else
			out->host = CB_HOST_OTHER;
	} else if (is_localhost_name(u->host)) {
		out->host = CB_HOST_LOOPBACK;
	} else if (local && local->hostname && names_equal(u->host, local->hostname)) {
		out->host = CB_HOST_HOSTNAME;
	} else if (rs == CB_RESOLVE_OK && resolved && n_resolved > 0) {
		out->host = CB_HOST_OTHER;
		for (size_t i = 0; i < n_resolved; i++) {
			if (addr_is_local(&resolved[i], local)) {
				out->host = CB_HOST_RESOLVES_LOCAL;
				break;
			}
		}
	} else if (rs != CB_RESOLVE_NOT_TRIED) {
		out->host = CB_HOST_UNRESOLVED;
	} else {
		out->host = CB_HOST_NOT_CHECKED;
		if (out->port_matches) {
			/* the only fact left that decides it is where the name points */
			out->verdict = CB_NEED_RESOLVE;
			return;
		}
	}

	if (out->host == CB_HOST_OTHER || out->host == CB_HOST_UNRESOLVED)
		out->verdict = CB_RUN_OTHER_HOST;
	else if (!out->port_matches)
		out->verdict = CB_RUN_OTHER_PORT;
	else /* this PC's relay, ANY path: the operator chose "OBS sends straight to Cast Beacon" */
		out->verdict = CB_STAND_ASIDE;
}

const char *cb_host_kind_text(enum cb_host_kind k)
{
	switch (k) {
	case CB_HOST_UNPARSEABLE:
		return "unreadable";
	case CB_HOST_LOOPBACK:
		return "this PC (loopback)";
	case CB_HOST_LOCAL_ADDR:
		return "this PC (one of its network addresses)";
	case CB_HOST_HOSTNAME:
		return "this PC (its hostname)";
	case CB_HOST_RESOLVES_LOCAL:
		return "this PC (the name resolves to one of its addresses)";
	case CB_HOST_OTHER:
		return "another machine";
	case CB_HOST_UNRESOLVED:
		return "a name that did not resolve (treated as another machine)";
	case CB_HOST_NOT_CHECKED:
		return "not looked up";
	}
	return "?";
}

const char *cb_verdict_text(enum cb_verdict v)
{
	switch (v) {
	case CB_STAND_ASIDE:
		return "standing aside";
	case CB_RUN_UNPARSEABLE:
		return "could not read OBS's server URL - no clash assumed";
	case CB_RUN_OTHER_HOST:
		return "OBS sends to another machine - no clash";
	case CB_RUN_OTHER_PORT:
		return "OBS sends to another port - no clash";
	case CB_NEED_RESOLVE:
		return "host needs a lookup";
	}
	return "?";
}
