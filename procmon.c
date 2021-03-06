/*-
 * xnumon - monitor macOS for malicious activity
 * https://www.roe.ch/xnumon
 *
 * Copyright (c) 2017-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Licensed under the Open Software License version 3.0.
 */

/*
 * Process monitoring core.
 *
 * Plenty of refactoring opportunities here.
 */

#include "procmon.h"

#include "tommylist.h"
#include "proc.h"
#include "hashes.h"
#include "cachehash.h"
#include "cachecsig.h"
#include "time.h"
#include "work.h"
#include "filemon.h"
#include "atomic.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

static config_t *config;

/* prepq state */
static tommy_list pqlist;
pthread_mutex_t pqmutex;        /* protects pqlist */
static uint64_t pqsize;         /* current number of elements in pqlist */
static uint64_t pqlookup;       /* counts total number of lookups in pq */
static uint64_t pqmiss;         /* counts no preloaded image found in pq */
static uint64_t pqdrop;         /* counts preloaded imgs removed due max TTL */
static uint64_t pqskip;         /* counts non-matching entries skipped in pq */

static atomic32_t images;
static uint64_t liveacq;        /* counts live process acquisitions */
static uint64_t miss_bypid;     /* counts various miss conditions */
static uint64_t miss_forksubj;
static uint64_t miss_execsubj;
static uint64_t miss_execinterp;
static uint64_t miss_chdirsubj;
static uint64_t miss_getcwd;
static atomic64_t ooms;         /* counts events impaired due to OOM */

strset_t *suppress_image_exec_by_ident;
strset_t *suppress_image_exec_by_path;
strset_t *suppress_image_exec_by_ancestor_ident;
strset_t *suppress_image_exec_by_ancestor_path;

static int image_exec_work(image_exec_t *);

/*
 * Ownership of path will be transfered to image_exec; caller must not assume
 * that path still exists after calling this function.  Path is also freed when
 * this function fails and returns NULL.
 *
 * Thread-safe.
 */
static image_exec_t *
image_exec_new(char *path) {
	image_exec_t *image;

	assert(path);

	image = malloc(sizeof(image_exec_t));
	if (!image) {
		free(path);
		atomic64_inc(&ooms);
		return NULL;
	}
	bzero(image, sizeof(image_exec_t));
	pthread_mutex_init(&image->refsmutex, NULL);
	image->refs = 1;
#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: image_exec_new(%p) refs=%zu\n",
	                image, image->refs);
#endif
	image->path = path;
	image->fd = -1;
	image->hdr.code = LOGEVT_IMAGE_EXEC;
	image->hdr.le_work = (__typeof__(image->hdr.le_work))image_exec_work;
	image->hdr.le_free = (__typeof__(image->hdr.le_free))image_exec_free;
	atomic32_inc(&images);
	return image;
}

/*
 * Must not use config because config will be set to NULL before the last
 * instances of image_exec are drained out of the log queue.
 */
void
image_exec_free(image_exec_t *image) {
	assert(image);
	pthread_mutex_lock(&image->refsmutex);
	image->refs--;
#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: image_exec_free(%p) refs=%zu (was %zu)\n",
	                image, image->refs, image->refs + 1);
#endif
	if (image->refs > 0) {
		pthread_mutex_unlock(&image->refsmutex);
		return;
	}
	pthread_mutex_unlock(&image->refsmutex);
	pthread_mutex_destroy(&image->refsmutex);
	if (image->script)
		image_exec_free(image->script);
	if (image->prev)
		image_exec_free(image->prev);
	if (image->path)
		free(image->path);
	if (image->cwd)
		free(image->cwd);
	if (image->codesign)
		codesign_free(image->codesign);
	atomic32_dec(&images);
	free(image);
}

static void
image_exec_ref(image_exec_t *image) {
	assert(image);
	pthread_mutex_lock(&image->refsmutex);
	image->refs++;
#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: image_exec_ref(%p) refs=%zu (was %zu)\n",
	                image, image->refs, image->refs - 1);
#endif
	pthread_mutex_unlock(&image->refsmutex);
}

/*
 * Prune history of exec images to config->ancestors previous levels.  Go
 * back in history and free previous image iff on the whole path, all images
 * were exclusively owned by this one, i.e. had a refcount of 1, in order to
 * make sure that we are not cutting short another fork's exec history.
 */
static void
image_exec_prune_ancestors(image_exec_t *image, size_t level) {
	assert(image);

#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: image_exec_prune_ancestors(%p, level %zu) "
	                "refs=%zu\n", image, level, image->refs);
#endif
	if (!image->prev)
		return;
	if (level >= config->ancestors) {
		image_exec_free(image->prev);
		image->prev = NULL;
		return;
	}
	if (image->refs == 1)
		image_exec_prune_ancestors(image->prev, level + 1);
}

/*
 * Partially thread-safe: only a single thread may call functions on a given
 * image_exec_t instance at a time.
 */
