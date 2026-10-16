/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * This machine's facts for the stand-aside decision (stand-aside.h): its interface addresses, its
 * hostname, and - only when the decision asks - what a name resolves to, inside a time box.
 * Results are bmalloc()ed; the caller bfree()s them.
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

/* Resolve `host` on a worker thread and wait at most timeout_ms for it. CB_RESOLVE_FAILED when the
 * name does not resolve, the time box runs out, or an earlier lookup is still stuck. */
enum cb_resolve cb_resolve_name(const char *host, unsigned timeout_ms, struct cb_addr **out, size_t *n_out);

/* Wait for a lookup still in flight, so its thread never outlives the module. Call on unload. */
void cb_resolver_shutdown(void);

#ifdef __cplusplus
}
#endif
