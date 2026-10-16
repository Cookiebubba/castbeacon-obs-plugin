/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * This machine's facts for the stand-aside decision - see local-host.h.
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

#include <util/bmem.h>
#include <util/threading.h>

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
		bfree(list);
		list = bmalloc(size);
		rc = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, list, &size);
	}
	if (rc != NO_ERROR) {
		bfree(list);
		return 0;
	}

	size_t count = 0;
	for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
		for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
			count++;

	struct cb_addr *addrs = count ? bzalloc(count * sizeof(*addrs)) : NULL;
	size_t n = 0;
	for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
		for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < count; u = u->Next)
			if (from_sockaddr(u->Address.lpSockaddr, &addrs[n]))
				n++;

	bfree(list);
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

	struct cb_addr *addrs = count ? bzalloc(count * sizeof(*addrs)) : NULL;
	size_t n = 0;
	for (struct ifaddrs *i = list; i && n < count; i = i->ifa_next)
		if (i->ifa_addr && from_sockaddr(i->ifa_addr, &addrs[n]))
			n++;

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

/* ---------------------------------------------------------------- time-boxed name lookup
 *
 * getaddrinfo has no timeout, so it runs on a worker thread and the caller (OBS's UI thread, in a
 * frontend event) waits at most timeout_ms. The job belongs to the CALLER side and is freed only
 * after its thread is joined, so a lookup that overruns simply stays "live" until it finishes.
 * Only ONE lookup is ever in flight: while a stuck one is still running, a new request is answered
 * "failed" at once instead of stacking threads, and unload joins the thread so it never runs code
 * from an unloaded module. Every entry point runs on OBS's UI thread, so the statics need no lock. */

struct resolve_job {
	char host[CB_HOST_MAX];
	os_event_t *done;
	enum cb_resolve result;
	struct cb_addr *addrs;
	size_t n;
	pthread_t thread;
};

static struct resolve_job *live_job = NULL; /* a lookup whose thread has not been joined yet */

static void *resolve_main(void *arg)
{
	struct resolve_job *job = arg;
	os_set_thread_name("castbeacon: resolve");

#ifdef _WIN32
	WSADATA wsa;
	bool wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	job->result = CB_RESOLVE_FAILED;
	if (getaddrinfo(job->host, NULL, &hints, &res) == 0 && res) {
		size_t count = 0;
		for (struct addrinfo *a = res; a; a = a->ai_next)
			count++;
		job->addrs = bzalloc(count * sizeof(*job->addrs));
		for (struct addrinfo *a = res; a && job->n < count; a = a->ai_next)
			if (from_sockaddr(a->ai_addr, &job->addrs[job->n]))
				job->n++;
		freeaddrinfo(res);
		if (job->n)
			job->result = CB_RESOLVE_OK;
	}

#ifdef _WIN32
	if (wsa_ok)
		WSACleanup();
#endif

	os_event_signal(job->done);
	return NULL;
}

static void job_join_and_free(struct resolve_job *job)
{
	pthread_join(job->thread, NULL);
	os_event_destroy(job->done);
	bfree(job->addrs);
	bfree(job);
}

enum cb_resolve cb_resolve_name(const char *host, unsigned timeout_ms, struct cb_addr **out, size_t *n_out)
{
	*out = NULL;
	*n_out = 0;
	if (!host || !*host || strlen(host) >= CB_HOST_MAX)
		return CB_RESOLVE_FAILED;

	if (live_job) {
		if (os_event_try(live_job->done) != 0)
			return CB_RESOLVE_FAILED; /* the last lookup is still stuck: do not stack another */
		job_join_and_free(live_job);
		live_job = NULL;
	}

	struct resolve_job *job = bzalloc(sizeof(*job));
	memcpy(job->host, host, strlen(host) + 1);
	if (os_event_init(&job->done, OS_EVENT_TYPE_MANUAL) != 0) {
		bfree(job);
		return CB_RESOLVE_FAILED;
	}
	if (pthread_create(&job->thread, NULL, resolve_main, job) != 0) {
		os_event_destroy(job->done);
		bfree(job);
		return CB_RESOLVE_FAILED;
	}

	if (os_event_timedwait(job->done, timeout_ms) != 0) {
		live_job = job; /* overran the time box: treated as unresolved, joined later */
		return CB_RESOLVE_FAILED;
	}

	enum cb_resolve result = CB_RESOLVE_FAILED;
	if (job->result == CB_RESOLVE_OK && job->n) {
		*out = bmemdup(job->addrs, job->n * sizeof(*job->addrs));
		*n_out = job->n;
		result = CB_RESOLVE_OK;
	}
	job_join_and_free(job);
	return result;
}

void cb_resolver_shutdown(void)
{
	if (live_job) {
		job_join_and_free(live_job);
		live_job = NULL;
	}
}