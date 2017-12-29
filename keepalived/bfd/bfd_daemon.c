/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        BFD child process handling
 *
 * Author:      Ilya Voronin, <ivoronin@gmail.com>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2015-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#ifdef BFD_SCHED_RT
#include <sched.h>
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>

#include "bfd.h"
#include "bfd_daemon.h"
#include "bfd_data.h"
#include "bfd_parser.h"
#include "bfd_scheduler.h"
#include "bfd_event.h"
#include "pidfile.h"
#include "logger.h"
#include "signals.h"
#include "list.h"
#include "main.h"
#include "parser.h"
#include "time.h"
#include "global_data.h"
#include "bitops.h"
#include "utils.h"
#include "scheduler.h"

/* Global variables */
int bfd_event_pipe[2] = { -1, -1};

/* Local variables */
static char *bfd_syslog_ident;

static int reload_bfd_thread(thread_t *);

/* Daemon stop sequence */
static void
stop_bfd(int status)
{
	signal_handler_destroy();

	/* Stop daemon */
	pidfile_rm(bfd_pidfile);

	/* Clean data */
	free_global_data(global_data);
	bfd_dispatcher_release(bfd_data);
	free_bfd_data(bfd_data);
	free_bfd_buffer();
	thread_destroy_master(master);

#ifdef _DEBUG_
	keepalived_free_final("BFD Child process");
#endif

	/*
	 * Reached when terminate signal catched.
	 * finally return to parent process.
	 */
	log_message(LOG_INFO, "Stopped");

	if (log_file_name)
		close_log_file();
	closelog();

#ifndef _MEM_CHECK_LOG_
	FREE_PTR(bfd_syslog_ident);
#else
        if (bfd_syslog_ident)
                free(bfd_syslog_ident);
#endif
	close_std_fd();

	exit(status);
}

/* Daemon init sequence */
void
open_bfd_pipe(void)
{
	/* Open BFD control pipe */
	if (pipe(bfd_event_pipe) == -1) {
		log_message(LOG_ERR, "Unable to create BFD event pipe: %m");
		stop_keepalived();
		return;
	}
	fcntl(bfd_event_pipe[0], F_SETFL, O_NONBLOCK | fcntl(bfd_event_pipe[0], F_GETFL));
	fcntl(bfd_event_pipe[1], F_SETFL, O_NONBLOCK | fcntl(bfd_event_pipe[1], F_GETFL));
}

/* Daemon init sequence */
static void
start_bfd(void)
{
	srand(time(NULL));

	global_data = alloc_global_data();
	if (!(bfd_data = alloc_bfd_data())) {
		stop_bfd(KEEPALIVED_EXIT_FATAL);
		return;
	}

	alloc_bfd_buffer();

	init_data(conf_file, bfd_init_keywords);

	bfd_complete_init();

	if (__test_bit(DUMP_CONF_BIT, &debug))
		dump_bfd_data(bfd_data);

	thread_add_event(master, bfd_dispatcher_init, bfd_data, 0);
}

/* Reload handler */
static void
sighup_bfd(__attribute__ ((unused)) void *v,
	   __attribute__ ((unused)) int sig)
{
	thread_add_event(master, reload_bfd_thread, NULL, 0);
}

/* Terminate handler */
static void
sigend_bfd(__attribute__ ((unused)) void *v,
	   __attribute__ ((unused)) int sig)
{
	if (master)
		thread_add_terminate_event(master);
}

/* BFD Child signal handling */
static void
bfd_signal_init(void)
{
	signal_handler_init();
	signal_set(SIGHUP, sighup_bfd, NULL);
	signal_set(SIGINT, sigend_bfd, NULL);
	signal_set(SIGTERM, sigend_bfd, NULL);
	signal_ignore(SIGPIPE);
}

/* Reload thread */
static int
reload_bfd_thread(__attribute__((unused)) thread_t * thread)
{
	timeval_t timer;
	timer = timer_now();

	/* set the reloading flag */
	SET_RELOAD;

	/* Signal handling */
	signal_handler_destroy();

	/* Destroy master thread */
	bfd_dispatcher_release(bfd_data);
	thread_destroy_master(master);
	master = thread_make_master();
	free_global_data(global_data);
	free_bfd_buffer();

	old_bfd_data = bfd_data;
	bfd_data = NULL;

	/* Reload the conf */
	signal_set(SIGCHLD, thread_child_handler, master);
	start_bfd();

	free_bfd_data(old_bfd_data);
	UNSET_RELOAD;

	log_message(LOG_INFO, "Reload finished in %li usec",
		    timer_tol(timer_sub_now(timer)));

	return 0;
}

