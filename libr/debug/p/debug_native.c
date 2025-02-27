/* radare - LGPL - Copyright 2009-2019 - pancake */

#include <r_userconf.h>
#include <r_debug.h>
#include <r_drx.h>
#include <r_asm.h>
#include <r_core.h>
#include <r_reg.h>
#include <r_lib.h>
#include <r_anal.h>
#include <signal.h>
#include <sys/types.h>

#if DEBUGGER

#include "native/drx.c" // x86 specific
#include "r_cons.h"

static int r_debug_native_continue (RDebug *dbg, int pid, int tid, int sig);
static int r_debug_native_reg_read (RDebug *dbg, int type, ut8 *buf, int size);
static int r_debug_native_reg_write (RDebug *dbg, int type, const ut8* buf, int size);

#include "native/bt.c"

#if __UNIX__
# include <errno.h>
# if !defined (__HAIKU__) && !defined (__sun)
#  include <sys/ptrace.h>
# endif
# include <sys/wait.h>
# include <signal.h>
#endif

#if __WINDOWS__
//#include <windows.h>
#include "native/windows/windows_debug.h"
// TODO: Move these onto windows.h?
R_API RList *r_w32_dbg_modules(RDebug *); //ugly!
R_API RList *r_w32_dbg_maps(RDebug *);
#define R_DEBUG_REG_T CONTEXT
#ifdef NTSTATUS
#undef NTSTATUS
#endif
#ifndef NTSTATUS
#define NTSTATUS int
#endif

#elif __BSD__
#include <sys/sysctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <kvm.h>
#include <limits.h>
#define R_DEBUG_REG_T struct reg
#include "native/procfs.h"
#if __KFBSD__ || __DragonFly__
#include <sys/user.h>
#include <libutil.h>
#elif __OpenBSD__ || __NetBSD__
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#include "native/procfs.h"

#elif __APPLE__
#include <sys/resource.h>
#include "native/xnu/xnu_debug.h"

#elif __sun

# define R_DEBUG_REG_T gregset_t
# undef DEBUGGER
# define DEBUGGER 0
# warning No debugger support for SunOS yet

#elif __linux__
#include <sys/mman.h>
#include "native/linux/linux_debug.h"
#include "native/procfs.h"
# ifdef __ANDROID__
#  define WAIT_ANY -1
#  ifndef WIFCONTINUED
#   define WIFCONTINUED(s) ((s) == 0xffff)
#  endif
# endif
#if (__x86_64__ || __i386__ || __arm__ || __arm64__) && !defined(__ANDROID__)
#include "native/linux/linux_coredump.h"
#endif
#else // OS

#warning Unsupported debugging platform
#undef DEBUGGER
#define DEBUGGER 0
#endif // ARCH

#ifdef __WALL
#define WAITPID_FLAGS __WALL
#else
#define WAITPID_FLAGS 0
#endif

#endif /* IF DEBUGGER */

/* begin of debugger code */
#if DEBUGGER

#if __WINDOWS__ || (!__APPLE__ && defined(WAIT_ON_ALL_CHILDREN))
static int r_debug_handle_signals(RDebug *dbg) {
#if __linux__
	return linux_handle_signals (dbg);
#elif __KFBSD__
#else
	return -1;
#endif
}
#endif

//this is temporal
#if __APPLE__ || __linux__

static char *r_debug_native_reg_profile (RDebug *dbg) {
#if __APPLE__
	return xnu_reg_profile (dbg);
#elif __linux__
	return linux_reg_profile (dbg);
#endif
}
#else

#include "native/reg.c" // x86 specific

#endif
static int r_debug_native_step (RDebug *dbg) {
#if __APPLE__
	return xnu_step (dbg);
#elif __WINDOWS__
	return w32_step (dbg);
#elif __BSD__
	int ret = ptrace (PT_STEP, dbg->pid, (caddr_t)1, 0);
	if (ret != 0) {
		perror ("native-singlestep");
		return false;
	}
	return true;
#else // linux
	return linux_step (dbg);
#endif
}

// return thread id
static int r_debug_native_attach (RDebug *dbg, int pid) {
#if 0
	if (!dbg || pid == dbg->pid)
		return dbg->tid;
#endif
#if __APPLE__
	return xnu_attach (dbg, pid);
#elif __WINDOWS__
	return w32_attach (dbg, pid);
#elif __linux__ || __ANDROID__
	return linux_attach (dbg, pid);
#elif __KFBSD__
	if (ptrace (PT_ATTACH, pid, 0, 0) != -1) {
		perror ("ptrace (PT_ATTACH)");
	}
	return pid;
#else
	int ret = ptrace (PTRACE_ATTACH, pid, 0, 0);
	if (ret != -1) {
		eprintf ("Trying to attach to %d\n", pid);
		perror ("ptrace (PT_ATTACH)");
	}
	return pid;
#endif
}

static int r_debug_native_detach (RDebug *dbg, int pid) {
#if __APPLE__
	return xnu_detach (dbg, pid);
#elif __WINDOWS__
	return w32_detach (dbg, pid);
#elif __BSD__
	return ptrace (PT_DETACH, pid, NULL, 0);
#else
	return r_debug_ptrace (dbg, PTRACE_DETACH, pid, NULL, (r_ptrace_data_t)(size_t)0);
#endif
}

static int r_debug_native_select(RDebug *dbg, int pid, int tid) {
#if __WINDOWS__
	return w32_select (dbg, pid, tid);
#elif __linux__
	return linux_select_thread (dbg, pid, tid);
#else
	return -1;
#endif
}

static int r_debug_native_continue_syscall (RDebug *dbg, int pid, int num) {
// XXX: num is ignored
#if __linux__
	linux_set_options (dbg, pid);
	return r_debug_ptrace (dbg, PTRACE_SYSCALL, pid, 0, 0);
#elif __BSD__
	ut64 pc = r_debug_reg_get (dbg, "PC");
	errno = 0;
	return ptrace (PTRACE_SYSCALL, pid, (void*)(size_t)pc, 0) == 0;
#else
	eprintf ("TODO: continue syscall not implemented yet\n");
	return -1;
#endif
}

#if !__WINDOWS__ && !__APPLE__ && !__BSD__
/* Callback to trigger SIGINT signal */
static void r_debug_native_stop(RDebug *dbg) {
	r_debug_kill (dbg, dbg->pid, dbg->tid, SIGINT);
	r_cons_break_pop ();
}
#endif

/* TODO: specify thread? */
/* TODO: must return true/false */
static int r_debug_native_continue(RDebug *dbg, int pid, int tid, int sig) {
#if __APPLE__
	bool ret = xnu_continue (dbg, pid, tid, sig);
	if (!ret) {
		return -1;
	}
	return tid;
#elif __WINDOWS__
	return w32_continue (dbg, pid, tid, sig);
#elif __BSD__
	void *data = (void*)(size_t)((sig != -1) ? sig : dbg->reason.signum);
	ut64 pc = r_debug_reg_get (dbg, "PC");
	return ptrace (PTRACE_CONT, pid, (void*)(size_t)pc, (int)(size_t)data) == 0;
#else
	int contsig = dbg->reason.signum;
	int ret = -1;

	if (sig != -1) {
		contsig = sig;
	}
	/* SIGINT handler for attached processes: dbg.consbreak (disabled by default) */
	if (dbg->consbreak) {
		r_cons_break_push ((RConsBreak)r_debug_native_stop, dbg);
	}

	if (dbg->continue_all_threads && dbg->n_threads) {
		RList *list = dbg->threads;
		RDebugPid *th;
		RListIter *it;

		if (list) {
			r_list_foreach (list, it, th) {
				if (th->pid) {
					ret = r_debug_ptrace (dbg, PTRACE_CONT, th->pid, NULL, (r_ptrace_data_t)(size_t)contsig);
					if (ret) {
						perror ("PTRACE_CONT");
					}
				}
			}
		}
	}
	else {
		ret = r_debug_ptrace (dbg, PTRACE_CONT, tid, NULL, (r_ptrace_data_t)(size_t)contsig);
		if (ret) {
			perror ("PTRACE_CONT");
		}
	}
	//return ret >= 0 ? tid : false;
	return tid;
#endif
}

