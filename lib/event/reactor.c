/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/event.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include "reactor.h"

#include "spdk/log.h"

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_INVALID = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_reactor {
	/* Logical core number for this reactor. */
	uint32_t			lcore;

	/*
	 * Contains pollers actively running on this reactor.  Pollers
	 *  are run round-robin. The reactor takes one poller from the head
	 *  of the ring, executes it, then puts it back at the tail of
	 *  the ring.
	 */
	struct rte_ring			*active_pollers;

	struct rte_ring			*events;
};

static struct spdk_reactor g_reactors[RTE_MAX_LCORE];
static uint64_t	g_reactor_mask  = 0;
static int	g_reactor_count = 0;

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore);

struct rte_mempool *g_spdk_event_mempool;

/** \file

*/

static struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	reactor = &g_reactors[lcore];
	return reactor;
}

spdk_event_t
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2,
		    spdk_event_t next)
{
	struct spdk_event *event = NULL;
	int rc;

	rc = rte_mempool_get(g_spdk_event_mempool, (void **)&event);
	RTE_VERIFY((rc == 0) && (event != NULL));

	event->lcore = lcore;
	event->fn = fn;
	event->arg1 = arg1;
	event->arg2 = arg2;
	event->next = next;

	return event;
}

static void
spdk_event_free(struct spdk_event *event)
{
	rte_mempool_put(g_spdk_event_mempool, (void *)event);
}

void
spdk_event_call(spdk_event_t event)
{
	int rc;
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(event->lcore);

	RTE_VERIFY(reactor->events != NULL);
	rc = rte_ring_enqueue(reactor->events, event);
	RTE_VERIFY(rc == 0);
}

static uint32_t
spdk_event_queue_count(uint32_t lcore)
{
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(lcore);

	if (reactor->events == NULL) {
		return 0;
	}

	return rte_ring_count(reactor->events);
}

static void
spdk_event_queue_run_single(uint32_t lcore)
{
	struct spdk_event *event = NULL;
	struct spdk_reactor *reactor;
	int rc;

	reactor = spdk_reactor_get(lcore);

	RTE_VERIFY(reactor->events != NULL);
	rc = rte_ring_dequeue(reactor->events, (void **)&event);

	if ((rc != 0) || event == NULL) {
		return;
	}

	event->fn(event);
	spdk_event_free(event);
}

static void
spdk_event_queue_run(uint32_t lcore, uint32_t count)
{
	while (count--) {
		spdk_event_queue_run_single(lcore);
	}
}

void
spdk_event_queue_run_all(uint32_t lcore)
{
	uint32_t count;

	count = spdk_event_queue_count(lcore);
	spdk_event_queue_run(lcore, count);
}

/**

\brief Set current reactor thread name to "reactor <cpu #>".

This makes the reactor threads distinguishable in top and gdb.

*/
static void set_reactor_thread_name(void)
{
	char thread_name[16];

	snprintf(thread_name, sizeof(thread_name), "reactor %d",
		 rte_lcore_id());

#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);
#else
#error missing platform support for thread name
#endif
}

/**

\brief This is the main function of the reactor thread.

\code

while (1)
	if (new work items to be scheduled)
		dequeue work item from new work item ring
		enqueue work item to active work item ring
	else if (active work item count > 0)
		dequeue work item from active work item ring
		invoke work item function pointer
		if (work item state == RUNNING)
			enqueue work item to active work item ring
	else if (application state != RUNNING)
		# exit the reactor loop
		break
	else
		sleep for 100ms

\endcode

Note that new work items are posted to a separate ring so that the
active work item ring can be kept single producer/single consumer and
only be touched by reactor itself.  This avoids atomic operations
on the active work item ring which would hurt performance.

*/
static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_poller	*poller = NULL;
	int			rc;

	set_reactor_thread_name();
	SPDK_NOTICELOG("waiting for work item to arrive...\n");

	while (1) {
		spdk_event_queue_run_all(rte_lcore_id());

		rte_timer_manage();

		if (rte_ring_dequeue(reactor->active_pollers, (void **)&poller) == 0) {
			poller->fn(poller->arg);
			rc = rte_ring_enqueue(reactor->active_pollers,
					      (void *)poller);
			if (rc != 0) {
				SPDK_ERRLOG("poller could not be enqueued\n");
				exit(EXIT_FAILURE);
			}
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}
	}

	return 0;
}

