/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdarg.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
#include <evenless/atomic.h>
#include <evenless/evl.h>
#include <evenless/monitor.h>
#include <evenless/mutex.h>
#include <evenless/event.h>
#include <evenless/thread.h>
#include <evenless/syscall.h>
#include <linux/types.h>
#include <uapi/evenless/factory.h>
#include <uapi/evenless/mutex.h>
#include "internal.h"

static int init_monitor_vargs(struct evl_monitor *mon,
			int type, int clockfd, unsigned int ceiling,
			const char *fmt, va_list ap)
{
	struct evl_monitor_attrs attrs;
	struct evl_element_ids eids;
	int efd, ret;
	char *name;

	if (evl_shared_memory == NULL)
		return -ENXIO;

	/*
	 * We align on the in-band SCHED_FIFO priority range. Although
	 * the core does not require this, a 1:1 mapping between
	 * in-band and out-of-band priorities is simpler to deal with
	 * for users with respect to inband <-> out-of-band mode
	 * switches.
	 */
	if (type == EVL_MONITOR_PP) {
		ret = sched_get_priority_max(SCHED_FIFO);
		if (ret < 0 || ceiling > ret)
			return -EINVAL;
	}

	ret = vasprintf(&name, fmt, ap);
	if (ret < 0)
		return -ENOMEM;

	attrs.type = type;
	attrs.clockfd = clockfd;
	attrs.initval = ceiling;
	efd = create_evl_element("monitor", name, &attrs, &eids);
	free(name);
	if (efd < 0)
		return efd;

	mon->active.state = evl_shared_memory + eids.state_offset;
	mon->active.fundle = eids.fundle;
	mon->active.type = type;
	mon->active.efd = efd;
	mon->magic = __MONITOR_ACTIVE_MAGIC;

	return 0;
}

static int init_monitor(struct evl_monitor *mon,
			int type, int clockfd, unsigned int ceiling,
			const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = init_monitor_vargs(mon, type, clockfd, ceiling, fmt, ap);
	va_end(ap);

	return ret;
}

static int open_monitor(struct evl_monitor *mon, const char *fmt, ...)
{
	struct evl_monitor_binding bind;
	int ret, efd;
	va_list ap;

	va_start(ap, fmt);
	efd = open_evl_element_vargs("monitor", fmt, ap);
	va_end(ap);
	if (efd < 0)
		return efd;

	ret = ioctl(efd, EVL_MONIOC_BIND, &bind);
	if (ret)
		return -errno;

	mon->active.state = evl_shared_memory + bind.eids.state_offset;
	mon->active.fundle = bind.eids.fundle;
	mon->active.type = bind.type;
	mon->active.efd = efd;
	mon->magic = __MONITOR_ACTIVE_MAGIC;

	return 0;
}

static int close_monitor(struct evl_monitor *mon)
{
	int efd;

	if (mon->magic != __MONITOR_ACTIVE_MAGIC)
		return -EINVAL;

	efd = mon->active.efd;
	mon->active.efd = -1;
	compiler_barrier();
	close(efd);

	mon->active.fundle = EVL_NO_HANDLE;
	mon->active.state = NULL;
	mon->magic = __MONITOR_DEAD_MAGIC;

	return 0;
}

int evl_new_mutex(struct evl_mutex *mutex, int clockfd,
		const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = init_monitor_vargs(&mutex->__mutex, EVL_MONITOR_PI,
				clockfd, 0, fmt, ap);
	va_end(ap);

	return ret;
}

int evl_new_mutex_ceiling(struct evl_mutex *mutex, int clockfd,
			unsigned int ceiling,
			const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = init_monitor_vargs(&mutex->__mutex, EVL_MONITOR_PP,
				clockfd, ceiling, fmt, ap);
	va_end(ap);

	return ret;
}

int evl_open_mutex(struct evl_mutex *mutex, const char *fmt, ...)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = open_monitor(_mutex, fmt, ap);
	va_end(ap);

	if (efd < 0)
		return efd;

	if (_mutex->active.type == EVL_MONITOR_EV)
		return -EINVAL;

	return efd;
}

int evl_close_mutex(struct evl_mutex *mutex)
{
	struct evl_monitor *_mutex = &mutex->__mutex;

	if (_mutex->active.type != EVL_MONITOR_PI &&
		_mutex->active.type != EVL_MONITOR_PP)
		return -EINVAL;

	return close_monitor(_mutex);
}