static RDebugInfo* r_debug_native_info (RDebug *dbg, const char *arg) {
#if __APPLE__
	return xnu_info (dbg, arg);
#elif __WINDOWS__
	return w32_info (dbg, arg);
#elif __linux__
	return linux_info (dbg, arg);
#elif __KFBSD__
	struct kinfo_proc *kp;
	RDebugInfo *rdi = R_NEW0 (RDebugInfo);
	if (!rdi) {
		return NULL;
	}

	if (!(kp = kinfo_getproc (dbg->pid))) {
		free (rdi);
		return NULL;
	}

	rdi->pid = dbg->pid;
	rdi->tid = dbg->tid;
	rdi->uid = kp->ki_uid;
	rdi->gid = kp->ki_pgid;
	rdi->exe = strdup (kp->ki_comm);

	switch (kp->ki_stat) {
		case SSLEEP:
			rdi->status = R_DBG_PROC_SLEEP;
			break;
		case SSTOP:
			rdi->status = R_DBG_PROC_STOP;
			break;
		case SZOMB:
			rdi->status = R_DBG_PROC_ZOMBIE;
			break;
		case SRUN:
		case SIDL:
		case SLOCK:
		case SWAIT:
			rdi->status = R_DBG_PROC_RUN;
			break;
		default:
			rdi->status = R_DBG_PROC_DEAD;
	}

	free (kp);

	return rdi;
#elif __OpenBSD__
	struct kinfo_proc *kp;
	char err[_POSIX2_LINE_MAX];
	int rc;
	RDebugInfo *rdi = R_NEW0 (RDebugInfo);
	if (!rdi) {
		return NULL;
	}

	kvm_t *kd = kvm_openfiles (NULL, NULL, NULL, KVM_NO_FILES, err);
	if (!kd) {
		free (rdi);
		return NULL;
	}

	kp = kvm_getprocs (kd, KERN_PROC_PID, dbg->pid, sizeof (*kp), &rc);
	if (kp) {
		rdi->pid = dbg->pid;
		rdi->tid = dbg->tid;
		rdi->uid = kp->p_uid;
		rdi->gid = kp->p__pgid;
		rdi->exe = strdup (kp->p_comm);

		rdi->status = R_DBG_PROC_STOP;

		if (kp->p_psflags & PS_ZOMBIE) {
				rdi->status = R_DBG_PROC_ZOMBIE;
		} else if (kp->p_psflags & PS_STOPPED){
				rdi->status = R_DBG_PROC_STOP;
		} else if (kp->p_psflags & PS_PPWAIT) {
				rdi->status = R_DBG_PROC_SLEEP;
		} else if ((kp->p_psflags & PS_EXEC) || (kp->p_psflags & PS_INEXEC)) {
				rdi->status = R_DBG_PROC_RUN;
		}

	}

	kvm_close (kd);

	return rdi;
#elif __NetBSD__
	struct kinfo_proc2 *kp;
	char err[_POSIX2_LINE_MAX];
	int np;
	RDebugInfo *rdi = R_NEW0 (RDebugInfo);
	if (!rdi) {
		return NULL;
	}

	kvm_t *kd = kvm_openfiles (NULL, NULL, NULL, KVM_NO_FILES, err);
	if (!kd) {
		free (rdi);
		return NULL;
	}

	kp = kvm_getproc2 (kd, KERN_PROC_PID, dbg->pid, sizeof(*kp), &np);
	if (kp) {
		rdi->pid = dbg->pid;
		rdi->tid = dbg->tid;
		rdi->uid = kp->p_uid;
		rdi->gid = kp->p__pgid;
		rdi->exe = strdup (kp->p_comm);

		rdi->status = R_DBG_PROC_STOP;

		switch (kp->p_stat) {
			case SDEAD:
				rdi->status = R_DBG_PROC_DEAD;
				break;
			case SSTOP:
				rdi->status = R_DBG_PROC_STOP;
				break;
			case SZOMB:
				rdi->status = R_DBG_PROC_ZOMBIE;
				break;
			case SACTIVE:
			case SIDL:
			case SDYING:
				rdi->status = R_DBG_PROC_RUN;
				break;
			default:
				rdi->status = R_DBG_PROC_SLEEP;
		}
	}

	kvm_close (kd);

	return rdi;
#else
	return NULL;
#endif
}

#if __WINDOWS__
static bool tracelib(RDebug *dbg, const char *mode, PLIB_ITEM item) {
	const char *needle = NULL;
	int tmp = 0;
	if (mode) {
		switch (mode[0]) {
		case 'l': needle = dbg->glob_libs; break;
		case 'u': needle = dbg->glob_unlibs; break;
		}
	}
	//eprintf ("(%d) %sing library at %p (%s) %s\n", item->pid, mode,
		//item->BaseOfDll, item->Path, item->Name);
	r_cons_printf ("(%d) %sing library at %p (%s) %s\n", item->pid, mode,
		item->BaseOfDll, item->Path, item->Name);
	r_cons_flush ();
	if (needle && strlen (needle)) {
		tmp = r_str_glob (item->Name, needle);
	}
	return !mode || !needle || tmp ;
}
#endif

/*
 * Wait for an event and start trying to figure out what to do with it.
 *
 * Returns R_DEBUG_REASON_*
 */
static RDebugReasonType r_debug_native_wait(RDebug *dbg, int pid) {
	RDebugReasonType reason = R_DEBUG_REASON_UNKNOWN;

#if __WINDOWS__
	// Store the original TID to attempt to switch back after handling events that
	// require switching to the event's thread that shouldn't bother the user
	int orig_tid = dbg->tid;
	bool restore_thread = false;
	RIOW32Dbg *rio = dbg->user;

	reason = w32_dbg_wait (dbg, pid);
	if (reason == R_DEBUG_REASON_NEW_LIB) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->lib) {
			if (tracelib (dbg, "load", r->lib)) {
				reason = R_DEBUG_REASON_TRAP;
			}

			/* Check if autoload PDB is set, and load PDB information if yes */
			RCore* core = dbg->corebind.core;
			bool autoload_pdb = dbg->corebind.cfggeti (core, "pdb.autoload");
			if (autoload_pdb) {
				dbg->corebind.cmdf (core, "o %s 0x%p", ((PLIB_ITEM)(r->lib))->Path, ((PLIB_ITEM)(r->lib))->BaseOfDll);
				char *o_res = dbg->corebind.cmdstrf (core, "o~+%s", ((PLIB_ITEM)(r->lib))->Path);
				int fd = atoi (o_res);
				free (o_res);
				char *pdb_path = dbg->corebind.cmdstr (core, "i~pdb");
				if (*pdb_path == 0) {
					eprintf ("Failure...\n");
					dbg->corebind.cmd (core, "i");
				} else {
					pdb_path = strchr (pdb_path, ' ') + 1;
					dbg->corebind.cmdf (core, "idp");
				}
				free (pdb_path);
				dbg->corebind.cmdf (core, "o-%d", fd);
			}
			r_debug_info_free (r);
		} else {
			//eprintf ("Loading unknown library.\n");
			r_cons_printf ("Loading unknown library.\n");
			r_cons_flush ();
		}
		restore_thread = true;
	} else if (reason == R_DEBUG_REASON_EXIT_LIB) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->lib) {
			if (tracelib (dbg, "unload", r->lib)) {
				reason = R_DEBUG_REASON_TRAP;
			}
			r_debug_info_free (r);
		} else {
			r_cons_printf ("Unloading unknown library.\n");
			r_cons_flush ();
		}
		restore_thread = true;
	} else if (reason == R_DEBUG_REASON_NEW_TID) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->thread) {
			PTHREAD_ITEM item = r->thread;
			r_cons_printf ("(%d) Created thread %d (start @ %p)\n", item->pid, item->tid, item->lpStartAddress);
			r_cons_flush ();

			r_debug_info_free (r);
		}
		restore_thread = true;
	} else if (reason == R_DEBUG_REASON_EXIT_TID) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->thread) {
			PTHREAD_ITEM item = r->thread;
			r_cons_printf ("(%d) Finished thread %d Exit code %d\n", (ut32)item->pid, (ut32)item->tid, (ut32)item->dwExitCode);
			r_cons_flush ();

			r_debug_info_free (r);
		}
		if (dbg->tid != orig_tid) {
			restore_thread = true;
		}
	} else if (reason == R_DEBUG_REASON_DEAD) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->thread) {
			PTHREAD_ITEM item = r->thread;
			r_cons_printf ("(%d) Finished process with exit code %d\n", dbg->main_pid, item->dwExitCode);
			r_cons_flush ();
			r_debug_info_free (r);
		}
		dbg->pid = -1;
		dbg->tid = -1;
	} else if (reason == R_DEBUG_REASON_USERSUSP && dbg->tid != orig_tid) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r && r->thread) {
			PTHREAD_ITEM item = r->thread;
			r_cons_printf ("(%d) Created DebugBreak thread %d (start @ %p)\n", item->pid, item->tid, item->lpStartAddress);
			r_cons_flush ();

			r_debug_info_free (r);
		}
		// DebugProcessBreak creates a new thread that will trigger a breakpoint. We record the
		// tid here to ignore it once the breakpoint is hit.
		rio->break_tid = dbg->tid;
		restore_thread = true;
	} else if (reason == R_DEBUG_REASON_BREAKPOINT && dbg->tid == rio->break_tid) {
		rio->break_tid = -2;
		reason = R_DEBUG_REASON_NONE;
		restore_thread = true;
	}

	if (restore_thread) {
		// Attempt to return to the original thread after handling the event
		dbg->tid = w32_select(dbg, dbg->pid, orig_tid);
		if (dbg->tid == -1) {
			dbg->pid = -1;
			reason = R_DEBUG_REASON_DEAD;
		} else {
			r_io_system (dbg->iob.io, sdb_fmt ("pid %d", dbg->tid));
			if (dbg->tid != orig_tid) {
				reason = R_DEBUG_REASON_UNKNOWN;
			}
		}
	}
#else
	if (pid == -1) {
		eprintf ("r_debug_native_wait called with -1 pid!\n");
		return R_DEBUG_REASON_ERROR;
	}