static int
image_exec_open(image_exec_t *image, const audit_attr_t *attr) {
	char buf[2];
	int rv;

	if (image->flags & (EIFLAG_STAT|EIFLAG_ATTR)) {
#ifdef DEBUG_EXECIMAGE
		fprintf(stderr, "DEBUG_EXECIMAGE: already have stat\n");
#endif
		return 0;
	}

	if (image->flags & EIFLAG_NOPATH) {
		if (attr)
			goto fallback;
		return -1;
	}

	assert(!!strncmp(image->path, "/dev/", 5));
	image->fd = open(image->path, O_RDONLY);
	if (image->fd == -1) {
		if (attr)
			goto fallback;
		return -1;
	}

	rv = sys_fdattr(&image->stat, image->fd);
	if (rv == -1) {
		if (attr)
			goto fallback;
		return -1;
	}

	if (attr && ((image->stat.mode != attr->mode) ||
	             (image->stat.uid != attr->uid) ||
	             (image->stat.gid != attr->gid) ||
	             (image->stat.dev != attr->dev) ||
	             (image->stat.ino != attr->ino)))
		goto fallback;

	/* https://www.in-ulm.de/~mascheck/various/shebang/ */
	if (pread(image->fd, buf, sizeof(buf), 0) == 2)
		if (buf[0] == '#' && buf[1] == '!')
			image->flags |= EIFLAG_SHEBANG;

	image->flags |= EIFLAG_STAT;
#ifdef DEBUG_EXECIMAGE
	fprintf(stderr, "DEBUG_EXECIMAGE: stat from path=%s\n", image->path);
#endif
	return 0;

fallback:
	image->stat.mode = attr->mode;
	image->stat.uid = attr->uid;
	image->stat.gid = attr->gid;
	image->stat.dev = attr->dev;
	image->stat.ino = attr->ino;
	image->flags |= EIFLAG_ATTR;
	return 0;
}

/*
 * Partially thread-safe: only a single thread may call functions on a given
 * image_exec_t instance at a time.
 */
static void
image_exec_close(image_exec_t *image) {
	assert(image);
	if (image->fd != -1) {
		close(image->fd);
		image->fd = -1;
	}
}

/*
 * Kern indicates if we are currently handling a kernel module callback.
 *
 * Partially thread-safe: only a single thread may call functions on a given
 * image_exec_t instance at a time.
 */
static int
image_exec_acquire(image_exec_t *image, bool kern) {
	stat_attr_t st;
	off_t sz;
	bool hit;
	int rv;

	assert(image);

	if (image->flags & EIFLAG_DONE)
		return 0;

	/* postpone hashes for later offline processing */
	if (kern && config->kextlevel < KEXTLEVEL_HASH)
		return 0;

	/* postpone large binaries for later offline processing */
	if (kern && image->stat.size > 1024*1024*8)
		return 0;

	if (!(image->flags & EIFLAG_HASHES)) {
		if (!(image->flags & EIFLAG_STAT) || image->fd == -1) {
			image->flags |= EIFLAG_DONE;
			return -1;
		}

		hit = cachehash_get(&image->hashes,
		                    image->stat.dev,
		                    image->stat.ino,
		                    &image->stat.mtime,
		                    &image->stat.ctime,
		                    &image->stat.btime);
		if (!hit) {
			/* cache miss, calculate hashes */
			rv = hashes_fd(&sz, &image->hashes, config->hflags,
			               image->fd);
			if ((rv == -1) || (sz != image->stat.size)) {
				close(image->fd);
				image->fd = -1;
				image->flags |= EIFLAG_DONE;
				return -1;
			}

			/*
			 * If 2nd stat does not match 1st, invalidate hashes.
			 */
			rv = sys_fdattr(&st, image->fd);
			if (rv == -1) {
				close(image->fd);
				image->fd = -1;
				image->flags |= EIFLAG_DONE;
				return -1;
			}
			/* fd still open, no need to compare dev and ino */
			if ((image->stat.size != st.size) ||
			    (image->stat.mtime.tv_sec != st.mtime.tv_sec) ||
			    (image->stat.mtime.tv_nsec != st.mtime.tv_nsec) ||
			    (image->stat.ctime.tv_sec != st.ctime.tv_sec) ||
			    (image->stat.ctime.tv_nsec != st.ctime.tv_nsec) ||
			    (image->stat.btime.tv_sec != st.btime.tv_sec) ||
			    (image->stat.btime.tv_nsec != st.btime.tv_nsec)) {
				image->flags &= ~EIFLAG_HASHES;
				close(image->fd);
				image->fd = -1;
				image->flags |= EIFLAG_DONE;
				return -1;
			}
			cachehash_put(image->stat.dev,
			              image->stat.ino,
			              &image->stat.mtime,
			              &image->stat.ctime,
			              &image->stat.btime,
			              &image->hashes);
#ifdef DEBUG_EXECIMAGE
			fprintf(stderr, "DEBUG_EXECIMAGE: hashes from path=%s\n", image->path);
#endif
		}
#ifdef DEBUG_EXECIMAGE
		else
			fprintf(stderr, "DEBUG_EXECIMAGE: hashes from cache\n");
#endif
		image->flags |= EIFLAG_HASHES;
	}
#ifdef DEBUG_EXECIMAGE
	else
		fprintf(stderr, "DEBUG_EXECIMAGE: already have hashes\n");
#endif

	/* everything below operates on paths, not open file descriptors */
	if (image->fd != -1) {
		close(image->fd);
		image->fd = -1;
	}

	/* postpone codesign for later offline processing? */
	if (kern && config->kextlevel < KEXTLEVEL_CSIG) {
		return 0;
	}

	/* skip code signing for scripts */
	if (image->flags & EIFLAG_SHEBANG) {
		image->flags |= EIFLAG_DONE;
		return 0;
	}

#ifdef DEBUG_EXECIMAGE
	if (image->codesign)
		fprintf(stderr, "DEBUG_EXECIMAGE: already have codesign\n");
#endif
	if (!image->codesign && (image->flags & EIFLAG_HASHES)) {
		image->codesign = cachecsig_get(&image->hashes);
		if (!image->codesign) {
			if (errno == ENOMEM) {
				image->flags |= EIFLAG_ENOMEM;
				image->flags |= EIFLAG_DONE;
				return -1;
			}
		}
#ifdef DEBUG_EXECIMAGE
		else
			fprintf(stderr, "DEBUG_EXECIMAGE: codesign from cache\n");
#endif
	}
	if (!image->codesign && config->codesign) {
		/* Postpone codesign verification of processes spawned as part
		 * of codesign verification during KAuth handling. */
		if (kern && (!strcmp(image->path, "/usr/libexec/xpcproxy") ||
		             !strcmp(image->path, "/usr/sbin/ocspd")))
			return 0;

		/* Check code signature (can be very slow!) */
		image->codesign = codesign_new(image->path, -1);
		if (!image->codesign) {
			if (errno == ENOMEM)
				image->flags |= EIFLAG_ENOMEM;
			image->flags |= EIFLAG_DONE;
			return -1;
		}

		/*
		 * If 3rd stat does not match 1st, invalidate codesign.
		 * If 3rd stat fails, return error but don't invalidate.
		 * The codesign routines fail internally if the data is changed
		 * during signature verification.
		 */
		rv = sys_pathattr(&st, image->path);
		if (rv == -1) {
			image->flags |= EIFLAG_DONE;
			return -1;
		}
		if ((image->stat.size != st.size) ||
		    (image->stat.dev != st.dev) ||
		    (image->stat.ino != st.ino) ||
		    (image->stat.mtime.tv_sec != st.mtime.tv_sec) ||
		    (image->stat.mtime.tv_nsec != st.mtime.tv_nsec) ||
		    (image->stat.ctime.tv_sec != st.ctime.tv_sec) ||
		    (image->stat.ctime.tv_nsec != st.ctime.tv_nsec) ||
		    (image->stat.btime.tv_sec != st.btime.tv_sec) ||
		    (image->stat.btime.tv_nsec != st.btime.tv_nsec)) {
			codesign_free(image->codesign);
			image->codesign = NULL;
			image->flags |= EIFLAG_DONE;
			return -1;
		}

		cachecsig_put(&image->hashes, image->codesign);
#ifdef DEBUG_EXECIMAGE
		fprintf(stderr, "DEBUG_EXECIMAGE: codesign from path=%s\n",
		                image->path);
#endif
	}

	image->flags |= EIFLAG_DONE;
	return 0;
}