/* BFD Child respawning thread */
static int
bfd_respawn_thread(thread_t * thread)
{
	pid_t pid;

	/* Fetch thread args */
	pid = THREAD_CHILD_PID(thread);

	/* Restart respawning thread */
	if (thread->type == THREAD_CHILD_TIMEOUT) {
		thread_add_child(master, bfd_respawn_thread, NULL,
				 pid, RESPAWN_TIMER);
		return 0;
	}

	/* We catch a SIGCHLD, handle it */
	if (!__test_bit(DONT_RESPAWN_BIT, &debug)) {
		log_message(LOG_ALERT, "BFD child process(%d) died: Respawning",
			    pid);
		start_bfd_child();
	} else {
		log_message(LOG_ALERT, "BFD child process(%d) died: Exiting",
			    pid);
		raise(SIGTERM);
	}
	return 0;
}

int
start_bfd_child(void)
{
#ifndef _DEBUG_
	pid_t pid;
	int ret;
	char *syslog_ident;

	/* Initialize child process */
	if (log_file_name)
		flush_log_file();

	pid = fork();

	if (pid < 0) {
		log_message(LOG_INFO, "BFD child process: fork error(%m)");
		return -1;
	} else if (pid) {
		bfd_child = pid;
		log_message(LOG_INFO, "Starting BFD child process, pid=%d",
			    pid);
		/* Start respawning thread */
		thread_add_child(master, bfd_respawn_thread, NULL,
				 pid, RESPAWN_TIMER);
		return 0;
	}
	prctl(PR_SET_PDEATHSIG, SIGTERM);

	/* Clear any child finder functions set in parent */
	set_child_finder_name(NULL);
	set_child_finder(NULL, NULL, NULL, NULL, NULL, 0);	/* Currently these won't be set */

	prog_type = PROG_TYPE_BFD;

	if ((instance_name
#if HAVE_DECL_CLONE_NEWNET
			   || network_namespace
#endif
					       ) &&
	     (bfd_syslog_ident = make_syslog_ident(PROG_BFD)))
		syslog_ident = bfd_syslog_ident;
	else
		syslog_ident = PROG_BFD;

	/* Opening local BFD syslog channel */
	if (!__test_bit(NO_SYSLOG_BIT, &debug))
		openlog(syslog_ident, LOG_PID | ((__test_bit(LOG_CONSOLE_BIT, &debug)) ? LOG_CONS : 0)
				    , (log_facility==LOG_DAEMON) ? LOG_LOCAL2 : log_facility);

	if (log_file_name)
		open_log_file(log_file_name, "bfd", network_namespace, instance_name);

	signal_handler_destroy();

#ifdef _MEM_CHECK_
	mem_log_init(PROG_CHECK, "Healthcheck child process");
#endif

	free_parent_mallocs_startup(true);

#ifdef BFD_SCHED_RT
	/* Set realtime priority */
	struct sched_param sp;
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	if (sched_setscheduler(pid, SCHED_RR, &sp))
		log_message(LOG_WARNING,
			    "BFD child process: cannot raise priority");
#endif

	/* Child process part, write pidfile */
	if (!pidfile_write(bfd_pidfile, getpid())) {
		/* Fatal error */
		log_message(LOG_INFO,
			    "BFD child process: cannot write pidfile");
		exit(0);
	}

	/* Create the new master thread */
	signal_handler_destroy();
	thread_destroy_master(master);
	master = thread_make_master();

	/* change to / dir */
	ret = chdir("/");
	if (ret < 0) {
		log_message(LOG_INFO, "BFD child process: error chdir");
	}

	/* Set mask */
	umask(0);
#endif

	/* If last process died during a reload, we can get there and we
	 * don't want to loop again, because we're not reloading anymore.
	 */
	UNSET_RELOAD;

	/* Signal handling initialization */
	bfd_signal_init();

	/* Start BFD daemon */
	start_bfd();

	/* Launch the scheduling I/O multiplexer */
	launch_scheduler();

	/* Finish BFD daemon process */
	stop_bfd(EXIT_SUCCESS);

	/* unreachable */
	exit(EXIT_SUCCESS);
}