#if __APPLE__
	r_cons_break_push (NULL, NULL);
	do {
		reason = xnu_wait (dbg, pid);
		if (reason == R_DEBUG_REASON_MACH_RCV_INTERRUPTED) {
			if (r_cons_is_breaked ()) {
				// Perhaps check the inferior is still alive,
				// otherwise xnu_stop will fail.
				reason = xnu_stop (dbg, pid)
					? R_DEBUG_REASON_USERSUSP
					: R_DEBUG_REASON_UNKNOWN;
			} else {
				// Weird; we'll retry the wait.
				continue;
			}
		}
		break;
	} while (true);
	r_cons_break_pop ();
#else
#if __linux__ && !defined (WAIT_ON_ALL_CHILDREN)
	reason = linux_dbg_wait (dbg, dbg->tid);
	if (reason == R_DEBUG_REASON_NEW_TID) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r) {
			eprintf ("(%d) Created thread %d\n", r->pid, r->tid);
			r_debug_info_free (r);
		}
	} else if (reason == R_DEBUG_REASON_EXIT_TID) {
		RDebugInfo *r = r_debug_native_info (dbg, "");
		if (r) {
			eprintf ("(%d) Finished thread %d Exit code\n", r->pid, r->tid);
			r_debug_info_free (r);
		}
	}
#else
	int status = -1;
	// XXX: this is blocking, ^C will be ignored
#ifdef WAIT_ON_ALL_CHILDREN
	int ret = waitpid (-1, &status, WAITPID_FLAGS);
#else
	int ret = waitpid (-1, &status, 0);
	if (ret != -1) {
		reason = R_DEBUG_REASON_TRAP;
	}
#endif // WAIT_ON_ALL_CHILDREN
	if (ret == -1) {
		r_sys_perror ("waitpid");
		return R_DEBUG_REASON_ERROR;
	}

	//eprintf ("r_debug_native_wait: status=%d (0x%x) (return=%d)\n", status, status, ret);

#ifdef WAIT_ON_ALL_CHILDREN
	if (ret != pid) {
		reason = R_DEBUG_REASON_NEW_PID;
		eprintf ("switching to pid %d\n", ret);
		r_debug_select (dbg, ret, ret);
	}
#endif // WAIT_ON_ALL_CHILDREN
	// TODO: switch status and handle reasons here
#if __linux__ && defined(PT_GETEVENTMSG)
	reason = linux_ptrace_event (dbg, pid, status);
#endif // __linux__

	/* propagate errors */
	if (reason == R_DEBUG_REASON_ERROR) {
		return reason;
	}

	/* we don't know what to do yet, let's try harder to figure it out. */
#if __FreeBSD__
	if (reason == R_DEBUG_REASON_TRAP) {
#else
	if (reason == R_DEBUG_REASON_UNKNOWN) {
#endif
		if (WIFEXITED (status)) {
			eprintf ("child exited with status %d\n", WEXITSTATUS (status));
			reason = R_DEBUG_REASON_DEAD;
		} else if (WIFSIGNALED (status)) {
			eprintf ("child received signal %d\n", WTERMSIG (status));
			reason = R_DEBUG_REASON_SIGNAL;
		} else if (WIFSTOPPED (status)) {
			if (WSTOPSIG (status) != SIGTRAP &&
				WSTOPSIG (status) != SIGSTOP) {
				eprintf ("Child stopped with signal %d\n", WSTOPSIG (status));
			}

			/* the ptrace documentation says GETSIGINFO is only necessary for
			 * differentiating the various stops.
			 *
			 * this might modify dbg->reason.signum
			 */
#if __FreeBSD__ || __OpenBSD__ || __NetBSD__
			reason = R_DEBUG_REASON_BREAKPOINT;
#elif __KFBSD__
			// Trying to figure out a bit by the signal
			struct ptrace_lwpinfo linfo = {0};
			siginfo_t siginfo;
			int ret = ptrace (PT_LWPINFO, dbg->pid, (char *)&linfo, sizeof (linfo));
			if (ret == -1) {
				if (errno == ESRCH) {
					dbg->reason.type = R_DEBUG_REASON_DEAD;
					goto reason;
				}
				r_sys_perror ("ptrace PTRACCE_LWPINFO");
				return -1;
			} else {
				// Not stopped by the signal
				if (linfo.pl_event == PL_EVENT_NONE) {
					dbg->reason.type = R_DEBUG_REASON_BREAKPOINT;
					goto reason;
				}
			}

			siginfo = linfo.pl_siginfo;
			dbg->reason.type = R_DEBUG_REASON_SIGNAL;
			dbg->reason.signum = siginfo.si_signo;

			switch (dbg->reason.signum) {
				case SIGABRT:
					dbg->reason.type = R_DEBUG_REASON_ABORT;
					break;
				case SIGSEGV:
					dbg->reason.type = R_DEBUG_REASON_SEGFAULT;
					break;
			}

reason:
			reason = dbg->reason.type;
#else
			if (!r_debug_handle_signals (dbg)) {
				return R_DEBUG_REASON_ERROR;
			}
			reason = dbg->reason.type;
#endif
#ifdef WIFCONTINUED
		} else if (WIFCONTINUED (status)) {
			eprintf ("child continued...\n");
			reason = R_DEBUG_REASON_NONE;
#endif
		} else if (status == 1) {
			/* XXX(jjd): does this actually happen? */
			eprintf ("EEK DEAD DEBUGEE!\n");
			reason = R_DEBUG_REASON_DEAD;
		} else if (status == 0) {
			/* XXX(jjd): does this actually happen? */
			eprintf ("STATUS=0?!?!?!?\n");
			reason = R_DEBUG_REASON_DEAD;
		} else {
			if (ret != pid) {
				reason = R_DEBUG_REASON_NEW_PID;
			} else {
				/* ugh. still don't know :-/ */
				eprintf ("CRAP. returning from wait without knowing why...\n");
			}
		}
	}

	/* if we still don't know what to do, we have a problem... */
	if (reason == R_DEBUG_REASON_UNKNOWN) {
		eprintf ("%s: no idea what happened... wtf?!?!\n", __func__);
		reason = R_DEBUG_REASON_ERROR;
	}
#endif // __linux__ && !defined (WAIT_ON_ALL_CHILDREN)
#endif // __APPLE__
#endif // __WINDOWS__
	dbg->reason.tid = pid;
	dbg->reason.type = reason;
	return reason;
}

#undef MAXPID
#define MAXPID 99999

static RList *r_debug_native_tids (RDebug *dbg, int pid) {
	printf ("TODO: Threads: \n");
	// T
	return NULL;
}