/*
 * Return true iff exec image matches either one of the idents in by_ident or
 * one of the paths in by_path.
 */
bool
image_exec_match_suppressions(image_exec_t *ie,
                              strset_t *by_ident, strset_t *by_path) {
	if (ie->codesign && codesign_is_good(ie->codesign)) {
		if (strset_contains3(by_ident, ie->codesign->ident,
		                               ie->codesign->teamid))
			return true;
	}
	if (ie->path) {
		if (strset_contains(by_path, ie->path))
			return true;
	}
	if (ie->script && ie->script->path) {
		if (strset_contains(by_path, ie->script->path))
			return true;
	}
	return false;
}

/*
 * Work function to be executed in the worker thread.
 *
 * Returning 0 leads to the event being logged, -1 indicates that this event
 * should not be logged (may or may not be due to an error).
 *
 * Partially thread-safe: only a single thread may call functions on a given
 * image_exec_t instance at a time.
 */
static int
image_exec_work(image_exec_t *ei) {
#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: image_exec_work(%p)\n", ei);
#endif
	image_exec_acquire(ei, false);
	image_exec_close(ei);
	if (ei->script) {
		image_exec_acquire(ei->script, false);
		image_exec_close(ei->script);
	}
	if (config->ancestors < SIZE_MAX)
		image_exec_prune_ancestors(ei, false);
	if (ei->flags & EIFLAG_ENOMEM) {
		atomic64_inc(&ooms);
		return -1;
	}
	if (ei->flags & EIFLAG_NOLOG)
		return -1;
	if (image_exec_match_suppressions(ei, suppress_image_exec_by_ident,
	                                      suppress_image_exec_by_path))
		return -1;
	return 0;
}

/*
 * Create new image_exec from pid using runtime lookups.
 */
static image_exec_t *
image_exec_from_pid(pid_t pid) {
	image_exec_t *ei;
	char *path;
	int nopath = 0;
	int rv;

	path = sys_pidpath(pid);
#ifdef DEBUG_PROCMON
	DEBUG(config->debug, "image_exec_from_pid",
	      "pid=%i path=%s", pid, path);
#endif
	if (!path) {
		if (errno == ENOMEM) {
			atomic64_inc(&ooms);
			return NULL;
		}
		if (asprintf(&path, "<%i>", pid) == -1) {
			atomic64_inc(&ooms);
			free(path);
			return NULL;
		}
		nopath = 1;
	}
	ei = image_exec_new(path);
	if (!ei)
		return NULL;
	rv = timespec_nanotime(&ei->hdr.tv);
	if (rv == -1) {
		image_exec_free(ei);
		return NULL;
	}
	if (nopath)
		ei->flags |= EIFLAG_NOPATH;
	ei->flags |= EIFLAG_PIDLOOKUP;
	ei->pid = pid;
	return ei;
}

