/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * A few portable threading primitives with NO libobs dependency (Win32 / pthreads underneath), so
 * the code that uses them (local-host.c) builds into the unit tests as well as the plugin.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CB_WAIT_FOREVER 0xffffffffUL

typedef struct cb_event cb_event;
typedef struct cb_mutex cb_mutex;
typedef struct cb_thread cb_thread;
typedef void (*cb_thread_fn)(void *arg);

/* A manual-reset event, created unsignalled. NULL on failure. */
cb_event *cb_event_create(void);
void cb_event_signal(cb_event *e);
/* True once signalled; false when timeout_ms ran out first (CB_WAIT_FOREVER: never). Timed waits run on a
 * MONOTONIC clock, so a wall-clock change neither stretches nor cuts them. */
bool cb_event_wait(cb_event *e, unsigned long timeout_ms);
void cb_event_destroy(cb_event *e);

/* A plain mutex. NULL on failure. */
cb_mutex *cb_mutex_create(void);
void cb_mutex_lock(cb_mutex *m);
void cb_mutex_unlock(cb_mutex *m);
void cb_mutex_destroy(cb_mutex *m);

/* Run fn(arg) on a new thread. NULL on failure. */
cb_thread *cb_thread_start(cb_thread_fn fn, void *arg);
/* Wait for the thread to end, then free the handle. */
void cb_thread_join(cb_thread *t);
/* Let the thread run on unsupervised and free the handle. Pin the module first (cb_pin_module)
 * if the thread may outlive it. */
void cb_thread_detach(cb_thread *t);

void cb_sleep_ms(unsigned long ms);

/* Milliseconds on a monotonic clock (only differences mean anything). */
unsigned long long cb_now_ms(void);

/* Keep the module this code lives in loaded until the process exits (GetModuleHandleExW PIN /
 * dlopen RTLD_NODELETE), so a detached thread never runs code from an unloaded module. */
bool cb_pin_module(void);

#ifdef __cplusplus
}
#endif
