/* enviable - An LD_PRELOAD to set environment variables at runtime.
 * Usage: LD_PRELOAD=./libenviable.so bash
 *
 *
 * Copyright (c) 2013  Geoffrey Thomas <geofft@ldpreload.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
*/

#define _GNU_SOURCE
#include <sys/inotify.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef DEBUG
#include <stdio.h>
#define maybe_perror(s) perror(s)
#else
#define maybe_perror(s)
#endif

static int (*real___libc_current_sigrtmin)(void) = NULL;

static char *watchfile = "/tmp/vars";

/* The following definitions are stolen from bash-4.2 */
__attribute__((weak))
extern int do_assignment_no_expand(char *);
__attribute__((weak))
extern void set_var_attribute(char *, int, int);
#define att_exported 1

int
__libc_current_sigrtmin(void)
{
	return real___libc_current_sigrtmin() + 1;
}

static void
enviable_setenv(char *line)
{
	char *eq = strchr(line, '=');
	if (do_assignment_no_expand && set_var_attribute) {
		/* We're running inside bash. Since bash maintains its
		 * own idea of the environment (shell variables marked
		 * exported), we need to go through that for child
		 * processes to see changes. */
		do_assignment_no_expand(line);
		*eq = '\0';
		set_var_attribute(line, att_exported, 0);
	} else {
		/* Unknown host process -- fall back to libc. */
		*eq = '\0';
		setenv(line, eq + 1, 1);
	}
}

static void
enviable_callback(int signum, siginfo_t *si, void *context)
{
	struct inotify_event event;
	if (read(si->si_fd, &event, sizeof(event)) != sizeof(event))
		return;

	FILE *fd = fopen(watchfile, "r");
	if (!fd) {
		return;
	}
	char *line = NULL;
	size_t n = 0;
	ssize_t len;
	while ((len = getline(&line, &n, fd)) > 0) {
		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
		} else {
			/* Conservatively reject partial lines. */
			continue;
		}
		enviable_setenv(line);
	}
	free(line);
	fclose(fd);
}

void __attribute__((constructor))
enviable_init(void)
{
	real___libc_current_sigrtmin = dlsym(RTLD_NEXT, "__libc_current_sigrtmin");
	if (!real___libc_current_sigrtmin) {
		/* Give up; we can't safely claim a signal. */
		maybe_perror("enviable: incompatible libc");
		return;
	}

	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd == -1) {
		maybe_perror("enviable: inotify_init");
		return;
	}

	int watch = inotify_add_watch(fd, watchfile, IN_CLOSE_WRITE);
	if (watch == -1) {
		maybe_perror("enviable: inotify_add_watch");
		goto err;
	}

	int sigrtmin = real___libc_current_sigrtmin();

	struct sigaction act = {
		.sa_sigaction = enviable_callback,
		.sa_mask = 0,
		.sa_flags = SA_SIGINFO,
	}, oldact;

	if (sigaction(sigrtmin, &act, &oldact) == -1) {
		maybe_perror("enviable: sigaction");
		goto err;
	}

	if ((oldact.sa_flags & SA_SIGINFO) ||
	    (oldact.sa_handler != SIG_DFL)) {
		/* Oops. Someone else already claimed a handler! */
		goto err_sig;
	}

	int flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		maybe_perror("enviable: fcntl F_GETFL");
		goto err_sig;
	}
	if (fcntl(fd, F_SETFL, flags | O_ASYNC) == -1) {
		maybe_perror("enviable: fcntl F_SETFL O_ASYNC");
		goto err_sig;
	}
	if (fcntl(fd, F_SETOWN, getpid()) == -1) {
		maybe_perror("enviable: fcntl F_SETOWN");
		goto err_sig;
	}
	if (fcntl(fd, F_SETSIG, sigrtmin) == -1) {
		maybe_perror("enviable: fcntl F_SETSIG");
		goto err_sig;
	}

	return;
err_sig:
	sigaction(sigrtmin, &oldact, NULL);
err:
	close(fd);
}