/*
 * Create new proc from pid using runtime lookups.  Called after looking up a
 * subject in proctab fails and for examination of processes which executed
 * before xnumon.
 *
 * Returns NULL on oom or if the process is not running anymore.
 *
 * Does oom counting, caller does not need to.
 * However, caller needs to count and report miss if this fails.
 */
static proc_t *
procmon_proc_from_pid(pid_t pid, bool log_event, struct timespec *tv) {
	proc_t *proc;
	pid_t ppid;

	proc = proctab_find_or_create(pid);
	if (!proc) {
		atomic64_inc(&ooms);
		return NULL;
	}

	if (sys_pidbsdinfo(&proc->fork_tv, &ppid, pid) == -1) {
		/* process not alive anymore */
		proctab_remove(pid, tv);
		return NULL;
	}

	if (proc->cwd) {
		free(proc->cwd);
	}
	proc->cwd = sys_pidcwd(pid);
	if (!proc->cwd) {
		if (errno == ENOMEM)
			atomic64_inc(&ooms);
		/* process not alive anymore unless ENOMEM */
		proctab_remove(pid, tv);
		return NULL;
	}

	if (proc->image_exec) {
		image_exec_free(proc->image_exec);
	}
	proc->image_exec = image_exec_from_pid(pid);
	if (!proc->image_exec) {
		/* process not alive anymore unless ENOMEM */
		proctab_remove(pid, tv);
		return NULL;
	}
	image_exec_open(proc->image_exec, NULL);

	/* after acquiring all info from process, go after parent before
	 * submitting the child into the queues */
	if ((ppid >= 0) && (ppid != pid)) {
		proc_t *pproc = proctab_find(ppid);
		if (!pproc) {
			pproc = procmon_proc_from_pid(ppid, log_event, tv);
			if (!pproc) {
				if (errno == ENOMEM) {
					proctab_remove(pid, tv);
					return NULL;
				}
				/* parent not alive anymore */
				ppid = -1;
			}
		}
		if (pproc) {
			proc->image_exec->prev = pproc->image_exec;
			if (proc->image_exec->prev) {
				image_exec_ref(proc->image_exec->prev);
			}
		}
	}

	if (!log_event || pid == 0)
		proc->image_exec->flags |= EIFLAG_NOLOG;
#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: work_submit(%p)\n",
	                proc->image_exec);
#endif
	image_exec_ref(proc->image_exec); /* ref is owned by proc */
	work_submit(proc->image_exec);
	return proc;
}

/*
 * Retrieve the current executable image for a given pid.
 * Intended to be called from other subsystems when logging process context
 * for an event related to a pid.
 * Caller must free the returned image_exec_t with image_exec_free().
 * On error returns NULL.
 * Not thread-safe - must be called from the main thread, not worker or logger!
 *
 * Caller does error counting and reporting.
 */
image_exec_t *
image_exec_by_pid(pid_t pid, struct timespec *tv) {
	proc_t *proc;

	proc = proctab_find(pid);
	if (!proc) {
		proc = procmon_proc_from_pid(pid, true, tv);
		if (!proc) {
			if (errno != ENOMEM) {
				miss_bypid++;
				DEBUG(config->debug, "miss_bypid",
				      "pid=%i", pid);
			}
			return NULL;
		}
		liveacq++;
	}
	image_exec_ref(proc->image_exec);
	return proc->image_exec;
}

/*
 * Handles fork.
 */
void
procmon_fork(struct timespec *tv,
             audit_proc_t *subject, pid_t childpid) {
	proc_t *parent, *child;

#ifdef DEBUG_PROCMON
	DEBUG(config->debug, "procmon_fork",
	      "subject->pid=%i childpid=%i\n",
	      subject->pid, childpid);
#endif

	parent = proctab_find(subject->pid);
	if (!parent) {
		parent = procmon_proc_from_pid(subject->pid, true, tv);
		if (!parent) {
			if (errno != ENOMEM) {
				miss_forksubj++;
				DEBUG(config->debug, "miss_forksubj",
				      "subject.pid=%i childpid=%i",
				      subject->pid, childpid);
			}
			return;
		}
		liveacq++;
	}
	assert(parent);

	proctab_remove(childpid, tv);
	child = proctab_create(childpid);
	if (!child) {
		atomic64_inc(&ooms);
		return;
	}
	child->fork_tv = *tv;

	assert(parent->cwd);
	child->cwd = strdup(parent->cwd);
	if (!child->cwd) {
		proctab_remove(childpid, tv);
		atomic64_inc(&ooms);
		return;
	}

	assert(parent->image_exec);
	child->image_exec = parent->image_exec;
	image_exec_ref(child->image_exec);
}

/*
 * Only handles true posix_spawn without the POSIX_SPAWN_SETEXEC attribute set.
 * POSIX_SPAWN_SETEXEC is treated as regular exec.
 *
 * Ownership of argv and imagepath is transfered; procmon guarantees that they
 * will be freed.
 */