static int try_lock(struct evl_mutex *mutex)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	struct evl_user_window *u_window;
	struct evl_monitor_state *gst;
	bool protect = false;
	fundle_t current;
	int mode, ret;

	current = evl_get_current();
	if (current == EVL_NO_HANDLE)
		return -EPERM;

	if (_mutex->magic == __MONITOR_UNINIT_MAGIC &&
		_mutex->uninit.type != EVL_MONITOR_EV) {
		ret = init_monitor(_mutex,
				_mutex->uninit.type,
				_mutex->uninit.clockfd,
				_mutex->uninit.ceiling,
				_mutex->uninit.name);
		if (ret)
			return ret;
	} else if (_mutex->magic != __MONITOR_ACTIVE_MAGIC)
		return -EINVAL;

	gst = _mutex->active.state;

	/*
	 * Threads running in-band and/or enabling some debug features
	 * must go through the slow syscall path.
	 */
	mode = evl_get_current_mode();
	if (!(mode & (T_INBAND|T_WEAK|T_DEBUG))) {
		if (_mutex->active.type == EVL_MONITOR_PP) {
			u_window = evl_get_current_window();
			/*
			 * Can't nest lazy ceiling requests, have to
			 * take the slow path when this happens.
			 */
			if (u_window->pp_pending != EVL_NO_HANDLE)
				goto slow_path;
			u_window->pp_pending = _mutex->active.fundle;
			protect = true;
		}
		ret = evl_fast_lock_mutex(&gst->u.gate.owner, current);
		if (ret == 0) {
			gst->flags &= ~EVL_MONITOR_SIGNALED;
			return 0;
		}
	} else {
	slow_path:
		ret = 0;
		if (evl_is_mutex_owner(&gst->u.gate.owner, current))
			ret = -EBUSY;
	}

	if (ret == -EBUSY) {
		if (protect)
			u_window->pp_pending = EVL_NO_HANDLE;

		return -EDEADLK;
	}

	return -ENODATA;
}

int evl_timedlock(struct evl_mutex *mutex,
		const struct timespec *timeout)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	struct evl_monitor_lockreq lreq;
	int ret, cancel_type;

	ret = try_lock(mutex);
	if (ret != -ENODATA)
		return ret;

	lreq.timeout = *timeout;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &cancel_type);

	do
		ret = oob_ioctl(_mutex->active.efd, EVL_MONIOC_ENTER, &lreq);
	while (ret && errno == EINTR);

	pthread_setcanceltype(cancel_type, NULL);

	return ret ? -errno : 0;
}

int evl_lock(struct evl_mutex *mutex)
{
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };

	return evl_timedlock(mutex, &timeout);
}

int evl_trylock(struct evl_mutex *mutex)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	int ret;

	ret = try_lock(mutex);
	if (ret != -ENODATA)
		return ret;

	do
		ret = oob_ioctl(_mutex->active.efd, EVL_MONIOC_TRYENTER);
	while (ret && errno == EINTR);

	return ret ? -errno : 0;
}

int evl_unlock(struct evl_mutex *mutex)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	struct evl_user_window *u_window;
	struct evl_monitor_state *gst;
	fundle_t current;
	int ret, mode;

	if (_mutex->magic != __MONITOR_ACTIVE_MAGIC)
		return -EINVAL;

	gst = _mutex->active.state;
	current = evl_get_current();
	if (!evl_is_mutex_owner(&gst->u.gate.owner, current))
		return -EPERM;

	/* Do we have waiters on a signaled event we are gating? */
	if (gst->flags & EVL_MONITOR_SIGNALED)
		goto slow_path;

	mode = evl_get_current_mode();
	if (mode & (T_WEAK|T_DEBUG))
		goto slow_path;

	if (evl_fast_unlock_mutex(&gst->u.gate.owner, current)) {
		if (_mutex->active.type == EVL_MONITOR_PP) {
			u_window = evl_get_current_window();
			u_window->pp_pending = EVL_NO_HANDLE;
		}
		return 0;
	}

	/*
	 * If the fast release failed, somebody else must be waiting
	 * for entering the lock or PP was committed for the current
	 * thread. Need to ask the kernel for proper release.
	 */
slow_path:
	ret = oob_ioctl(_mutex->active.efd, EVL_MONIOC_EXIT);

	return ret ? -errno : 0;
}

int evl_set_mutex_ceiling(struct evl_mutex *mutex,
			unsigned int ceiling)
{
	struct evl_monitor *_mutex = &mutex->__mutex;
	int ret;

	if (ceiling == 0)
		return -EINVAL;

	ret = sched_get_priority_max(SCHED_FIFO);
	if (ret < 0 || ceiling > ret)
		return -EINVAL;

	if (_mutex->magic == __MONITOR_UNINIT_MAGIC) {
		if (_mutex->uninit.type != EVL_MONITOR_PP)
			return -EINVAL;
		_mutex->uninit.ceiling = ceiling;
		return 0;
	}

	if (_mutex->magic != __MONITOR_ACTIVE_MAGIC ||
		_mutex->active.type != EVL_MONITOR_PP)
		return -EINVAL;

	_mutex->active.state->u.gate.ceiling = ceiling;

	return 0;
}