static void
spdk_reactor_construct(struct spdk_reactor *reactor, uint32_t lcore)
{
	char	ring_name[64];

	reactor->lcore = lcore;

	snprintf(ring_name, sizeof(ring_name), "spdk_active_pollers_%d", lcore);
	reactor->active_pollers =
		rte_ring_create(ring_name, SPDK_POLLER_RING_SIZE, rte_lcore_to_socket_id(lcore),
				RING_F_SP_ENQ | RING_F_SC_DEQ);

	snprintf(ring_name, sizeof(ring_name) - 1, "spdk_event_queue_%u", lcore);
	reactor->events =
		rte_ring_create(ring_name, 65536, rte_lcore_to_socket_id(lcore), RING_F_SC_DEQ);
	RTE_VERIFY(reactor->events != NULL);
}

static void
spdk_reactor_start(struct spdk_reactor *reactor)
{
	if (reactor->lcore != rte_get_master_lcore()) {
		switch (rte_eal_get_lcore_state(reactor->lcore)) {
		case FINISHED:
			rte_eal_wait_lcore(reactor->lcore);
		/* drop through */
		case WAIT:
			rte_eal_remote_launch(_spdk_reactor_run, (void *)reactor, reactor->lcore);
			break;
		case RUNNING:
			printf("Something already running on lcore %d\n", reactor->lcore);
			break;
		}
	} else {
		_spdk_reactor_run(reactor);
	}
}

int
spdk_app_get_core_count(void)
{
	return g_reactor_count;
}

int
spdk_app_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	unsigned int i;
	char *end;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);
	if (*end != '\0' || errno) {
		return -1;
	}

	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if ((*cpumask & (1ULL << i)) && !rte_lcore_is_enabled(i)) {
			*cpumask &= ~(1ULL << i);
		}
	}

	return 0;
}

static int
spdk_reactor_parse_mask(const char *mask)
{
	int i;
	int ret = 0;
	uint32_t master_core = rte_get_master_lcore();

	if (g_reactor_state >= SPDK_REACTOR_STATE_INITIALIZED) {
		SPDK_ERRLOG("cannot set reactor mask after application has started\n");
		return -1;
	}

	g_reactor_mask = 0;

	if (mask == NULL) {
		/* No mask specified so use the same mask as DPDK. */
		RTE_LCORE_FOREACH(i) {
			g_reactor_mask |= (1ULL << i);
		}
	} else {
		ret = spdk_app_parse_core_mask(mask, &g_reactor_mask);
		if (ret != 0) {
			SPDK_ERRLOG("reactor mask %s specified on command line "
				    "is invalid\n", mask);
			return ret;
		}
		if (!(g_reactor_mask & (1ULL << master_core))) {
			SPDK_ERRLOG("master_core %d must be set in core mask\n", master_core);
			return -1;
		}
	}

	return 0;
}

uint64_t
spdk_app_get_core_mask(void)
{
	return g_reactor_mask;
}


static uint64_t
spdk_reactor_get_socket_mask(void)
{
	int i;
	uint32_t socket_id;
	uint64_t socket_info = 0;

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & g_reactor_mask)) {
			socket_id = rte_lcore_to_socket_id(i);
			socket_info |= (1ULL << socket_id);
		}
	}

	return socket_info;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i;

	RTE_VERIFY(rte_get_master_lcore() == rte_lcore_id());

	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;

	RTE_LCORE_FOREACH_SLAVE(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_start(reactor);
		}
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(rte_get_master_lcore());
	spdk_reactor_start(reactor);

	rte_eal_mp_wait_lcore();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

void spdk_reactors_stop(void)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