void
procmon_spawn(struct timespec *tv,
              audit_proc_t *subject,
              pid_t childpid,
              char *imagepath, audit_attr_t *attr,
              char **argv, char **envv) {
#ifdef DEBUG_PROCMON
	DEBUG(config->debug, "procmon_spawn",
	      "subject->pid=%i childpid=%i imagepath=%s",
	      subject->pid, childpid, imagepath);
#endif

	procmon_fork(tv, subject, childpid);
	subject->pid = childpid;
	procmon_exec(tv, subject, imagepath, attr, argv, envv);
}

/*
 * Append an element to the prepq.
 * Called from the kext event handler, if kextlevel is > 0.
 */
static void
prepq_append(image_exec_t *ei) {
	pthread_mutex_lock(&pqmutex);
	tommy_list_insert_tail(&pqlist, &ei->hdr.node, ei);
	pqsize++;
	pthread_mutex_unlock(&pqmutex);
}

/*
 * Remove an existing (!) element from the prepq.
 * Called from the exec handler only (!).
 *
 * The chosen locking strategy only works if only a single thread is removing
 * elements and one or more other threads are only adding elements.
 */
static void
prepq_remove_existing(image_exec_t *ei) {
	pthread_mutex_lock(&pqmutex);
	tommy_list_remove_existing(&pqlist,
	                           &ei->hdr.node);
	pqsize--;
	pthread_mutex_unlock(&pqmutex);
}

/*
 * Look up the corresponding exec images acquired by kext events before the
 * audit event was committed.  Linking the audit event to the correct kext
 * events even when events are being lost for some reason is probably the most
 * tricky part of all of this.
 */
static void
prepq_lookup(image_exec_t **image, image_exec_t **interp,
             proc_t *proc, char *imagepath, audit_attr_t *attr, char **argv) {
	*image = NULL;
	*interp = NULL;
	pqlookup++;
	for (tommy_node *node = tommy_list_head(&pqlist);
	     node; node = node->next) {
		image_exec_t *ei = node->data;
		assert(ei);

		if (!*image) {
			/*
			 * Find the image based on (pid,dev,ino) or
			 * (pid,basename(path)) as a fallback if no attr is
			 * available from the audit event.  When the kernel
			 * passes a wrong path to the audit framework, it does
			 * not provide attributes; in that case we have to rely
			 * on just the pid and the basename.
			 */
			if (ei->pid == proc->pid &&
			    ((attr && ei->stat.dev == attr->dev &&
			              ei->stat.ino == attr->ino) ||
			     (!attr &&
			      !sys_basenamecmp(ei->path, imagepath)))) {
				/* we have a match */
				prepq_remove_existing(ei);
				*image = ei;
				/* script executions always have the
				 * interpreter as argv[0] and the script file
				 * as argv[1].  The remaining arguments are the
				 * arguments passed to the scripts, if any */
				if (((*image)->flags & EIFLAG_SHEBANG) &&
				    argv && argv[0] && argv[1])
					continue;
				break;
			}
		} else {
			assert(!*interp);
			assert(argv && argv[0] && argv[1]);
			/* #! can be relative path and we have no attr now.
			 * Using (pid,basename(path)) is the best we can do
			 * at this point. */
			if (ei->pid == proc->pid &&
			    !sys_basenamecmp(ei->path, argv[0])) {
				/* we have a match */
				prepq_remove_existing(ei);
				*interp = ei;
				break;
			}
		}

		pqskip++;
#if 0
		DEBUG(config->debug, "prepq_skip",
		      "looking for %s[%i]: skipped %s[%i]",
		      imagepath, proc->pid, ei->path, ei->pid);
#endif
		if (++ei->pqttl == MAXPQTTL) {
			DEBUG(config->debug, "prepq_drop",
			      "looking for %s[%i]: dropped %s[%i]",
			      imagepath, proc->pid, ei->path, ei->pid);
			prepq_remove_existing(ei);
			image_exec_free(ei);
			pqdrop++;
		}
	}
	assert(!(*interp && !*image));
}

/*
 * For scripts, this will be called once, with argv[0] as the interpreter and
 * argv[1+] as argv[0+] of the script execution, imagepath as the script and
 * attr as the file attributes of the script.
 *
 * Ownership of argv and imagepath is transfered, procmon guarantees that they
 * will be freed.  Only argv and attr can be NULL.
 */
