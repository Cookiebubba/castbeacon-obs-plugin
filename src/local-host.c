/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * This machine's facts for the stand-aside decision, and the decision run end to end - see
 * local-host.h.
 */

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* getifaddrs, getaddrinfo */
#endif

/* Sockets first: on Windows they must come before anything that pulls in windows.h. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <unistd.h>
#endif

#include "local-host.h"
#include "cb-thread.h"

#include <stdlib.h>
#include <string.h>

static bool from_sockaddr(const struct sockaddr *sa, struct cb_addr *out)
{
	if (!sa)
		return false;
	memset(out, 0, sizeof(*out));
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *in4 = (const struct sockaddr_in *)sa;
		out->family = 4;
		memcpy(out->b, &in4->sin_addr, 4);
		return true;
	}
	if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
		static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
		const unsigned char *b = (const unsigned char *)&in6->sin6_addr;
		if (memcmp(b, mapped, sizeof(mapped)) == 0) {
			out->family = 4;
			memcpy(out->b, b + 12, 4);
		} else {
			out->family = 6;
			memcpy(out->b, b, 16);
		}
		return true;
	}
	return false;
}

/* ---------------------------------------------------------------- interface addresses */

#ifdef _WIN32

size_t cb_local_addresses(struct cb_addr **out)
{
	*out = NULL;
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
			    GAA_FLAG_SKIP_FRIENDLY_NAME;
	ULONG size = 16 * 1024;
	IP_ADAPTER_ADDRESSES *list = NULL;
	ULONG rc = ERROR_BUFFER_OVERFLOW;

	for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; attempt++) {
		free(list);
		list = malloc(size);
		if (!list)
			return 0;
		rc = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &size);
	}
	if (rc != NO_ERROR) {
		free(list);
		return 0;
	}

	size_t count = 0;
	for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
		for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
			count++;

	struct cb_addr *addrs = count ? calloc(count, sizeof(*addrs)) : NULL;
	size_t n = 0;
	if (addrs) {
		for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < count; u = u->Next)
				if (from_sockaddr(u->Address.lpSockaddr, &addrs[n]))
					n++;
	}

	free(list);
	*out = addrs;
	return n;
}

bool cb_local_hostname(char *buf, size_t size)
{
	if (!buf || size < 2)
		return false;
	DWORD len = (DWORD)size;
	if (!GetComputerNameExA(ComputerNameDnsHostname, buf, &len) || len == 0) {
		buf[0] = '\0';
		return false;
	}
	return true;
}

#else

size_t cb_local_addresses(struct cb_addr **out)
{
	*out = NULL;
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return 0;

	size_t count = 0;
	for (struct ifaddrs *i = list; i; i = i->ifa_next)
		if (i->ifa_addr)
			count++;

	struct cb_addr *addrs = count ? calloc(count, sizeof(*addrs)) : NULL;
	size_t n = 0;
	if (addrs) {
		for (struct ifaddrs *i = list; i && n < count; i = i->ifa_next)
			if (i->ifa_addr && from_sockaddr(i->ifa_addr, &addrs[n]))
				n++;
	}

	freeifaddrs(list);
	*out = addrs;
	return n;
}

bool cb_local_hostname(char *buf, size_t size)
{
	if (!buf || size < 2)
		return false;
	if (gethostname(buf, size - 1) != 0) {
		buf[0] = '\0';
		return false;
	}
	buf[size - 1] = '\0';
	return buf[0] != '\0';
}

#endif

/* ---------------------------------------------------------------- the resolver */

enum cb_resolve cb_system_resolve(const char *host, struct cb_addr **out, size_t *n_out)
{
	*out = NULL;
	*n_out = 0;

#ifdef _WIN32
	WSADATA wsa;
	bool wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	enum cb_resolve result = CB_RESOLVE_FAILED;
	struct addrinfo *res = NULL;
	if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
		size_t count = 0;
		for (struct addrinfo *a = res; a; a = a->ai_next)
			count++;
		struct cb_addr *addrs = calloc(count, sizeof(*addrs));
		size_t n = 0;
		if (addrs) {
			for (struct addrinfo *a = res; a && n < count; a = a->ai_next)
				if (from_sockaddr(a->ai_addr, &addrs[n]))
					n++;
		}
		freeaddrinfo(res);
		if (n) {
			*out = addrs;
			*n_out = n;
			result = CB_RESOLVE_OK;
		} else {
			free(addrs);
		}
	}

#ifdef _WIN32
	if (wsa_ok)
		WSACleanup();
#endif
	return result;
}

static cb_resolver_fn resolver = cb_system_resolve;

void cb_set_resolver(cb_resolver_fn fn)
{
	resolver = fn ? fn : cb_system_resolve;
}

/* ---------------------------------------------------------------- one lookup on its own thread
 *
 * The lookup belongs to whoever started it and is freed only after its thread is joined
 * (cb_lookup_finish), so an answer that arrives late still has somewhere to land. */

struct cb_lookup {
	char host[CB_HOST_MAX];
	cb_resolver_fn fn;
	cb_event *done;
	cb_thread *thread;
	enum cb_resolve result;
	struct cb_addr *addrs;
	size_t n;
};

static void lookup_main(void *arg)
{
	struct cb_lookup *l = arg;
	l->result = l->fn(l->host, &l->addrs, &l->n);
	if (l->result == CB_RESOLVE_OK && (!l->addrs || !l->n))
		l->result = CB_RESOLVE_FAILED;
	cb_event_signal(l->done);
}

