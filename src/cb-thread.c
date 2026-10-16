/*
 * Cast Beacon OBS Plugin (GPLv2).
 *
 * Portable threading primitives - see cb-thread.h.
 */

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dladdr, RTLD_NOLOAD, RTLD_NODELETE */
#endif

#include "cb-thread.h"

#include <stdlib.h>

/* Any address inside this module identifies it to GetModuleHandleExW / dladdr. */
static const char module_anchor = 0;

/* The thread gets its own copy of fn/arg, so the handle can be freed (detach) before it starts. */
struct start_block {
	cb_thread_fn fn;
	void *arg;
};

#ifdef _WIN32

#include <windows.h>

struct cb_event {
	HANDLE h;
};

struct cb_thread {
	HANDLE h;
};

cb_event *cb_event_create(void)
{
	cb_event *e = calloc(1, sizeof(*e));
	if (!e)
		return NULL;
	e->h = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!e->h) {
		free(e);
		return NULL;
	}
	return e;
}

void cb_event_signal(cb_event *e)
{
	if (e)
		SetEvent(e->h);
}

bool cb_event_wait(cb_event *e, unsigned long timeout_ms)
{
	if (!e)
		return false;
	DWORD ms = timeout_ms == CB_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms;
	return WaitForSingleObject(e->h, ms) == WAIT_OBJECT_0;
}

void cb_event_destroy(cb_event *e)
{
	if (e) {
		CloseHandle(e->h);
		free(e);
	}
}

struct cb_mutex {
	CRITICAL_SECTION cs;
};

cb_mutex *cb_mutex_create(void)
{
	cb_mutex *m = calloc(1, sizeof(*m));
	if (m)
		InitializeCriticalSection(&m->cs);
	return m;
}

void cb_mutex_lock(cb_mutex *m)
{
	EnterCriticalSection(&m->cs);
}

void cb_mutex_unlock(cb_mutex *m)
{
	LeaveCriticalSection(&m->cs);
}

void cb_mutex_destroy(cb_mutex *m)
{
	if (m) {
		DeleteCriticalSection(&m->cs);
		free(m);
	}
}

static DWORD WINAPI trampoline(LPVOID p)
{
	struct start_block s = *(struct start_block *)p;
	free(p);
	s.fn(s.arg);
	return 0;
}

cb_thread *cb_thread_start(cb_thread_fn fn, void *arg)
{
	cb_thread *t = calloc(1, sizeof(*t));
	struct start_block *s = malloc(sizeof(*s));
	if (!t || !s) {
		free(t);
		free(s);
		return NULL;
	}
	s->fn = fn;
	s->arg = arg;
	t->h = CreateThread(NULL, 0, trampoline, s, 0, NULL);
	if (!t->h) {
		free(s);
		free(t);
		return NULL;
	}
	return t;
}

void cb_thread_join(cb_thread *t)
{
	if (!t)
		return;
	WaitForSingleObject(t->h, INFINITE);
	CloseHandle(t->h);
	free(t);
}

void cb_thread_detach(cb_thread *t)
{
	if (!t)
		return;
	CloseHandle(t->h);
	free(t);
}

void cb_sleep_ms(unsigned long ms)
{
	Sleep((DWORD)ms);
}

unsigned long long cb_now_ms(void)
{
	return (unsigned long long)GetTickCount64();
}

bool cb_pin_module(void)
{
	HMODULE m = NULL;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				  (LPCWSTR)&module_anchor, &m) != 0;
}

#else

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

struct cb_event {
	pthread_mutex_t m;
	pthread_cond_t c;
	bool set;
};

struct cb_thread {
	pthread_t t;
};

cb_event *cb_event_create(void)
{
	cb_event *e = calloc(1, sizeof(*e));
	if (!e)
		return NULL;
	if (pthread_mutex_init(&e->m, NULL) != 0) {
		free(e);
		return NULL;
	}
#ifdef __APPLE__
	/* macOS has no pthread_condattr_setclock: cb_event_wait uses relative waits there instead */
	int rc = pthread_cond_init(&e->c, NULL);
#else
	pthread_condattr_t attr;
	int rc = pthread_condattr_init(&attr);
	if (rc == 0) {
		pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		rc = pthread_cond_init(&e->c, &attr);
		pthread_condattr_destroy(&attr);
	}
#endif
	if (rc != 0) {
		pthread_mutex_destroy(&e->m);
		free(e);
		return NULL;
	}
	return e;
}