void
procmon_exec(struct timespec *tv,
             audit_proc_t *subject,
             char *imagepath, audit_attr_t *attr,
             char **argv, char **envv) {
	proc_t *proc;
	image_exec_t *prev_image_exec;
	char *cwd;

#ifdef DEBUG_PROCMON
	DEBUG(config->debug, "procmon_exec",
	      "subject->pid=%i imagepath=%s", subject->pid, imagepath);
#endif

	proc = proctab_find(subject->pid);
	if (!proc) {
		proc = procmon_proc_from_pid(subject->pid, true, tv);
		if (!proc) {
			if (errno != ENOMEM) {
				miss_execsubj++;
				DEBUG(config->debug, "miss_execsubj",
				      "subject.pid=%i imagepath=%s argv[0]=%s",
				      subject->pid, imagepath,
				      argv ? argv[0] : NULL);
			}
			free(imagepath);
			if (argv)
				free(argv);
			if (envv)
				free(envv);
			return;
		}
		liveacq++;
	}
	assert(proc);

	image_exec_t *image, *interp;
	prepq_lookup(&image, &interp, proc, imagepath, attr, argv);

#if 0
	if (image)
		fprintf(stderr, "found kext image "
		                "pid=%i path=%s is_script=%i\n",
		                image->pid, image->path,
		                image->flags & EIFLAG_SHEBANG);
	if (interp)
		fprintf(stderr, "found kext interp "
		                "pid=%i path=%s is_script=%i\n",
		                interp->pid, interp->path,
		                interp->flags & EIFLAG_SHEBANG);
#endif

	if (!image) {
		DEBUG(config->debug && config->kextlevel > 0,
		      "prepq_miss",
		      "looking for %s[%i]: not found (image)",
		      imagepath, proc->pid);
		pqmiss++;
		image = image_exec_new(imagepath);
		if (!image) {
			/* no counter, oom is the only reason this can happen */
			if (argv)
				free(argv);
			if (envv)
				free(envv);
			assert(!interp);
			return;
		}
	} else {
		free(imagepath);
	}
	assert(image);
	image_exec_open(image, attr);

	/*
	 * XXX why are we not using the shebang from the script file here if
	 * argv is unavailable?
	 */
	if (image->flags & EIFLAG_SHEBANG) {
		if (!interp) {
			DEBUG(config->debug && config->kextlevel > 0,
			      "prepq_miss",
			      "looking for %s[%i]: not found (interp "
			      "argv[0]=%s)",
			      imagepath, proc->pid, argv ? argv[0] : NULL);
			pqmiss++;
			if (!argv) {
				miss_execinterp++;
				DEBUG(config->debug, "miss_execinterp",
				      "subject.pid=%i imagepath=%s "
				      "argv=NULL attr:%s",
				      subject->pid, imagepath,
				      attr ? "y" : "n");
				image_exec_free(image);
				if (envv)
					free(envv);
				return;
			}
			if (argv[0][0] == '/' || proc->cwd) {
				char *p = sys_realpath(argv[0], proc->cwd);
				if (!p) {
					if (errno == ENOMEM)
						atomic64_inc(&ooms);
					miss_execinterp++;
					DEBUG(config->debug,
					      "miss_execinterp",
					      "subject.pid=%i imagepath=%s "
					      "argv[0]=%s argv[1]=%s "
					      "attr:%s",
					      subject->pid, imagepath,
					      argv[0], argv[1],
					      attr ? "y" : "n");
					image_exec_free(image);
					free(argv);
					if (envv)
						free(envv);
					return;
				}
				interp = image_exec_new(p);
			}
			if (!interp) {
				miss_execinterp++;
				DEBUG(config->debug, "miss_execinterp",
				      "subject.pid=%i imagepath=%s "
				      "argv[0]=%s argv[1]=%s attr:%s",
				      subject->pid, imagepath,
				      argv[0], argv[1],
				      attr ? "y" : "n");
				image_exec_free(image);
				free(argv);
				if (envv)
					free(envv);
				return;
			}
		}
		assert(interp);
		image_exec_open(interp, NULL);
	}

	/* replace the process' executable image */
	prev_image_exec = proc->image_exec;
	if (image->flags & EIFLAG_SHEBANG) {
		proc->image_exec = interp;
		proc->image_exec->script = image;
	} else {
		proc->image_exec = image;
	}
	assert(proc->image_exec);
	assert(proc->image_exec != prev_image_exec);
	cwd = strdup(proc->cwd);
	if (!cwd) {
		atomic64_inc(&ooms);
		image_exec_free(proc->image_exec);
		proc->image_exec = NULL;
		/* free what would have been transfered to image_exec below */
		if (prev_image_exec)
			image_exec_free(prev_image_exec);
		if (argv)
			free(argv);
		if (envv)
			free(envv);
		return;
	}
	assert(proc->image_exec->refs == 1);
	proc->image_exec->hdr.tv = *tv;
	proc->image_exec->fork_tv = proc->fork_tv;
	proc->image_exec->pid = proc->pid;
	proc->image_exec->subject = *subject;
	proc->image_exec->argv = argv;
	proc->image_exec->envv = envv;
	proc->image_exec->cwd = cwd;
	proc->image_exec->prev = prev_image_exec;

	if (proc->image_exec->prev->flags & EIFLAG_NOLOG_KIDS)
		proc->image_exec->flags |= EIFLAG_NOLOG | EIFLAG_NOLOG_KIDS;
	else if (image_exec_match_suppressions(proc->image_exec,
				suppress_image_exec_by_ancestor_ident,
				suppress_image_exec_by_ancestor_path))
		proc->image_exec->flags |= EIFLAG_NOLOG_KIDS;

#ifdef DEBUG_REFS
	fprintf(stderr, "DEBUG_REFS: work_submit(%p)\n",
	                proc->image_exec);
#endif
	image_exec_ref(proc->image_exec); /* ref is owned by proc */
	work_submit(proc->image_exec);
}

/*
 * Called from both EXIT and WAIT4 events because EXIT is only triggered for
 * actual calls to exit(), not for process termination e.g. as a result of
 * signal().  As a result, this routine needs to handle multiple calls per
 * process, ideally with little overhead.  In all cases, the process is already
 * gone and lookups of current process state would be useless here.
 */
