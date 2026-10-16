/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * This machine's facts for the stand-aside decision (stand-aside.h) - its interface addresses, its
 * hostname, a host-name lookup that runs on its own thread - and cb_judge(), which runs the whole
 * decision. No libobs in here (cb-thread.h underneath), so the unit tests build it too.
 * Everything that returns memory returns malloc()ed memory; free it with free().
 */

#pragma once

#include "stand-aside.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every unicast address on every interface (IPv4 and IPv6, loopback included). Returns the count
 * and sets *out (NULL when there are none). */
size_t cb_local_addresses(struct cb_addr **out);

/* This machine's hostname. False when it cannot be read. */
bool cb_local_hostname(char *buf, size_t size);

/* How a host name is resolved. The default is the system's getaddrinfo; tests swap in a fake
 * (a delay, a failure). Must set *out (malloc()ed) and *n when it returns CB_RESOLVE_OK. */
typedef enum cb_resolve (*cb_resolver_fn)(const char *host, struct cb_addr **out, size_t *n);
void cb_set_resolver(cb_resolver_fn fn); /* NULL = the system resolver */
enum cb_resolve cb_system_resolve(const char *host, struct cb_addr **out, size_t *n);

/* A lookup running on its own thread. */
struct cb_lookup;
struct cb_lookup *cb_lookup_start(const char *host); /* NULL when it could not start */
/* True once the answer is in; false when timeout_ms ran out first (the lookup keeps going). */
bool cb_lookup_wait(struct cb_lookup *l, unsigned long timeout_ms);
/* Wait for the answer however long it takes, join the thread, free the lookup, return the answer. */
enum cb_resolve cb_lookup_finish(struct cb_lookup *l, struct cb_addr **out, size_t *n);

/* Reports one answer: FINAL, or PROVISIONAL (always stand_aside) then LATE when a lookup overran
 * timebox_ms. `line` is the log line (cb_format_decision) - it never holds the stream key. */
typedef void (*cb_report_fn)(void *ctx, enum cb_phase phase, bool stand_aside, const char *line);

/* cb_judge's signature, so a caller can be handed a different judge (tests). */
typedef void (*cb_judge_fn)(const char *server, const char *key, const char *relay_server, const char *relay_key,
			    unsigned long timebox_ms, cb_report_fn report, void *ctx);

/* The whole decision for OBS's output (server + key) against the shadow's target (relay_server +
 * relay_key). BLOCKS - possibly for as long as the system resolver takes - so it belongs on a
 * worker thread. Local addresses are gathered only when the host is an IP literal, or after a
 * lookup; the hostname only for a name. */
void cb_judge(const char *server, const char *key, const char *relay_server, const char *relay_key,
	      unsigned long timebox_ms, cb_report_fn report, void *ctx);

#ifdef __cplusplus
}
#endif