int evl_get_mutex_ceiling(struct evl_mutex *mutex)
{
	struct evl_monitor *_mutex = &mutex->__mutex;

	if (_mutex->magic == __MONITOR_UNINIT_MAGIC) {
		if (_mutex->uninit.type != EVL_MONITOR_PP)
			return -EINVAL;

		return _mutex->uninit.ceiling;
	}

	if (_mutex->magic != __MONITOR_ACTIVE_MAGIC ||
		_mutex->active.type != EVL_MONITOR_PP)
		return -EINVAL;

	return _mutex->active.state->u.gate.ceiling;
}

int evl_new_event(struct evl_event *event,
		int clockfd, const char *fmt, ...)
{
	struct evl_monitor *_event = &event->__event;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = init_monitor_vargs(_event, EVL_MONITOR_EV,
				clockfd, 0, fmt, ap);
	va_end(ap);

	return ret;
}

int evl_open_event(struct evl_event *event, const char *fmt, ...)
{
	struct evl_monitor *_event = &event->__event;
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = open_monitor(_event, fmt, ap);
	va_end(ap);

	if (efd < 0)
		return efd;

	if (_event->active.type != EVL_MONITOR_EV)
		return -EINVAL;

	return efd;
}

int evl_close_event(struct evl_event *event)
{
	struct evl_monitor *_event = &event->__event;

	if (_event->active.type != EVL_MONITOR_EV)
		return -EINVAL;

	return close_monitor(_event);
}

static int check_event_sanity(struct evl_event *event)
{
	struct evl_monitor *_event = &event->__event;
	int ret;

	if (_event->magic == __MONITOR_UNINIT_MAGIC &&
		_event->uninit.type == EVL_MONITOR_EV) {
		ret = init_monitor(_event, EVL_MONITOR_EV,
				_event->uninit.clockfd, 0,
				_event->uninit.name);
		if (ret)
			return ret;
	} else if (_event->magic != __MONITOR_ACTIVE_MAGIC)
		return -EINVAL;

	if (_event->active.type != EVL_MONITOR_EV)
		return -EINVAL;

	return 0;
}

static struct evl_monitor_state *
get_lock_state(struct evl_event *event)
{
	struct evl_monitor_state *est = event->__event.active.state;

	if (est->u.event.gate_offset == EVL_MONITOR_NOGATE)
		return NULL;	/* Nobody waits on @event */

	return evl_shared_memory + est->u.event.gate_offset;
}

struct unwait_data {
	struct evl_monitor_unwaitreq ureq;
	int efd;
};

static void unwait_monitor(void *data)
{
	struct unwait_data *unwait = data;
	int ret;

	do
		ret = oob_ioctl(unwait->efd, EVL_MONIOC_UNWAIT,
				&unwait->ureq);
	while (ret && errno == EINTR);
}

int evl_timedwait(struct evl_event *event,
		struct evl_mutex *mutex,
		const struct timespec *timeout)
{
	struct evl_monitor *_event = &event->__event;
	struct evl_monitor_waitreq req;
	struct unwait_data unwait;
	int ret, old_type;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	req.gatefd = mutex->__mutex.active.efd;
	req.timeout = *timeout;
	req.status = -EINVAL;
	unwait.ureq.gatefd = req.gatefd;
	unwait.efd = _event->active.efd;

	pthread_cleanup_push(unwait_monitor, &unwait);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type);
	ret = oob_ioctl(_event->active.efd, EVL_MONIOC_WAIT, &req);
	pthread_setcanceltype(old_type, NULL);
	pthread_cleanup_pop(0);

	/*
	 * If oob_ioctl() failed with EINTR, we got forcibly unblocked
	 * while waiting for the event or trying to reacquire the lock
	 * afterwards, in which case the mutex was left
	 * unlocked. Issue UNWAIT to recover from that situation.
	 */
	if (ret && errno == EINTR) {
		unwait_monitor(&unwait);
		pthread_testcancel();
	}

	return ret ? -errno : req.status;
}

int evl_wait(struct evl_event *event, struct evl_mutex *mutex)
{
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };

	return evl_timedwait(event, mutex, &timeout);
}

int evl_signal(struct evl_event *event)
{
	struct evl_monitor_state *est, *gst;
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags |= EVL_MONITOR_SIGNALED;
		est = event->__event.active.state;
		est->flags |= EVL_MONITOR_SIGNALED;
	}

	return 0;
}

int evl_signal_thread(struct evl_event *event, int thrfd)
{
	struct evl_monitor_state *gst;
	__u32 efd;
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags |= EVL_MONITOR_SIGNALED;
		efd = event->__event.active.efd;
	}

	return oob_ioctl(thrfd, EVL_THRIOC_SIGNAL, &efd) ? -errno : 0;
}

int evl_broadcast(struct evl_event *event)
{
	struct evl_monitor_state *est, *gst;
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags |= EVL_MONITOR_SIGNALED;
		est = event->__event.active.state;
		est->flags |= EVL_MONITOR_SIGNALED|EVL_MONITOR_BROADCAST;
	}

	return 0;
}