void
procmon_exit(struct timespec *tv, pid_t pid) {
#ifdef DEBUG_EXIT
	DEBUG(config->debug, "procmon_exit",
	      "pid=%i", pid);
#endif

	proctab_remove(pid, tv);
}

/*
 * We use wait4 to catch processes that terminated without calling exit().
 * Because wait4 returns for processes that were terminated as well as for
 * processes that were suspended, we have to check the validity of the pid.
 * If the process does not exist at this time, we remove it from our state.
 *
 * This code requires root privileges.
 */
void
procmon_wait4(struct timespec *tv, pid_t pid) {
	int rv;

#ifdef DEBUG_EXIT
	DEBUG(config->debug, "procmon_wait4",
	      "pid=%i", pid);
#endif

	if ((pid == -1) || (pid == 0))
		return;

	rv = kill(pid, 0);
	if ((rv == -1) && (errno == ESRCH))
		procmon_exit(tv, pid);
}

/*
 * CWD tracking is only needed in order to reconstruct full paths to relative
 * interpreter paths in shebangs.
 *
 * Path will be freed within procmon and must not be further used by the caller
 * after calling this function.
 */
void
procmon_chdir(struct timespec *tv, pid_t pid, char *path) {
	proc_t *proc;

#ifdef DEBUG_CHDIR
	DEBUG(config->debug, "procmon_chdir",
	      "pid=%i path=%s", pid, path);
#endif

	proc = proctab_find(pid);
	if (!proc) {
		proc = procmon_proc_from_pid(pid, true, tv);
		if (!proc) {
			if (errno != ENOMEM) {
				miss_chdirsubj++;
				DEBUG(config->debug, "miss_chdirsubj",
				      "pid=%i path=%s", pid, path);
			}
			free(path);
			return;
		}
		liveacq++;
	}
	assert(proc);

	if (proc->cwd)
		free(proc->cwd);
	proc->cwd = path;
}

/*
 * Called while the kernel is waiting for our KAuth verdict.
 *
 * For scripts, this will be called first for the script, then for the
 * interpreter.
 *
 * Unlike other procmon functions, imagepath will NOT be owned by procmon and
 * remains owned by the caller.
 */
void
procmon_kern_preexec(struct timespec *tm, pid_t pid, const char *imagepath) {
	image_exec_t *ei;
	char *path;

#ifdef DEBUG_PROCMON
	DEBUG(config->debug, "procmon_kern_preexec",
	      "pid=%i imagepath=%s", pid, imagepath);
#endif

	path = strdup(imagepath);
	if (!path) {
		atomic64_inc(&ooms);
		return;
	}

	ei = image_exec_new(path);
	if (!ei)
		return;
	ei->hdr.tv = *tm;
	ei->pid = pid;
	image_exec_open(ei, NULL);
	image_exec_acquire(ei, true);
	prepq_append(ei);
}

/*
 * Preload the process context information for pid.
 *
 * The procmon code base should actually work without any preloading too.
 * Main difference is that for processes recovered later, image exec events
 * are always logged, while for preloaded processes, the logging can be
 * configured, but is suppressed by default.
 */
void
procmon_preloadpid(pid_t pid) {
	proc_t *proc;

	proc = proctab_find(pid);
	if (proc)
		/* pid was already loaded as an ancestor of a previous call */
		return;
	(void)procmon_proc_from_pid(pid, !config->suppress_image_exec_at_start,
	                            NULL);
}

/*
 * Return the stored current working directory for a process by pid.
 * Caller must not free the string.
 */
const char *
procmon_getcwd(pid_t pid, struct timespec *tv) {
	proc_t *proc;

	proc = proctab_find(pid);
	if (!proc) {
		proc = procmon_proc_from_pid(pid, true, tv);
		if (!proc) {
			if (errno != ENOMEM) {
				miss_getcwd++;
				DEBUG(config->debug, "miss_getcwd",
				      "pid=%i", pid);
			}
			return NULL;
		}
		liveacq++;
	}
	return proc->cwd;
}

/*
 * Called from sockmon to create socket context on a process.
 *
 * Can silently fail.
 */
void
procmon_socket_create(pid_t pid, int fd,
                      int proto) {
	fd_ctx_t *ctx;
	proc_t *proc;

	proc = proctab_find(pid);
	if (!proc)
		return; // XXX count

	ctx = proc_getfd(proc, fd);
	if (ctx) {
		/* reuse existing allocation */
		bzero(((char *)ctx)+sizeof(ctx->node),
		      sizeof(fd_ctx_t)-sizeof(ctx->node));
		ctx->fd = fd;
	} else {
		ctx = malloc(sizeof(fd_ctx_t));
		if (!ctx) {
			atomic64_inc(&ooms);
			return;
		}
		bzero(ctx, sizeof(fd_ctx_t));
		ctx->fd = fd;
		proc_setfd(proc, ctx);
	}
	ctx->flags = FDFLAG_SOCKET;
	ctx->so.proto = proto;
}

/*
 * Called from sockmon to bind a local socket address to a socket and
 * return the protocol that we stored from the earlier call to
 * procmon_socket_create().
 *
 * Returns proto = 0 if no state available on this socket.
 */
