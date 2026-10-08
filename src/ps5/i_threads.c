// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by James Robert Roman.
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/i_threads.c
/// \brief Multithreading abstraction - PlayStation 5
///
/// sdl/i_threads.c on pthreads, which the console's libkernel provides. The
/// one thing to carry over carefully is that SDL's mutexes are recursive and
/// the engine relies on it, so these are created PTHREAD_MUTEX_RECURSIVE.

#include "../doomdef.h"
#include "../i_threads.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef void * (*Create_fn)(void);

struct Link;
struct Thread;

typedef struct Link   * Link;
typedef struct Thread * Thread;

struct Link
{
	void * data;
	Link   next;
	Link   prev;
};

struct Thread
{
	I_thread_fn   entry;
	void        * userdata;

	pthread_t     thread;
	int           started;
};

static Link    i_thread_pool;
static Link    i_mutex_pool;
static Link    i_cond_pool;

static I_mutex        i_thread_pool_mutex;
static I_mutex        i_mutex_pool_mutex;
static I_mutex        i_cond_pool_mutex;

static atomic_int     i_threads_running = 1;

static void *
Create_mutex (void)
{
	pthread_mutex_t     * mutex;
	pthread_mutexattr_t   attr;

	mutex = malloc(sizeof *mutex);

	if (! mutex)
		return NULL;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

	if (pthread_mutex_init(mutex, &attr) != 0)
	{
		pthread_mutexattr_destroy(&attr);
		free(mutex);
		return NULL;
	}

	pthread_mutexattr_destroy(&attr);
	return mutex;
}

static void *
Create_cond (void)
{
	pthread_cond_t * cond;

	cond = malloc(sizeof *cond);

	if (! cond)
		return NULL;

	if (pthread_cond_init(cond, NULL) != 0)
	{
		free(cond);
		return NULL;
	}

	return cond;
}

static Link
Insert_link (
		Link * head,
		Link   link
){
	link->prev = NULL;
	link->next = (*head);
	if ((*head))
		(*head)->prev = link;
	(*head)    = link;
	return link;
}

static void
Free_link (
		Link * head,
		Link   link
){
	if (link->prev)
		link->prev->next = link->next;
	else
		(*head) = link->next;

	if (link->next)
		link->next->prev = link->prev;

	free(link->data);
	free(link);
}

static Link
New_link (void *data)
{
	Link link;

	link = malloc(sizeof *link);

	if (! link)
		abort();

	link->data = data;

	return link;
}

static void *
Identity (
		Link      *  pool_anchor,
		I_mutex      pool_mutex,

		void      ** anchor,

		Create_fn    create_fn
){
	void * id;

	id = __atomic_load_n(anchor, __ATOMIC_ACQUIRE);

	if (! id)
	{
		I_lock_mutex(&pool_mutex);
		{
			id = __atomic_load_n(anchor, __ATOMIC_ACQUIRE);

			if (! id)
			{
				id = (*create_fn)();

				if (! id)
					abort();

				Insert_link(pool_anchor, New_link(id));

				__atomic_store_n(anchor, id, __ATOMIC_RELEASE);
			}
		}
		I_unlock_mutex(pool_mutex);
	}

	return id;
}

static void *
Worker (
		void * arg
){
	Link   link = arg;
	Thread th;

	th = link->data;

	(*th->entry)(th->userdata);

	if (atomic_load(&i_threads_running))
	{
		I_lock_mutex(&i_thread_pool_mutex);
		{
			if (atomic_load(&i_threads_running))
			{
				pthread_detach(th->thread);
				Free_link(&i_thread_pool, link);
			}
		}
		I_unlock_mutex(i_thread_pool_mutex);
	}

	return NULL;
}

void
I_spawn_thread (
		const char  * name,
		I_thread_fn   entry,
		void        * userdata
){
	Link   link;
	Thread th;

	(void)name;

	th = malloc(sizeof *th);

	if (! th)
		abort();

	th->entry    = entry;
	th->userdata = userdata;
	th->started  = 0;

	I_lock_mutex(&i_thread_pool_mutex);
	{
		link = Insert_link(&i_thread_pool, New_link(th));

		if (atomic_load(&i_threads_running))
		{
			if (pthread_create(&th->thread, NULL, Worker, link) != 0)
				abort();
			th->started = 1;
		}
	}
	I_unlock_mutex(i_thread_pool_mutex);
}

int
I_thread_is_stopped (void)
{
	return ( ! atomic_load(&i_threads_running) );
}

void
I_start_threads (void)
{
	i_thread_pool_mutex = Create_mutex();
	i_mutex_pool_mutex  = Create_mutex();
	i_cond_pool_mutex   = Create_mutex();

	if (!(
				i_thread_pool_mutex &&
				i_mutex_pool_mutex  &&
				i_cond_pool_mutex
	)){
		abort();
	}
}

void
I_stop_threads (void)
{
	Link        link;
	Link        next;

	Thread      th;
	pthread_mutex_t * mutex;
	pthread_cond_t  * cond;

	if (atomic_load(&i_threads_running))
	{
		/* rely on the good will of thread-san */
		atomic_store(&i_threads_running, 0);

		I_lock_mutex(&i_thread_pool_mutex);
		{
			for (
					link = i_thread_pool;
					link;
					link = next
			){
				next = link->next;
				th   = link->data;

				if (th->started)
					pthread_join(th->thread, NULL);

				free(th);
				free(link);
			}
		}
		I_unlock_mutex(i_thread_pool_mutex);

		for (
				link = i_mutex_pool;
				link;
				link = next
		){
			next  = link->next;
			mutex = link->data;

			pthread_mutex_destroy(mutex);
			free(mutex);

			free(link);
		}

		for (
				link = i_cond_pool;
				link;
				link = next
		){
			next = link->next;
			cond = link->data;

			pthread_cond_destroy(cond);
			free(cond);

			free(link);
		}

		pthread_mutex_destroy(i_thread_pool_mutex);
		pthread_mutex_destroy(i_mutex_pool_mutex);
		pthread_mutex_destroy(i_cond_pool_mutex);
		free(i_thread_pool_mutex);
		free(i_mutex_pool_mutex);
		free(i_cond_pool_mutex);
	}
}

void
I_lock_mutex (
		I_mutex * anchor
){
	pthread_mutex_t * mutex;

	mutex = Identity(
			&i_mutex_pool,
			i_mutex_pool_mutex,
			anchor,
			Create_mutex
	);

	pthread_mutex_lock(mutex);
}

void
I_unlock_mutex (
		I_mutex id
){
	pthread_mutex_unlock(id);
}

void
I_hold_cond (
		I_cond  * cond_anchor,
		I_mutex   mutex_id
){
	pthread_cond_t * cond;

	cond = Identity(
			&i_cond_pool,
			i_cond_pool_mutex,
			cond_anchor,
			Create_cond
	);

	pthread_cond_wait(cond, mutex_id);
}

void
I_wake_one_cond (
		I_cond * anchor
){
	pthread_cond_t * cond;

	cond = Identity(
			&i_cond_pool,
			i_cond_pool_mutex,
			anchor,
			Create_cond
	);

	pthread_cond_signal(cond);
}

void
I_wake_all_cond (
		I_cond * anchor
){
	pthread_cond_t * cond;

	cond = Identity(
			&i_cond_pool,
			i_cond_pool_mutex,
			anchor,
			Create_cond
	);

	pthread_cond_broadcast(cond);
}