static RList *r_debug_native_pids (RDebug *dbg, int pid) {
	RList *list = r_list_new ();
	if (!list) {
		return NULL;
	}
#if __APPLE__
	if (pid) {
		RDebugPid *p = xnu_get_pid (pid);
		if (p) {
			r_list_append (list, p);
		}
	} else {
		int i;
		for (i = 1; i < MAXPID; i++) {
			RDebugPid *p = xnu_get_pid (i);
			if (p) {
				r_list_append (list, p);
			}
		}
	}
#elif __WINDOWS__
	return w32_pid_list (dbg, pid, list);
#elif __linux__
	list->free = (RListFree)&r_debug_pid_free;
	DIR *dh;
	struct dirent *de;
	char *ptr, st, buf[1024];
	int i, uid;
	if (pid) {
		/* add the requested pid. should we do this? we don't even know if it's valid still.. */
		r_list_append (list, r_debug_pid_new ("(current)", pid, 0, 's', 0));
	}
	dh = opendir ("/proc");
	if (!dh) {
		r_sys_perror ("opendir /proc");
		r_list_free (list);
		return NULL;
	}
	while ((de = readdir (dh))) {
		uid = 0;
		st = ' ';
		/* for each existing pid file... */
		i = atoi (de->d_name);
		if (i <= 0) {
			continue;
		}

		/* try to read the status */
		buf[0] = 0;
		if (procfs_pid_slurp (i, "status", buf, sizeof (buf)) == -1) {
			continue;
		}
		buf[sizeof (buf) - 1] = 0;

		// get process State
		ptr = strstr (buf, "State:");
		if (ptr) {
			st = ptr[7];
		}
		/* look for the parent process id */
		ptr = strstr (buf, "PPid:");
		if (pid && ptr) {
			int ppid = atoi (ptr + 5);

			/* if this is the requested process... */
			if (i == pid) {
				// append it to the list with parent
				r_list_append (list, r_debug_pid_new (
					"(ppid)", ppid, uid, st, 0));
			}

			/* ignore it if it is not one of our children */
			if (ppid != pid) {
				continue;
			}
		}

		// get process Uid
		ptr = strstr (buf, "Uid:");
		if (ptr) {
			uid = atoi (ptr + 4);
		}
		// TODO: add support for gid in RDebugPid.new()
		// ptr = strstr (buf, "Gid:");
		// if (ptr) {
		// 	gid = atoi (ptr + 4);
		// }
		if (procfs_pid_slurp (i, "cmdline", buf, sizeof (buf)) == -1) {
			continue;
		}
		r_list_append (list, r_debug_pid_new (buf, i, uid, st, 0));
	}
	closedir (dh);
#else /* rest is BSD */
#ifdef __NetBSD__
# define KVM_OPEN_FLAG KVM_NO_FILES
# define KVM_GETPROCS(kd, opt, arg, cntptr) \
	kvm_getproc2 (kd, opt, arg, sizeof(struct kinfo_proc2), cntptr)
# define KP_COMM(x) (x)->p_comm
# define KP_PID(x) (x)->p_pid
# define KP_PPID(x) (x)->p_ppid
# define KP_UID(x) (x)->p_uid
# define KINFO_PROC kinfo_proc2
#elif defined(__OpenBSD__)
# define KVM_OPEN_FLAG KVM_NO_FILES
# define KVM_GETPROCS(kd, opt, arg, cntptr) \
	kvm_getprocs (kd, opt, arg, sizeof(struct kinfo_proc), cntptr)
# define KP_COMM(x) (x)->p_comm
# define KP_PID(x) (x)->p_pid
# define KP_PPID(x) (x)->p_ppid
# define KP_UID(x) (x)->p_uid
# define KINFO_PROC kinfo_proc
#elif __DragonFly__
# define KVM_OPEN_FLAG O_RDONLY
# define KVM_GETPROCS(kd, opt, arg, cntptr) \
	kvm_getprocs (kd, opt, arg, cntptr)
# define KP_COMM(x) (x)->kp_comm
# define KP_PID(x) (x)->kp_pid
# define KP_PPID(x) (x)->kp_ppid
# define KP_UID(x) (x)->kp_uid
# define KINFO_PROC kinfo_proc
#else
# define KVM_OPEN_FLAG O_RDONLY
# define KVM_GETPROCS(kd, opt, arg, cntptr) \
	kvm_getprocs (kd, opt, arg, cntptr)
# define KP_COMM(x) (x)->ki_comm
# define KP_PID(x) (x)->ki_pid
# define KP_PPID(x) (x)->ki_ppid
# define KP_UID(x) (x)->ki_uid
# define KINFO_PROC kinfo_proc
#endif
	char errbuf[_POSIX2_LINE_MAX];
	struct KINFO_PROC* kp;
	int cnt = 0;
	kvm_t* kd = kvm_openfiles (NULL, NULL, NULL, KVM_OPEN_FLAG, errbuf);
	if (!kd) {
		eprintf ("kvm_openfiles says %s\n", errbuf);
		return NULL;
	}
	if (pid) {
		kp = KVM_GETPROCS (kd, KERN_PROC_PID, pid, &cnt);
		if (cnt == 1) {
			RDebugPid *p = r_debug_pid_new (KP_COMM(kp), pid, KP_UID(kp), 's', 0);
			if (p) r_list_append (list, p);
			/* we got our process, now fetch the parent process */
			kp = KVM_GETPROCS (kd, KERN_PROC_PID, KP_PPID(kp), &cnt);
                        if (cnt == 1) {
				RDebugPid *p = r_debug_pid_new (KP_COMM(kp), KP_PID(kp), KP_UID(kp), 's', 0);
				if (p) r_list_append (list, p);
			}
		}
	} else {
		kp = KVM_GETPROCS (kd, KERN_PROC_UID, geteuid(), &cnt);
		int i;
		for (i = 0; i < cnt; i++) {
			RDebugPid *p = r_debug_pid_new (KP_COMM(kp + i), KP_PID(kp + i), KP_UID(kp), 's', 0);
			if (p) {
				r_list_append (list, p);
			}
		}
	}
	kvm_close(kd);
#endif
	return list;
}

static RList *r_debug_native_threads (RDebug *dbg, int pid) {
	RList *list = r_list_new ();
	if (!list) {
		eprintf ("No list?\n");
		return NULL;
	}
#if __APPLE__
	return xnu_thread_list (dbg, pid, list);
#elif __WINDOWS__
	return w32_thread_list (dbg, pid, list);
#elif __linux__
	return linux_thread_list (pid, list);
#else
	eprintf ("TODO: list threads\n");
	r_list_free (list);
	return NULL;
#endif
}

#if __sun || __NetBSD__ || __KFBSD__ || __OpenBSD__ || __DragonFly__

//Function to read register from Linux, BSD, Android systems
static int bsd_reg_read (RDebug *dbg, int type, ut8* buf, int size) {
	int showfpu = false;
	int pid = dbg->pid;
	int ret;
	if (type < -1) {
		showfpu = true; // hack for debugging
		type = -type;
	}
	switch (type) {
	case R_REG_TYPE_DRX:
#if __i386__ || __x86_64__
#if __KFBSD__
	{
		// TODO
		struct dbreg dbr;
		ret = ptrace (PT_GETDBREGS, pid, (caddr_t)&dbr, sizeof(dbr));
		if (ret != 0) return false;
		// XXX: maybe the register map is not correct, must review
	}
#endif
#endif
		return true;
		break;
	case R_REG_TYPE_FPU:
	case R_REG_TYPE_MMX:
	case R_REG_TYPE_XMM:
		break;
	case R_REG_TYPE_SEG:
	case R_REG_TYPE_FLG:
	case R_REG_TYPE_GPR:
		{
		R_DEBUG_REG_T regs;
		memset (&regs, 0, sizeof(regs));
		memset (buf, 0, size);
		#if __NetBSD__ || __OpenBSD__
			ret = ptrace (PTRACE_GETREGS, pid, (caddr_t)&regs, sizeof (regs));
		#elif __KFBSD__
			ret = ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0);
		#else
			#warning not implemented for this platform
			ret = 1;
		#endif
		// if perror here says 'no such process' and the
		// process exists still.. is because there's a
		// missing call to 'wait'. and the process is not
		// yet available to accept more ptrace queries.
		if (ret != 0) return false;
		if (sizeof(regs) < size) size = sizeof(regs);
		memcpy (buf, &regs, size);
		return sizeof(regs);
		}
		break;
	}
	return true;
}
#endif // if __sun || __NetBSD__ || __KFBSD__ || __OpenBSD__



// TODO: what about float and hardware regs here ???
// TODO: add flag for type
static int r_debug_native_reg_read (RDebug *dbg, int type, ut8 *buf, int size) {
	if (size < 1) {
		return false;
	}
#if __APPLE__
	return xnu_reg_read (dbg, type, buf, size);
#elif __WINDOWS__
	return w32_reg_read (dbg, type, buf, size);
#elif __linux__
	return linux_reg_read (dbg, type, buf, size);
#elif __sun || __NetBSD__ || __KFBSD__ || __OpenBSD__ || __DragonFly__
	return bsd_reg_read (dbg, type, buf, size);
#else
	#warning dbg-native not supported for this platform
	return false;
#endif
}

static int r_debug_native_reg_write (RDebug *dbg, int type, const ut8* buf, int size) {
	// XXX use switch or so
	if (type == R_REG_TYPE_DRX) {
#if __i386__ || __x86_64__
#if __APPLE__
		return xnu_reg_write (dbg, type, buf, size);
#elif __WINDOWS__
		return w32_reg_write (dbg, type, buf, size);
#elif __linux__
		return linux_reg_write (dbg, type, buf, size);
#elif __KFBSD__
		return (0 == ptrace (PT_SETDBREGS, dbg->pid,
			(caddr_t)buf, sizeof (struct dbreg)));
#else
		//eprintf ("TODO: No support for write DRX registers\n");
		return false;
#endif
#else // i386/x86-64
		return false;
#endif
	} else if (type == R_REG_TYPE_GPR) {
#if __APPLE__
		return xnu_reg_write (dbg, type, buf, size);
#elif __WINDOWS__
		return w32_reg_write (dbg, type, buf, size);
#elif __linux__
		return linux_reg_write (dbg, type, buf, size);
#elif __sun || __NetBSD__ || __KFBSD__ || __OpenBSD__ || __DragonFly__
		int ret = ptrace (PTRACE_SETREGS, dbg->pid,
			(void*)(size_t)buf, sizeof (R_DEBUG_REG_T));
		if (sizeof (R_DEBUG_REG_T) < size)
			size = sizeof (R_DEBUG_REG_T);
		return (ret != 0) ? false: true;
#else
#warning r_debug_native_reg_write not implemented
#endif
	} else if (type == R_REG_TYPE_FPU) {
#if __linux__
		return linux_reg_write (dbg, type, buf, size);
#endif
	} //else eprintf ("TODO: reg_write_non-gpr (%d)\n", type);
	return false;
}