struct cb_lookup *cb_lookup_start(const char *host)
{
	if (!host || !*host || strlen(host) >= CB_HOST_MAX)
		return NULL;
	struct cb_lookup *l = calloc(1, sizeof(*l));
	if (!l)
		return NULL;
	memcpy(l->host, host, strlen(host) + 1);
	l->fn = resolver;
	l->result = CB_RESOLVE_FAILED;
	l->done = cb_event_create();
	if (!l->done) {
		free(l);
		return NULL;
	}
	l->thread = cb_thread_start(lookup_main, l);
	if (!l->thread) {
		cb_event_destroy(l->done);
		free(l);
		return NULL;
	}
	return l;
}

bool cb_lookup_wait(struct cb_lookup *l, unsigned long timeout_ms)
{
	return l && cb_event_wait(l->done, timeout_ms);
}

enum cb_resolve cb_lookup_finish(struct cb_lookup *l, struct cb_addr **out, size_t *n)
{
	*out = NULL;
	*n = 0;
	if (!l)
		return CB_RESOLVE_FAILED;
	cb_thread_join(l->thread); /* returns once lookup_main has, however long that takes */
	enum cb_resolve result = l->result;
	if (result == CB_RESOLVE_OK) {
		*out = l->addrs;
		*n = l->n;
	} else {
		free(l->addrs);
	}
	cb_event_destroy(l->done);
	free(l);
	return result;
}

/* ---------------------------------------------------------------- the decision, end to end */

struct judging {
	struct cb_url u;
	bool parsed;
	struct cb_url relay;
	struct cb_local local;
	struct cb_addr *addrs; /* gathered lazily */
	char hostname[256];
	unsigned long timebox_ms;
	cb_report_fn report;
	void *ctx;
};

static void emit(struct judging *j, enum cb_phase phase, const struct cb_decision *d)
{
	char line[768];
	cb_format_decision(phase, &j->u, j->parsed, &j->relay, d, j->timebox_ms, line, sizeof(line));
	if (j->report)
		j->report(j->ctx, phase, d->verdict == CB_STAND_ASIDE, line);
}

/* Decide; when the answer is "another machine" and this machine's addresses have not been read
 * yet, read them and decide again. Loopback and 0.0.0.0/:: never need them. */
static void decide(struct judging *j, enum cb_resolve rs, const struct cb_addr *resolved, size_t n_resolved,
		   struct cb_decision *d)
{
	cb_decide(&j->u, j->parsed, &j->local, rs, resolved, n_resolved, j->relay.port, j->relay.path, d);
	bool literal_elsewhere = j->u.host_is_ip && d->host == CB_HOST_OTHER;
	bool resolved_elsewhere = rs == CB_RESOLVE_OK && d->host == CB_HOST_OTHER;
	if ((literal_elsewhere || resolved_elsewhere) && !j->addrs) {
		j->local.n_addrs = cb_local_addresses(&j->addrs);
		j->local.addrs = j->addrs;
		if (j->addrs)
			cb_decide(&j->u, j->parsed, &j->local, rs, resolved, n_resolved, j->relay.port, j->relay.path,
				  d);
	}
}

/* The host is a name only DNS can place: look it up, waiting at most the time box for a FINAL
 * answer; past it, report PROVISIONAL (another machine for now) and then the LATE answer. */
static void judge_by_lookup(struct judging *j, struct cb_decision *d)
{
	struct cb_lookup *l = cb_lookup_start(j->u.host);
	if (!l) {
		decide(j, CB_RESOLVE_FAILED, NULL, 0, d);
		emit(j, CB_PHASE_FINAL, d);
		return;
	}

	bool in_time = cb_lookup_wait(l, j->timebox_ms);
	if (!in_time) {
		decide(j, CB_RESOLVE_FAILED, NULL, 0, d);
		emit(j, CB_PHASE_PROVISIONAL, d);
	}

	struct cb_addr *resolved = NULL;
	size_t n_resolved = 0;
	enum cb_resolve rs = cb_lookup_finish(l, &resolved, &n_resolved);
	decide(j, rs, resolved, n_resolved, d);
	emit(j, in_time ? CB_PHASE_FINAL : CB_PHASE_LATE, d);
	free(resolved);
}

void cb_judge(const char *server, const char *key, const char *relay_server, const char *relay_key,
	      unsigned long timebox_ms, cb_report_fn report, void *ctx)
{
	struct judging *j = calloc(1, sizeof(*j));
	if (!j) {
		if (report)
			report(ctx, CB_PHASE_FINAL, false, "main output check: out of memory - shadow output proceeds");
		return;
	}
	j->timebox_ms = timebox_ms;
	j->report = report;
	j->ctx = ctx;

	if (!cb_url_parse(relay_server, relay_key, &j->relay)) {
		if (report)
			report(ctx, CB_PHASE_FINAL, false,
			       "main output check: the shadow's own target did not parse - shadow output proceeds");
		free(j);
		return;
	}

	j->parsed = cb_url_parse(server, key, &j->u);
	if (j->parsed && !j->u.host_is_ip && cb_local_hostname(j->hostname, sizeof(j->hostname)))
		j->local.hostname = j->hostname;

	struct cb_decision d;
	decide(j, CB_RESOLVE_NOT_TRIED, NULL, 0, &d);
	if (d.verdict == CB_NEED_RESOLVE)
		judge_by_lookup(j, &d);
	else
		emit(j, CB_PHASE_FINAL, &d);

	free(j->addrs);
	free(j);
}