int
spdk_reactors_init(const char *mask)
{
	uint32_t i;
	int rc;
	struct spdk_reactor *reactor;

	rc = spdk_reactor_parse_mask(mask);
	if (rc < 0) {
		return rc;
	}

	printf("Occupied cpu core mask is 0x%lx\n", spdk_app_get_core_mask());
	printf("Occupied cpu socket mask is 0x%lx\n", spdk_reactor_get_socket_mask());

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_construct(reactor, i);
			g_reactor_count++;
		}
	}

	/* TODO: separate event mempools per socket */
	g_spdk_event_mempool = rte_mempool_create("spdk_event_mempool", 262144,
			       sizeof(struct spdk_event), 128, 0,
			       NULL, NULL, NULL, NULL,
			       SOCKET_ID_ANY, 0);

	if (g_spdk_event_mempool == NULL) {
		SPDK_ERRLOG("spdk_event_mempool allocation failed\n");
		return -1;
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return rc;
}

int
spdk_reactors_fini(void)
{
	/* TODO: free rings and mempool */
	return 0;
}

static void
_spdk_event_add_poller(spdk_event_t event)
{
	struct spdk_reactor *reactor = spdk_event_get_arg1(event);
	struct spdk_poller *poller = spdk_event_get_arg2(event);
	struct spdk_event *next = spdk_event_get_next(event);

	poller->lcore = reactor->lcore;

	rte_ring_enqueue(reactor->active_pollers, (void *)poller);

	if (next) {
		spdk_event_call(next);
	}
}

void
spdk_poller_register(struct spdk_poller *poller,
		     uint32_t lcore, spdk_event_t complete)
{
	struct spdk_reactor *reactor;
	struct spdk_event *event;

	reactor = spdk_reactor_get(lcore);
	event = spdk_event_allocate(lcore, _spdk_event_add_poller, reactor, poller, complete);
	spdk_event_call(event);
}

static void
_spdk_event_remove_poller(spdk_event_t event)
{
	struct spdk_reactor *reactor = spdk_event_get_arg1(event);
	struct spdk_poller *poller = spdk_event_get_arg2(event);
	struct spdk_event *next = spdk_event_get_next(event);
	struct spdk_poller *tmp = NULL;
	uint32_t i;
	int rc;

	/* Loop over all pollers, without breaking early, so that
	 * the list of pollers stays in the same order. */
	for (i = 0; i < rte_ring_count(reactor->active_pollers); i++) {
		rte_ring_dequeue(reactor->active_pollers, (void **)&tmp);
		if (tmp != poller) {
			rc = rte_ring_enqueue(reactor->active_pollers, (void *)tmp);
			if (rc != 0) {
				SPDK_ERRLOG("poller could not be enqueued\n");
				exit(EXIT_FAILURE);
			}
		}
	}

	if (next) {
		spdk_event_call(next);
	}
}

void
spdk_poller_unregister(struct spdk_poller *poller,
		       struct spdk_event *complete)
{
	struct spdk_reactor *reactor;
	struct spdk_event *event;

	reactor = spdk_reactor_get(poller->lcore);
	event = spdk_event_allocate(poller->lcore, _spdk_event_remove_poller, reactor, poller, complete);

	spdk_event_call(event);
}

static void
_spdk_poller_migrate(spdk_event_t event)
{
	struct spdk_poller *poller = spdk_event_get_arg1(event);
	struct spdk_event *next = spdk_event_get_next(event);

	/* Register the poller on the current lcore. This works
	 * because we already set this event up so that it is called
	 * on the new_lcore.
	 */
	spdk_poller_register(poller, rte_lcore_id(), next);
}

void
spdk_poller_migrate(struct spdk_poller *poller, int new_lcore,
		    struct spdk_event *complete)
{
	struct spdk_event *event;

	RTE_VERIFY(spdk_app_get_core_mask() & (1ULL << new_lcore));
	RTE_VERIFY(poller != NULL);

	event = spdk_event_allocate(new_lcore, _spdk_poller_migrate, poller, NULL, complete);

	spdk_poller_unregister(poller, event);
}