void
procmon_socket_bind(int *proto,
                    pid_t pid, int fd,
                    ipaddr_t *addr, uint16_t port) {
	proc_t *proc;
	fd_ctx_t *ctx;

	proc = proctab_find(pid);
	if (!proc)
		goto errout;
	ctx = proc_getfd(proc, fd);
	if (!ctx || !(ctx->flags & FDFLAG_SOCKET))
		goto errout;
	ctx->so.addr = *addr;
	ctx->so.port = port;
	*proto = ctx->so.proto;
	return;

errout:
	*proto = 0;
}

/*
 * Called from sockmon to retrieve socket state stored from previous calls
 * to procmon_socket_create() and procmon_socket_bind().
 *
 * Returns proto = 0, addr = NULL and undefined port if no state is available.
 * The buffer containing ipaddr_t must be copied by the caller.
 */
void
procmon_socket_state(int *proto, ipaddr_t **addr, uint16_t *port,
                     pid_t pid, int fd) {
	proc_t *proc;
	fd_ctx_t *ctx;

	proc = proctab_find(pid);
	if (!proc)
		goto errout;
	ctx = proc_getfd(proc, fd);
	if (!ctx || !(ctx->flags & FDFLAG_SOCKET))
		goto errout;
	if (ipaddr_is_empty(&ctx->so.addr)) {
		*addr = NULL;
	} else {
		*addr = &ctx->so.addr;
		*port = ctx->so.port;
	}
	*proto = ctx->so.proto;
	return;

errout:
	*addr = NULL;
	*proto = 0;
}

void
procmon_file_open(audit_proc_t *subject, int fd, char *path) {
	proc_t *proc;
	fd_ctx_t *ctx;

	proc = proctab_find(subject->pid);
	if (!proc) /* XXX create from pid?! */
		return;

	ctx = proc_getfd(proc, fd);
	if (ctx) {
		/* reuse existing allocation */
		bzero(((char *)ctx)+sizeof(ctx->node),
		      sizeof(fd_ctx_t)-sizeof(ctx->node));
		ctx->fd = fd;
	} else {
		ctx = malloc(sizeof(fd_ctx_t));
		if (!ctx) {
			atomic64_inc(&ooms);
			return;
		}
		bzero(ctx, sizeof(fd_ctx_t));
		ctx->fd = fd;
		proc_setfd(proc, ctx);
	}
	ctx->flags = FDFLAG_FILE;
	ctx->fi.subject = *subject;
	ctx->fi.path = strdup(path);
	if (!ctx->fi.path) {
		atomic64_inc(&ooms);
	}
}

void
procmon_fd_close(pid_t pid, int fd) {
	proc_t *proc;

	proc = proctab_find(pid);
	if (!proc)
		return;
	fd_ctx_t *ctx = proc_closefd(proc, fd);
	if (ctx) {
		proc_freefd(ctx);
	}
}

int
procmon_init(config_t *cfg) {
	proctab_init();
	config = cfg;
	images = 0;
	miss_bypid = 0;
	miss_forksubj = 0;
	miss_execsubj = 0;
	miss_execinterp = 0;
	miss_chdirsubj = 0;
	miss_getcwd = 0;
	ooms = 0;
	pqlookup = 0;
	pqmiss = 0;
	pqdrop = 0;
	pqskip = 0;
	pqsize = 0;
	tommy_list_init(&pqlist);
	pthread_mutex_init(&pqmutex, NULL);
	suppress_image_exec_by_ident = &cfg->suppress_image_exec_by_ident;
	suppress_image_exec_by_path = &cfg->suppress_image_exec_by_path;
	suppress_image_exec_by_ancestor_ident =
		&cfg->suppress_image_exec_by_ancestor_ident;
	suppress_image_exec_by_ancestor_path =
		&cfg->suppress_image_exec_by_ancestor_path;
	return 0;
}

void
procmon_fini(void) {
	if (!config)
		return;

	/* kext thread must be terminated before call to procmon_fini */
	pthread_mutex_destroy(&pqmutex);
	while (!tommy_list_empty(&pqlist)) {
		image_exec_t *ei;
		ei = tommy_list_remove_existing(&pqlist,
		                                tommy_list_head(&pqlist));
		image_exec_free(ei);
		pqsize--;
	}
	assert(pqsize == 0);
	proctab_fini();
	config = NULL;
}

void
procmon_stats(procmon_stat_t *st) {
	assert(st);

	st->procs = procs; /* external */
	st->images = (uint32_t)images;
	st->liveacq = liveacq;
	st->miss_bypid = miss_bypid;
	st->miss_forksubj = miss_forksubj;
	st->miss_execsubj = miss_execsubj;
	st->miss_execinterp = miss_execinterp;
	st->miss_chdirsubj = miss_chdirsubj;
	st->miss_getcwd = miss_getcwd;
	st->ooms = (uint64_t)ooms;
	st->pqlookup = pqlookup;
	st->pqmiss = pqmiss;
	st->pqdrop = pqdrop;
	st->pqskip = pqskip;
	st->pqsize = pqsize;
}

/*
 * Returns the number of exec images in existence.
 * Can be safely called after procmon_fini().
 */
uint32_t
procmon_images(void) {
	return (uint32_t)images;
}