#if __KFBSD__
static RList *r_debug_native_sysctl_map (RDebug *dbg) {
	int mib[4];
	size_t len;
	char *buf, *bp, *eb;
	struct kinfo_vmentry *kve;
	RList *list = NULL;
	RDebugMap *map;

	len = 0;
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_VMMAP;
	mib[3] = dbg->pid;

	if (sysctl (mib, 4, NULL, &len, NULL, 0) != 0) return NULL;
	len = len * 4 / 3;
	buf = malloc(len);
	if (!buf) {
		return NULL;
	}
	if (sysctl (mib, 4, buf, &len, NULL, 0) != 0) {
		free (buf);
		return NULL;
	}
	bp = buf;
	eb = buf + len;
	list = r_debug_map_list_new();
	if (!list) {
		free (buf);
		return NULL;
	}
	while (bp < eb) {
		kve = (struct kinfo_vmentry *)(uintptr_t)bp;
		map = r_debug_map_new (kve->kve_path, kve->kve_start,
					kve->kve_end, kve->kve_protection, 0);
		if (!map) break;
		r_list_append (list, map);
		bp += kve->kve_structsize;
	}
	free (buf);
	return list;
}
#elif __OpenBSD__
static RList *r_debug_native_sysctl_map (RDebug *dbg) {
	int mib[3];
	size_t len;
	struct kinfo_vmentry entry;
	u_long old_end = 0;
	RList *list = NULL;
	RDebugMap *map;

	len = sizeof(entry);
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC_VMMAP;
	mib[2] = dbg->pid;
	entry.kve_start = 0;

	if (sysctl (mib, 3, &entry, &len, NULL, 0) == -1) {
		eprintf ("Could not get memory map: %s\n", strerror(errno));
		return NULL;
	}

	list = r_debug_map_list_new();
	if (!list) return NULL;

	while (sysctl (mib, 3, &entry, &len, NULL, 0) != -1) {
		if (old_end == entry.kve_end) {
			/* No more entries */
			break;
		}
		/* path to vm obj is not included in kinfo_vmentry.
		 * see usr.sbin/procmap for namei-cache lookup.
		 */
		map = r_debug_map_new ("", entry.kve_start, entry.kve_end,
				entry.kve_protection, 0);
		if (!map) break;
		r_list_append (list, map);

		entry.kve_start = entry.kve_start + 1;
		old_end = entry.kve_end;
	}

	return list;
}
#elif __linux__
static int io_perms_to_prot (int io_perms) {
	int prot_perms = PROT_NONE;

	if (io_perms & R_PERM_R) {
		prot_perms |= PROT_READ;
	}
	if (io_perms & R_PERM_W) {
		prot_perms |= PROT_WRITE;
	}
	if (io_perms & R_PERM_X) {
		prot_perms |= PROT_EXEC;
	}
	return prot_perms;
}


static int linux_map_thp (RDebug *dbg, ut64 addr, int size) {
#if !defined(__ANDROID__) && defined(MADV_HUGEPAGE)
	RBuffer *buf = NULL;
	char code[1024];
	int ret = true;
	char *asm_list[] = {
		"x86", "x86.as",
		"x64", "x86.as",
		NULL
	};
	// In architectures where radare is supported, arm and x86, it is 2MB
	const size_t thpsize = 1<<21;

	if ((size%thpsize)) {
		eprintf ("size not a power of huge pages size\n");
		return false;
	}

	// In always mode, is more into mmap syscall level
	// even though the address might not have the 'hg'
	// vmflags
	if (r_sys_thp_mode() != 1) {
		eprintf ("transparent huge page mode is not in madvise mode\n");
		return false;
	}

	int num = r_syscall_get_num (dbg->anal->syscall, "madvise");

	snprintf (code, sizeof (code),
		"sc_madvise@syscall(%d);\n"
		"main@naked(0) { .rarg0 = sc_madvise(0x%08" PFMT64x ",%d, %d);break;\n"
		"}\n",
		num, addr, size, MADV_HUGEPAGE);
	r_egg_reset (dbg->egg);
	r_egg_setup (dbg->egg, dbg->arch, 8 * dbg->bits, 0, 0);
	r_egg_load (dbg->egg, code, 0);
	if (!r_egg_compile (dbg->egg)) {
		eprintf ("Cannot compile.\n");
		goto err_linux_map_thp;
	}
	if (!r_egg_assemble_asm (dbg->egg, asm_list)) {
		eprintf ("r_egg_assemble: invalid assembly\n");
		goto err_linux_map_thp;
	}
	buf = r_egg_get_bin (dbg->egg);
	if (buf) {
		r_reg_arena_push (dbg->reg);
		ut64 tmpsz;
		const ut8 *tmp = r_buf_data (buf, &tmpsz);
		ret = r_debug_execute (dbg, tmp, tmpsz, 1) == 0;
		r_reg_arena_pop (dbg->reg);
	}
err_linux_map_thp:
	return ret;
#else
	return false;
#endif
}