void cb_event_signal(cb_event *e)
{
	if (!e)
		return;
	pthread_mutex_lock(&e->m);
	e->set = true;
	pthread_cond_broadcast(&e->c);
	pthread_mutex_unlock(&e->m);
}

bool cb_event_wait(cb_event *e, unsigned long timeout_ms)
{
	if (!e)
		return false;
	pthread_mutex_lock(&e->m);
	if (timeout_ms == CB_WAIT_FOREVER) {
		while (!e->set)
			pthread_cond_wait(&e->c, &e->m);
	} else {
		struct timespec until;
		clock_gettime(CLOCK_MONOTONIC, &until);
		until.tv_sec += (time_t)(timeout_ms / 1000);
		until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		while (!e->set) {
#ifdef __APPLE__
			struct timespec now, left;
			clock_gettime(CLOCK_MONOTONIC, &now);
			left.tv_sec = until.tv_sec - now.tv_sec;
			left.tv_nsec = until.tv_nsec - now.tv_nsec;
			if (left.tv_nsec < 0) {
				left.tv_sec--;
				left.tv_nsec += 1000000000L;
			}
			if (left.tv_sec < 0)
				break;
			pthread_cond_timedwait_relative_np(&e->c, &e->m, &left);
#else
			if (pthread_cond_timedwait(&e->c, &e->m, &until) == ETIMEDOUT)
				break;
#endif
		}
	}
	bool set = e->set;
	pthread_mutex_unlock(&e->m);
	return set;
}

void cb_event_destroy(cb_event *e)
{
	if (!e)
		return;
	pthread_cond_destroy(&e->c);
	pthread_mutex_destroy(&e->m);
	free(e);
}

struct cb_mutex {
	pthread_mutex_t m;
};

cb_mutex *cb_mutex_create(void)
{
	cb_mutex *m = calloc(1, sizeof(*m));
	if (m && pthread_mutex_init(&m->m, NULL) != 0) {
		free(m);
		return NULL;
	}
	return m;
}

void cb_mutex_lock(cb_mutex *m)
{
	pthread_mutex_lock(&m->m);
}

void cb_mutex_unlock(cb_mutex *m)
{
	pthread_mutex_unlock(&m->m);
}

void cb_mutex_destroy(cb_mutex *m)
{
	if (m) {
		pthread_mutex_destroy(&m->m);
		free(m);
	}
}

static void *trampoline(void *p)
{
	struct start_block s = *(struct start_block *)p;
	free(p);
	s.fn(s.arg);
	return NULL;
}

cb_thread *cb_thread_start(cb_thread_fn fn, void *arg)
{
	cb_thread *t = calloc(1, sizeof(*t));
	struct start_block *s = malloc(sizeof(*s));
	if (!t || !s) {
		free(t);
		free(s);
		return NULL;
	}
	s->fn = fn;
	s->arg = arg;
	if (pthread_create(&t->t, NULL, trampoline, s) != 0) {
		free(s);
		free(t);
		return NULL;
	}
	return t;
}

void cb_thread_join(cb_thread *t)
{
	if (!t)
		return;
	pthread_join(t->t, NULL);
	free(t);
}

void cb_thread_detach(cb_thread *t)
{
	if (!t)
		return;
	pthread_detach(t->t);
	free(t);
}

void cb_sleep_ms(unsigned long ms)
{
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000);
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

unsigned long long cb_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000L);
}

bool cb_pin_module(void)
{
	Dl_info info;
	if (!dladdr(&module_anchor, &info) || !info.dli_fname)
		return false;
	/* NOLOAD: only take another reference on the module that is already loaded; NODELETE: and
	 * never unload it. The handle is deliberately never closed. */
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != NULL;
}

#endif
