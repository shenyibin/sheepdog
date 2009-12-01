/*
 * Copyright (C) 2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syscall.h>
#include <sys/types.h>
#include <linux/types.h>
#include <linux/signalfd.h>

#include "list.h"
#include "util.h"
#include "work.h"
#include "logger.h"
#include "event.h"

extern int signalfd(int fd, const sigset_t *mask, int flags);

struct worker_info {
	pthread_t worker_thread[NR_WORKER_THREAD];

	pthread_mutex_t finished_lock;
	struct list_head finished_list;

	/* wokers sleep on this and signaled by tgtd */
	pthread_cond_t pending_cond;
	/* locked by tgtd and workers */
	pthread_mutex_t pending_lock;
	/* protected by pending_lock */
	struct list_head pending_list;

	pthread_mutex_t startup_lock;

	int sig_fd;

	int stop;
};

static struct worker_info __wi;

static void bs_thread_request_done(int fd, int events, void *data)
{
	int ret;
	struct worker_info *wi = data;
	struct work *work;
	struct signalfd_siginfo siginfo[16];
	LIST_HEAD(list);

	ret = read(fd, (char *)siginfo, sizeof(siginfo));
	if (ret <= 0) {
		return;
	}

	pthread_mutex_lock(&wi->finished_lock);
	list_splice_init(&wi->finished_list, &list);
	pthread_mutex_unlock(&wi->finished_lock);

	while (!list_empty(&list)) {
		work = list_first_entry(&list, struct work, w_list);
		list_del(&work->w_list);

		work->done(work, 0);
	}
}

static void *worker_routine(void *arg)
{
	struct worker_info *wi = &__wi;
	struct work *work;
	pthread_t *p = arg;
	int idx = p - wi->worker_thread;
	sigset_t set;

	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, NULL);

	pthread_mutex_lock(&wi->startup_lock);
	dprintf("started this thread %d\n", idx);
	pthread_mutex_unlock(&wi->startup_lock);

	while (!wi->stop) {
		pthread_mutex_lock(&wi->pending_lock);
	retest:
		if (list_empty(&wi->pending_list)) {
			pthread_cond_wait(&wi->pending_cond, &wi->pending_lock);
			if (wi->stop) {
				pthread_mutex_unlock(&wi->pending_lock);
				pthread_exit(NULL);
			}
			goto retest;
		}

		work = list_first_entry(&wi->pending_list,
				       struct work, w_list);

		list_del(&work->w_list);
		pthread_mutex_unlock(&wi->pending_lock);

		work->fn(work, idx);

		pthread_mutex_lock(&wi->finished_lock);
		list_add_tail(&work->w_list, &wi->finished_list);
		pthread_mutex_unlock(&wi->finished_lock);

		kill(getpid(), SIGUSR2);
	}

	pthread_exit(NULL);
}

int init_worker(void)
{
	int i, ret;
	sigset_t mask;
	struct worker_info *wi = &__wi;

	INIT_LIST_HEAD(&wi->pending_list);
	INIT_LIST_HEAD(&wi->finished_list);

	pthread_cond_init(&wi->pending_cond, NULL);

	pthread_mutex_init(&wi->finished_lock, NULL);
	pthread_mutex_init(&wi->pending_lock, NULL);
	pthread_mutex_init(&wi->startup_lock, NULL);

	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR2);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	wi->sig_fd = signalfd(-1, &mask, 0);
	if (wi->sig_fd < 0) {
		eprintf("failed to create a signal fd, %m\n");
		return 1;
	}

	ret = fcntl(wi->sig_fd, F_GETFL);
	ret = fcntl(wi->sig_fd, F_SETFL, ret | O_NONBLOCK);

	ret = register_event(wi->sig_fd, bs_thread_request_done, wi);
	if (ret) {
		eprintf("failed to add epoll event\n");
		goto destroy_cond_mutex;
	}

	pthread_mutex_lock(&wi->startup_lock);
	for (i = 0; i < NR_WORKER_THREAD; i++) {
		ret = pthread_create(&wi->worker_thread[i], NULL,
				     worker_routine, &wi->worker_thread[i]);

		if (ret) {
			eprintf("failed to create a worker thread, %d %s\n",
				i, strerror(ret));
			if (ret)
				goto destroy_threads;
		}
	}
	pthread_mutex_unlock(&wi->startup_lock);

	return 0;
destroy_threads:

	wi->stop = 1;
	pthread_mutex_unlock(&wi->startup_lock);
	for (; i > 0; i--) {
		pthread_join(wi->worker_thread[i - 1], NULL);
		eprintf("stopped the worker thread %d\n", i - 1);
	}

	unregister_event(wi->sig_fd);
destroy_cond_mutex:
	pthread_cond_destroy(&wi->pending_cond);
	pthread_mutex_destroy(&wi->pending_lock);
	pthread_mutex_destroy(&wi->startup_lock);
	pthread_mutex_destroy(&wi->finished_lock);

	return 1;
}

void exit_worker(void)
{
	int i;
	struct worker_info *wi = &__wi;

	wi->stop = 1;
	pthread_cond_broadcast(&wi->pending_cond);

	for (i = 0; wi->worker_thread[i] &&
		     i < ARRAY_SIZE(wi->worker_thread); i++)
		pthread_join(wi->worker_thread[i], NULL);

	pthread_cond_destroy(&wi->pending_cond);
	pthread_mutex_destroy(&wi->pending_lock);
	pthread_mutex_destroy(&wi->startup_lock);
	pthread_mutex_destroy(&wi->finished_lock);

	unregister_event(wi->sig_fd);

	wi->stop = 0;
}

void queue_work(struct work *work)
{
	struct worker_info *wi = &__wi;

	pthread_mutex_lock(&wi->pending_lock);

	list_add_tail(&work->w_list, &wi->pending_list);

	pthread_mutex_unlock(&wi->pending_lock);

	pthread_cond_signal(&wi->pending_cond);
}