static RDebugMap* linux_map_alloc (RDebug *dbg, ut64 addr, int size, bool thp) {
	RBuffer *buf = NULL;
	RDebugMap* map = NULL;
	char code[1024], *sc_name;
	int num;
	/* force to usage of x86.as, not yet working x86.nz */
	char *asm_list[] = {
			"x86", "x86.as",
			"x64", "x86.as",
			NULL};

	/* NOTE: Since kernel 2.4,  that  system  call  has  been  superseded  by
       		 mmap2(2 and  nowadays  the  glibc  mmap()  wrapper  function invokes
       		 mmap2(2)). If arch is x86_32 then usage mmap2() */
	if (!strcmp (dbg->arch, "x86") && dbg->bits == 4) {
		sc_name = "mmap2";
	} else {
		sc_name = "mmap";
	}
	num = r_syscall_get_num (dbg->anal->syscall, sc_name);
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
	snprintf (code, sizeof (code),
		"sc_mmap@syscall(%d);\n"
		"main@naked(0) { .rarg0 = sc_mmap(0x%08"PFMT64x",%d,%d,%d,%d,%d);break;\n"
		"}\n",
		num, addr, size, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	r_egg_reset (dbg->egg);
	r_egg_setup (dbg->egg, dbg->arch, 8 * dbg->bits, 0, 0);
	r_egg_load (dbg->egg, code, 0);
	if (!r_egg_compile (dbg->egg)) {
		eprintf ("Cannot compile.\n");
		goto err_linux_map_alloc;
	}
	if (!r_egg_assemble_asm (dbg->egg, asm_list)) {
		eprintf ("r_egg_assemble: invalid assembly\n");
		goto err_linux_map_alloc;
	}
	buf = r_egg_get_bin (dbg->egg);
	if (buf) {
		ut64 map_addr;

		r_reg_arena_push (dbg->reg);
		ut64 tmpsz;
		const ut8 *tmp = r_buf_data (buf, &tmpsz);
		map_addr = r_debug_execute (dbg, tmp, tmpsz, 1);
		r_reg_arena_pop (dbg->reg);
		if (map_addr != (ut64)-1) {
			if (thp) {
				if (!linux_map_thp (dbg, map_addr, size)) {
					// Not overly dramatic
					eprintf ("map promotion to huge page failed\n");
				}
			}
			r_debug_map_sync (dbg);
			map = r_debug_map_get (dbg, map_addr);
		}
	}
err_linux_map_alloc:
	return map;
}

static int linux_map_dealloc(RDebug *dbg, ut64 addr, int size) {
	RBuffer *buf = NULL;
	char code[1024];
	int ret = 0;
	char *asm_list[] = {
		"x86", "x86.as",
		"x64", "x86.as",
		NULL
	};
	int num = r_syscall_get_num (dbg->anal->syscall, "munmap");

	snprintf (code, sizeof (code),
		"sc_munmap@syscall(%d);\n"
		"main@naked(0) { .rarg0 = sc_munmap(0x%08" PFMT64x ",%d);break;\n"
		"}\n",
		num, addr, size);
	r_egg_reset (dbg->egg);
	r_egg_setup (dbg->egg, dbg->arch, 8 * dbg->bits, 0, 0);
	r_egg_load (dbg->egg, code, 0);
	if (!r_egg_compile (dbg->egg)) {
		eprintf ("Cannot compile.\n");
		goto err_linux_map_dealloc;
	}
	if (!r_egg_assemble_asm (dbg->egg, asm_list)) {
		eprintf ("r_egg_assemble: invalid assembly\n");
		goto err_linux_map_dealloc;
	}
	buf = r_egg_get_bin (dbg->egg);
	if (buf) {
		r_reg_arena_push (dbg->reg);
		ut64 tmpsz;
		const ut8 *tmp = r_buf_data (buf, &tmpsz);
		ret = r_debug_execute (dbg, tmp, tmpsz, 1) == 0;
		r_reg_arena_pop (dbg->reg);
	}
err_linux_map_dealloc:
	return ret;
}
#endif

static RDebugMap* r_debug_native_map_alloc (RDebug *dbg, ut64 addr, int size, bool thp) {
#if __APPLE__
	(void)thp;
	return xnu_map_alloc (dbg, addr, size);
#elif __WINDOWS__
	(void)thp;
	return w32_map_alloc (dbg, addr, size);
#elif __linux__
	return linux_map_alloc (dbg, addr, size, thp);
#else
	// malloc not implemented for this platform
	return NULL;
#endif
}

static int r_debug_native_map_dealloc (RDebug *dbg, ut64 addr, int size) {
#if __APPLE__
	return xnu_map_dealloc (dbg, addr, size);
#elif __WINDOWS__
	return w32_map_dealloc (dbg, addr, size);
#elif __linux__
	return linux_map_dealloc (dbg, addr, size);
#else
    // mdealloc not implemented for this platform
	return false;
#endif
}

#if !__WINDOWS__ && !__APPLE__
static void _map_free(RDebugMap *map) {
	if (!map) {
		return;
	}
	free (map->name);
	free (map->file);
	free (map);
}
#endif

static RList *r_debug_native_map_get (RDebug *dbg) {
	RList *list = NULL;
#if __KFBSD__
	int ign;
	char unkstr[128];
#endif
#if __APPLE__
	list = xnu_dbg_maps (dbg, 0);
#elif __WINDOWS__
	list = r_w32_dbg_maps (dbg);
#else
#if __sun
	char path[1024];
	/* TODO: On solaris parse /proc/%d/map */
	snprintf (path, sizeof(path) - 1, "pmap %d >&2", ps.tid);
	system (path);
#else
	RDebugMap *map;
	int i, perm, unk = 0;
	char *pos_c;
	char path[1024], line[1024], name[1024];
	char region[100], region2[100], perms[5];
	FILE *fd;
	if (dbg->pid == -1) {
		//eprintf ("r_debug_native_map_get: No selected pid (-1)\n");
		return NULL;
	}
	/* prepend 0x prefix */
	region[0] = region2[0] = '0';
	region[1] = region2[1] = 'x';

#if __OpenBSD__
	/* OpenBSD has no procfs, so no idea trying. */
	return r_debug_native_sysctl_map (dbg);
#endif

#if __KFBSD__
	list = r_debug_native_sysctl_map (dbg);
	if (list) {
		return list;
	}
	snprintf (path, sizeof (path), "/proc/%d/map", dbg->pid);
#else
	snprintf (path, sizeof (path), "/proc/%d/maps", dbg->pid);
#endif
	fd = r_sandbox_fopen (path, "r");
	if (!fd) {
		perror (sdb_fmt ("Cannot open '%s'", path));
		return NULL;
	}

	list = r_list_new ();
	if (!list) {
		fclose (fd);
		return NULL;
	}
	list->free = (RListFree)_map_free;
	while (!feof (fd)) {
		size_t line_len;
		bool map_is_shared = false;
		ut64 map_start, map_end;

		if (!fgets (line, sizeof (line), fd)) {
			break;
		}
		/* kill the newline if we got one */
		line_len = strlen (line);
		if (line[line_len - 1] == '\n') {
			line[line_len - 1] = '\0';
			line_len--;
		}
		/* maps files should not have empty lines */
		if (line_len == 0) {
			break;
		}
#if __KFBSD__
		// 0x8070000 0x8072000 2 0 0xc1fde948 rw- 1 0 0x2180 COW NC vnode /usr/bin/gcc
		if (sscanf (line, "%s %s %d %d 0x%s %3s %d %d",
				&region[2], &region2[2], &ign, &ign,
				unkstr, perms, &ign, &ign) != 8) {
			eprintf ("%s: Unable to parse \"%s\"\n", __func__, path);
			r_list_free (list);
			return NULL;
		}

		/* snag the file name */
		pos_c = strchr (line, '/');
		if (pos_c) {
			strncpy (name, pos_c, sizeof (name) - 1);
		} else {
			name[0] = '\0';
		}
#else
		ut64 offset = 0;;
		// 7fc8124c4000-7fc81278d000 r--p 00000000 fc:00 17043921 /usr/lib/locale/locale-archive
		i = sscanf (line, "%s %s %08"PFMT64x" %*s %*s %[^\n]", &region[2], perms, &offset, name);
		if (i == 3) {
			name[0] = '\0';
		} else if (i != 4) {
			eprintf ("%s: Unable to parse \"%s\"\n", __func__, path);
			eprintf ("%s: problematic line: %s\n", __func__, line);
			r_list_free (list);
			return NULL;
		}

		/* split the region in two */
		pos_c = strchr (&region[2], '-');
		if (!pos_c) { // should this be an error?
			continue;
		}
		strncpy (&region2[2], pos_c + 1, sizeof (region2) - 2 - 1);
#endif // __KFBSD__
		if (!*name) {
			snprintf (name, sizeof (name), "unk%d", unk++);
		}
		perm = 0;
		for (i = 0; i < 5 && perms[i]; i++) {
			switch (perms[i]) {
			case 'r': perm |= R_PERM_R; break;
			case 'w': perm |= R_PERM_W; break;
			case 'x': perm |= R_PERM_X; break;
			case 'p': map_is_shared = false; break;
			case 's': map_is_shared = true; break;
			}
		}

		map_start = r_num_get (NULL, region);
		map_end = r_num_get (NULL, region2);
		if (map_start == map_end || map_end == 0) {
			eprintf ("%s: ignoring invalid map size: %s - %s\n", __func__, region, region2);
			continue;
		}
		map = r_debug_map_new (name, map_start, map_end, perm, 0);
		if (!map) {
			break;
		}
#if __linux__
		map->offset = offset;
		map->shared = map_is_shared;
#endif
		map->file = strdup (name);
		r_list_append (list, map);
	}
	fclose (fd);
#endif // __sun
#endif // __APPLE__
	return list;
}

static RList *r_debug_native_modules_get (RDebug *dbg) {
	char *lastname = NULL;
	RDebugMap *map;
	RListIter *iter, *iter2;
	RList *list, *last;
	bool must_delete;
#if __APPLE__
	list = xnu_dbg_maps (dbg, 1);
	if (list && !r_list_empty (list)) {
		return list;
	}
#elif  __WINDOWS__
	list = r_w32_dbg_modules (dbg);
	if (list && !r_list_empty (list)) {
		return list;
	}
#endif
	if (!(list = r_debug_native_map_get (dbg))) {
		return NULL;
	}
	if (!(last = r_list_newf ((RListFree)r_debug_map_free))) {
		r_list_free (list);
		return NULL;
	}
	r_list_foreach_safe (list, iter, iter2, map) {
		const char *file = map->file;
		if (!map->file) {
			file = map->file = strdup (map->name);
		}
		must_delete = true;
		if (file && *file == '/') {
			if (!lastname || strcmp (lastname, file)) {
				must_delete = false;
			}
		}
		if (must_delete) {
			r_list_delete (list, iter);
		} else {
			r_list_append (last, map);
			free (lastname);
			lastname = strdup (file);
		}
	}
	list->free = NULL;
	free (lastname);
	r_list_free (list);
	return last;
}

static bool r_debug_native_kill(RDebug *dbg, int pid, int tid, int sig) {
	bool ret = false;
	if (pid == 0) {
		pid = dbg->pid;
	}
#if __WINDOWS__
	ret = w32_kill (dbg, pid, tid, sig);
#else
#if 0
	if (thread) {
// XXX this is linux>2.5 specific..ugly
		if (dbg->tid>0 && (ret = tgkill (dbg->pid, dbg->tid, sig))) {
			if (ret != -1)
				ret = true;
		}
	} else {
#endif
	if (sig == SIGKILL && dbg->threads) {
		r_list_free (dbg->threads);
		dbg->threads = NULL;
	}
	if ((r_sandbox_kill (pid, sig) != -1)) {
		ret = true;
	}
	if (errno == 1) {
		ret = -true; // EPERM
	}
#if 0
//	}
#endif
#endif
	return ret;
}

struct r_debug_desc_plugin_t r_debug_desc_plugin_native;
static int r_debug_native_init (RDebug *dbg) {
	dbg->h->desc = r_debug_desc_plugin_native;
#if __WINDOWS__
	return w32_init (dbg);
#else
	return true;
#endif
}

static void sync_drx_regs (RDebug *dbg, drxt *regs, size_t num_regs) {
#if __i386__ || __x86_64__
	/* sanity check, we rely on this assumption */
	if (num_regs != NUM_DRX_REGISTERS) {
		eprintf ("drx: Unsupported number of registers for get_debug_regs\n");
		return;
	}

	// sync drx regs
#define R dbg->reg
	regs[0] = r_reg_getv (R, "dr0");
	regs[1] = r_reg_getv (R, "dr1");
	regs[2] = r_reg_getv (R, "dr2");
	regs[3] = r_reg_getv (R, "dr3");
/*
	RESERVED
	regs[4] = r_reg_getv (R, "dr4");
	regs[5] = r_reg_getv (R, "dr5");
*/
	regs[6] = r_reg_getv (R, "dr6");
	regs[7] = r_reg_getv (R, "dr7");
#else
	eprintf ("drx sync_drx_regs: Unsupported platform\n");
#endif
}

static void set_drx_regs (RDebug *dbg, drxt *regs, size_t num_regs) {
#if __i386__ || __x86_64__
	/* sanity check, we rely on this assumption */
	if (num_regs != NUM_DRX_REGISTERS){
		eprintf ("drx: Unsupported number of registers for get_debug_regs\n");
		return;
	}

#define R dbg->reg
 	r_reg_setv (R, "dr0", regs[0]);
	r_reg_setv (R, "dr1", regs[1]);
	r_reg_setv (R, "dr2", regs[2]);
	r_reg_setv (R, "dr3", regs[3]);
	r_reg_setv (R, "dr6", regs[6]);
	r_reg_setv (R, "dr7", regs[7]);
#else
	eprintf ("drx set_drx_regs: Unsupported platform\n");
#endif
}

static int r_debug_native_drx (RDebug *dbg, int n, ut64 addr, int sz, int rwx, int g, int api_type) {
#if __i386__ || __x86_64__
	int retval = false;
	drxt regs[NUM_DRX_REGISTERS] = {0};
	// sync drx regs
	sync_drx_regs (dbg, regs, NUM_DRX_REGISTERS);

	switch (api_type) {
	case DRX_API_LIST:
		drx_list (regs);
		retval = false;
		break;
	case DRX_API_GET_BP:
		/* get the index of the breakpoint at addr */
		retval = drx_get_at (regs, addr);
		break;
	case DRX_API_REMOVE_BP:
		/* remove hardware breakpoint */
		drx_set (regs, n, addr, -1, 0, 0);
		retval = true;
		break;
	case DRX_API_SET_BP:
		/* set hardware breakpoint */
		drx_set (regs, n, addr, sz, rwx, g);
		retval = true;
		break;
	default:
		/* this should not happen, someone misused the API */
		eprintf ("drx: Unsupported api type in r_debug_native_drx\n");
		retval = false;
	}

	set_drx_regs (dbg, regs, NUM_DRX_REGISTERS);

	return retval;
#else
	eprintf ("drx: Unsupported platform\n");
#endif
	return false;
}


#if __linux__

#if __arm__ || __arm64__ || __aarch64__
#include <sys/prctl.h>
#include <sys/uio.h>

#define NT_ARM_VFP	0x400		/* ARM VFP/NEON registers */
#define NT_ARM_TLS	0x401		/* ARM TLS register */
#define NT_ARM_HW_BREAK	0x402		/* ARM hardware breakpoint registers */
#define NT_ARM_HW_WATCH	0x403		/* ARM hardware watchpoint registers */
#define NT_ARM_SYSTEM_CALL	0x404	/* ARM system call number */

#ifndef PTRACE_GETHBPREGS
#define PTRACE_GETHBPREGS 29
#define PTRACE_SETHBPREGS 30
#endif

#if __arm__

static bool ll_arm32_hwbp_set(pid_t pid, ut64 addr, int size, int wp, int type) {
	const unsigned byte_mask = (1 << size) - 1;
	//const unsigned type = 2; // Write.
	const unsigned enable = 1;
	const unsigned control = byte_mask << 5 | type << 3 | enable;
	(void)ptrace (PTRACE_SETHBPREGS, pid, -1, (void*)(size_t)addr);
	return ptrace (PTRACE_SETHBPREGS, pid, -2, &control) != -1;
}

static bool arm32_hwbp_add (RDebug *dbg, RBreakpoint* bp, RBreakpointItem *b) {
	return ll_arm32_hwbp_set (dbg->pid, b->addr, b->size, 0, 1 | 2 | 4);
}

static bool arm32_hwbp_del (RDebug *dbg, RBreakpoint *bp, RBreakpointItem *b) {
	return false; // TODO: hwbp.del not yetimplemented
}
#else
static bool ll_arm32_hwbp_set(pid_t pid, ut64 addr, int size, int wp, int type) {
	return false;
}

static bool arm32_hwbp_add (RDebug *dbg, RBreakpoint* bp, RBreakpointItem *b) {
	return false;
}

static bool arm32_hwbp_del (RDebug *dbg, RBreakpoint *bp, RBreakpointItem *b) {
	return false;
}
#endif // PTRACE_GETHWBPREGS
#endif // __arm

#if __arm64__ || __aarch64__
// type = 2 = write
//static volatile uint8_t var[96] __attribute__((__aligned__(32)));

static bool ll_arm64_hwbp_set(pid_t pid, ut64 _addr, int size, int wp, ut32 type) {
	const volatile uint8_t *addr = (void*)(size_t)_addr; //&var[32 + wp];
	const unsigned int offset = (uintptr_t)addr % 8;
	const ut32 byte_mask = ((1 << size) - 1) << offset;
	const ut32 enable = 1;
	const ut32 control = byte_mask << 5 | type << 3 | enable;

	struct user_hwdebug_state dreg_state = {0};
	struct iovec iov = {0};
	iov.iov_base = &dreg_state;
	iov.iov_len = sizeof (dreg_state);

	if (ptrace (PTRACE_GETREGSET, pid, NT_ARM_HW_WATCH, &iov) == -1) {
		// error reading regs
	}
	memcpy (&dreg_state, iov.iov_base, sizeof (dreg_state));
	// wp is not honored here i think... we can't have more than one wp for now..
	dreg_state.dbg_regs[0].addr = (uintptr_t)(addr - offset);
	dreg_state.dbg_regs[0].ctrl = control;
	iov.iov_base = &dreg_state;
	iov.iov_len = r_offsetof (struct user_hwdebug_state, dbg_regs) +
				sizeof (dreg_state.dbg_regs[0]);
	if (ptrace (PTRACE_SETREGSET, pid, NT_ARM_HW_WATCH, &iov) == 0) {
		return true;
	}

	if (errno == EIO) {
		eprintf ("ptrace(PTRACE_SETREGSET, NT_ARM_HW_WATCH) not supported on this hardware: %s\n",
			strerror (errno));
	}

	eprintf ("ptrace(PTRACE_SETREGSET, NT_ARM_HW_WATCH) failed: %s\n", strerror (errno));
	return false;
}

static bool ll_arm64_hwbp_del(pid_t pid, ut64 _addr, int size, int wp, ut32 type) {
	// const volatile uint8_t *addr = &var[32 + wp];
	// TODO: support multiple watchpoints and find
	struct user_hwdebug_state dreg_state = {0};
	struct iovec iov = {0};
	iov.iov_base = &dreg_state;
	// only delete 1 bp for now
	iov.iov_len = r_offsetof (struct user_hwdebug_state, dbg_regs) +
				sizeof (dreg_state.dbg_regs[0]);
	if (ptrace (PTRACE_SETREGSET, pid, NT_ARM_HW_WATCH, &iov) == 0) {
		return true;
	}
	if (errno == EIO) {
		eprintf ("ptrace(PTRACE_SETREGSET, NT_ARM_HW_WATCH) not supported on this hardware: %s\n",
			strerror (errno));
	}

	eprintf ("ptrace(PTRACE_SETREGSET, NT_ARM_HW_WATCH) failed: %s\n", strerror (errno));
	return false;
}

static bool arm64_hwbp_add (RDebug *dbg, RBreakpoint* bp, RBreakpointItem *b) {
	return ll_arm64_hwbp_set (dbg->pid, b->addr, b->size, 0, 1 | 2 | 4);
}

static bool arm64_hwbp_del (RDebug *dbg, RBreakpoint *bp, RBreakpointItem *b) {
	return ll_arm64_hwbp_del (dbg->pid, b->addr, b->size, 0, 1 | 2 | 4);
}

#endif //  __arm64__
#endif // __linux__

/*
 * set or unset breakpoints...
 *
 * we only handle the case for hardware breakpoints here. otherwise,
 * we let the caller handle the work.
 */
static int r_debug_native_bp (RBreakpoint *bp, RBreakpointItem *b, bool set) {
	RDebug *dbg = bp->user;
	if (b && b->hw) {
#if __i386__ || __x86_64__
	return set
		? drx_add (dbg, bp, b)
		: drx_del (dbg, bp, b);
#elif __arm64__ || __aarch64__
# if __linux__
	return set
		? arm64_hwbp_add (dbg, bp, b)
		: arm64_hwbp_del (dbg, bp, b);
# endif
#elif __arm__
	return set
		? arm32_hwbp_add (dbg, bp, b)
		: arm32_hwbp_del (dbg, bp, b);
#endif
	}
	return false;
}

#if __KFBSD__

#include <sys/un.h>
#include <arpa/inet.h>

static void addr_to_string (struct sockaddr_storage *ss, char *buffer, int buflen) {
	char buffer2[INET6_ADDRSTRLEN];
	struct sockaddr_in6 *sin6;
	struct sockaddr_in *sin;
	struct sockaddr_un *sun;

	if (buflen > 0)
	switch (ss->ss_family) {
	case AF_LOCAL:
		sun = (struct sockaddr_un *)ss;
		strncpy (buffer, (sun && *sun->sun_path)?
			sun->sun_path: "-", buflen - 1);
		break;
	case AF_INET:
		sin = (struct sockaddr_in *)ss;
		snprintf (buffer, buflen, "%s:%d", inet_ntoa (sin->sin_addr),
				ntohs (sin->sin_port));
		break;
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)ss;
		if (inet_ntop (AF_INET6, &sin6->sin6_addr, buffer2,
				sizeof (buffer2)) != NULL) {
			snprintf (buffer, buflen, "%s.%d", buffer2,
				ntohs (sin6->sin6_port));
		} else {
			strcpy (buffer, "-");
		}
		break;
	default:
		*buffer = 0;
		break;
	}
}
#endif

#if __APPLE__

static int getMaxFiles() {
	struct rlimit limit;
	if (getrlimit (RLIMIT_NOFILE, &limit) != 0) {
		return 1024;
	}
	return limit.rlim_cur;
}

static RList *xnu_desc_list (int pid) {
#if TARGET_OS_IPHONE || __POWERPC__
	return NULL;
#else
#define xwr2rwx(x) ((x&1)<<2) | (x&2) | ((x&4)>>2)
	RDebugDesc *desc;
	RList *ret = r_list_new();
	struct vnode_fdinfowithpath vi;
	int i, nb, type = 0;
	int maxfd = getMaxFiles();

	for (i=0 ; i<maxfd; i++) {
		nb = proc_pidfdinfo (pid, i, PROC_PIDFDVNODEPATHINFO, &vi, sizeof (vi));
		if (nb<1) {
			continue;
		}
		if (nb < sizeof (vi)) {
			perror ("too few bytes");
			break;
		}
		//printf ("FD %d RWX %x ", i, vi.pfi.fi_openflags);
		//printf ("PATH %s\n", vi.pvip.vip_path);
		desc = r_debug_desc_new (i,
				vi.pvip.vip_path,
				xwr2rwx(vi.pfi.fi_openflags),
				type, 0);
		r_list_append (ret, desc);
	}
	return ret;
#endif
}
#endif

static RList *r_debug_desc_native_list (int pid) {
#if __APPLE__
	return xnu_desc_list (pid);
#elif __WINDOWS__
	return w32_desc_list (pid);
#elif __KFBSD__
	RList *ret = NULL;
	int perm, type, mib[4];
	size_t len;
	char *buf, *bp, *eb, *str, path[1024];
	RDebugDesc *desc;
	struct kinfo_file *kve;

	len = 0;
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_FILEDESC;
	mib[3] = pid;

	if (sysctl (mib, 4, NULL, &len, NULL, 0) != 0) return NULL;
	len = len * 4 / 3;
	buf = malloc(len);
	if (!buf) {
		return NULL;
	}
	if (sysctl (mib, 4, buf, &len, NULL, 0) != 0) {
		free (buf);
		return NULL;
	}
	bp = buf;
	eb = buf + len;
	ret = r_list_new ();
	if (!ret) {
		free (buf);
		return NULL;
	}
	ret->free = (RListFree) r_debug_desc_free;
	while (bp < eb) {
		kve = (struct kinfo_file *)(uintptr_t)bp;
		bp += kve->kf_structsize;
		if (kve->kf_fd < 0) continue; // Skip root and cwd. We need it ??
		str = kve->kf_path;
		switch (kve->kf_type) {
		case KF_TYPE_VNODE: type = 'v'; break;
		case KF_TYPE_SOCKET:
			type = 's';
#if __FreeBSD_version < 1200031
			if (kve->kf_sock_domain == AF_LOCAL) {
				struct sockaddr_un *sun =
					(struct sockaddr_un *)&kve->kf_sa_local;
				if (sun->sun_path[0] != 0)
					addr_to_string (&kve->kf_sa_local, path, sizeof(path));
				else
					addr_to_string (&kve->kf_sa_peer, path, sizeof(path));
			} else {
				addr_to_string (&kve->kf_sa_local, path, sizeof(path));
				strcat (path, " ");
				addr_to_string (&kve->kf_sa_peer, path + strlen (path),
						sizeof (path));
			}
#else
			if (kve->kf_sock_domain == AF_LOCAL) {
				struct sockaddr_un *sun =
					(struct sockaddr_un *)&kve->kf_un.kf_sock.kf_sa_local;;
				if (sun->sun_path[0] != 0)
					addr_to_string (&kve->kf_un.kf_sock.kf_sa_local, path, sizeof(path));
				else
					addr_to_string (&kve->kf_un.kf_sock.kf_sa_peer, path, sizeof(path));
			} else {
				addr_to_string (&kve->kf_un.kf_sock.kf_sa_local, path, sizeof(path));
				strcat (path, " ");
				addr_to_string (&kve->kf_un.kf_sock.kf_sa_peer, path + strlen (path),
						sizeof (path));
			}
#endif
			str = path;
			break;
		case KF_TYPE_PIPE: type = 'p'; break;
		case KF_TYPE_FIFO: type = 'f'; break;
		case KF_TYPE_KQUEUE: type = 'k'; break;
		case KF_TYPE_CRYPTO: type = 'c'; break;
		case KF_TYPE_MQUEUE: type = 'm'; break;
		case KF_TYPE_SHM: type = 'h'; break;
		case KF_TYPE_PTS: type = 't'; break;
		case KF_TYPE_SEM: type = 'e'; break;
		case KF_TYPE_NONE:
		case KF_TYPE_UNKNOWN:
		default: type = '-'; break;
		}
		perm = (kve->kf_flags & KF_FLAG_READ)? R_PERM_R: 0;
		perm |= (kve->kf_flags & KF_FLAG_WRITE)? R_PERM_W: 0;
		desc = r_debug_desc_new (kve->kf_fd, str, perm, type, kve->kf_offset);
		if (!desc) {
			break;
		}
		r_list_append (ret, desc);
	}

	free (buf);
	return ret;
#elif __linux__
	return linux_desc_list (pid);
#else
#warning list filedescriptors not supported for this platform
	return NULL;
#endif
}

static int r_debug_native_map_protect (RDebug *dbg, ut64 addr, int size, int perms) {
#if __WINDOWS__
	return w32_map_protect (dbg, addr, size, perms);
#elif __APPLE__
	return xnu_map_protect (dbg, addr, size, perms);
#elif __linux__
	RBuffer *buf = NULL;
	char code[1024];
	int num;

	num = r_syscall_get_num (dbg->anal->syscall, "mprotect");
	snprintf (code, sizeof (code),
		"sc@syscall(%d);\n"
		"main@global(0) { sc(%p,%d,%d);\n"
		":int3\n"
		"}\n", num, (void*)addr, size, io_perms_to_prot (perms));

	r_egg_reset (dbg->egg);
	r_egg_setup(dbg->egg, dbg->arch, 8 * dbg->bits, 0, 0);
	r_egg_load (dbg->egg, code, 0);
	if (!r_egg_compile (dbg->egg)) {
		eprintf ("Cannot compile.\n");
		return false;
	}
	if (!r_egg_assemble (dbg->egg)) {
		eprintf ("r_egg_assemble: invalid assembly\n");
		return false;
	}
	buf = r_egg_get_bin (dbg->egg);
	if (buf) {
		r_reg_arena_push (dbg->reg);
		ut64 tmpsz;
		const ut8 *tmp = r_buf_data (buf, &tmpsz);
		r_debug_execute (dbg, tmp, tmpsz, 1);
		r_reg_arena_pop (dbg->reg);
		return true;
	}

	return false;
#else
	// mprotect not implemented for this platform
	return false;
#endif
}

static int r_debug_desc_native_open (const char *path) {
	return 0;
}

#if 0
static int r_debug_setup_ownership (int fd, RDebug *dbg) {
	RDebugInfo *info = r_debug_info (dbg, NULL);

	if (!info) {
		eprintf ("Error while getting debug info.\n");
		return -1;
	}
	fchown (fd, info->uid, info->gid);
	r_debug_info_free (info);
  	return 0;
}
#endif

static bool r_debug_gcore (RDebug *dbg, RBuffer *dest) {
#if __APPLE__
	return xnu_generate_corefile (dbg, dest);
#elif __linux__ && (__x86_64__ || __i386__ || __arm__ || __arm64__)
#  if __ANDROID__
	return false;
#  else
	return linux_generate_corefile (dbg, dest);
#  endif
#else
	return false;
#endif
}

struct r_debug_desc_plugin_t r_debug_desc_plugin_native = {
	.open = r_debug_desc_native_open,
	.list = r_debug_desc_native_list,
};

RDebugPlugin r_debug_plugin_native = {
	.name = "native",
	.license = "LGPL3",
#if __i386__
	.bits = R_SYS_BITS_32,
	.arch = "x86",
	.canstep = 1,
#elif __x86_64__
	.bits = R_SYS_BITS_32 | R_SYS_BITS_64,
	.arch = "x86",
	.canstep = 1, // XXX it's 1 on some platforms...
#elif __aarch64__ || __arm64__
	.bits = R_SYS_BITS_16 | R_SYS_BITS_32 | R_SYS_BITS_64,
	.arch = "arm",
	.canstep = 1,
#elif __arm__
	.bits = R_SYS_BITS_16 | R_SYS_BITS_32 | R_SYS_BITS_64,
	.arch = "arm",
	.canstep = 0,
#elif __mips__
	.bits = R_SYS_BITS_32 | R_SYS_BITS_64,
	.arch = "mips",
	.canstep = 0,
#elif __powerpc__
# if __powerpc64__
	.bits = R_SYS_BITS_32 | R_SYS_BITS_64,
# else
	.bits = R_SYS_BITS_32,
#endif
	.arch = "ppc",
	.canstep = 1,
#else
	.bits = 0,
	.arch = 0,
	.canstep = 0,
#ifdef _MSC_VER
#pragma message("Unsupported architecture")
#else
#warning Unsupported architecture
#endif
#endif
	.init = &r_debug_native_init,
	.step = &r_debug_native_step,
	.cont = &r_debug_native_continue,
	.contsc = &r_debug_native_continue_syscall,
	.attach = &r_debug_native_attach,
	.detach = &r_debug_native_detach,
// TODO: add native select for other platforms?
#if __WINDOWS__ || __linux__
	.select = &r_debug_native_select,
#endif
	.pids = &r_debug_native_pids,
	.tids = &r_debug_native_tids,
	.threads = &r_debug_native_threads,
	.wait = &r_debug_native_wait,
	.kill = &r_debug_native_kill,
	.frames = &r_debug_native_frames, // rename to backtrace ?
	.reg_profile = r_debug_native_reg_profile,
	.reg_read = r_debug_native_reg_read,
	.info = r_debug_native_info,
	.reg_write = (void *)&r_debug_native_reg_write,
	.map_alloc = r_debug_native_map_alloc,
	.map_dealloc = r_debug_native_map_dealloc,
	.map_get = r_debug_native_map_get,
	.modules_get = r_debug_native_modules_get,
	.map_protect = r_debug_native_map_protect,
	.breakpoint = r_debug_native_bp,
	.drx = r_debug_native_drx,
	.gcore = r_debug_gcore,
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_DBG,
	.data = &r_debug_plugin_native,
	.version = R2_VERSION
};
#endif // R2_PLUGIN_INCORE

//#endif
#else // DEBUGGER
RDebugPlugin r_debug_plugin_native = {
	NULL // .name = "native",
};

#endif // DEBUGGER
