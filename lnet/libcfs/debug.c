/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/notifier.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/completion.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/miscdevice.h>
#include <linux/version.h>

# define DEBUG_SUBSYSTEM S_PORTALS

#include <linux/kp30.h>
#include <linux/portals_compat25.h>
#include <linux/libcfs.h>

#include "tracefile.h"

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
#include <linux/kallsyms.h>
#endif

unsigned int portal_subsystem_debug = ~0 - (S_PORTALS | S_NAL);
EXPORT_SYMBOL(portal_subsystem_debug);

unsigned int portal_debug = (D_WARNING | D_DLMTRACE | D_ERROR | D_EMERG | D_HA |
                             D_RPCTRACE | D_VFSTRACE);
EXPORT_SYMBOL(portal_debug);

unsigned int portal_printk;
EXPORT_SYMBOL(portal_printk);

unsigned int portal_stack;
EXPORT_SYMBOL(portal_stack);

#ifdef __KERNEL__
atomic_t portal_kmemory = ATOMIC_INIT(0);
EXPORT_SYMBOL(portal_kmemory);
#endif

static DECLARE_WAIT_QUEUE_HEAD(debug_ctlwq);

char debug_file_path[1024] = "/tmp/lustre-log";
static char debug_file_name[1024];
static int handled_panic; /* to avoid recursive calls to notifiers */
char portals_upcall[1024] = "/usr/lib/lustre/portals_upcall";

void portals_debug_dumplog_internal(void *arg)
{
        void *journal_info = current->journal_info;
        current->journal_info = NULL;

        snprintf(debug_file_name, sizeof(debug_file_path) - 1,
                 "%s.%ld.%ld", debug_file_path, CURRENT_SECONDS, (long)arg);
        printk(KERN_ALERT "LustreError: dumping log to %s\n", debug_file_name);
        tracefile_dump_all_pages(debug_file_name);

        current->journal_info = journal_info;
}

int portals_debug_dumplog_thread(void *arg)
{
        kportal_daemonize("");
        reparent_to_init();
        portals_debug_dumplog_internal(arg);
        wake_up(&debug_ctlwq);
        return 0;
}

void portals_debug_dumplog(void)
{
        int rc;
        DECLARE_WAITQUEUE(wait, current);
        ENTRY;

        /* we're being careful to ensure that the kernel thread is
         * able to set our state to running as it exits before we
         * get to schedule() */
        set_current_state(TASK_INTERRUPTIBLE);
        add_wait_queue(&debug_ctlwq, &wait);

        rc = kernel_thread(portals_debug_dumplog_thread,
                           (void *)(long)current->pid,
                           CLONE_VM | CLONE_FS | CLONE_FILES);
        if (rc < 0)
                printk(KERN_ERR "LustreError: cannot start log dump thread: "
                       "%d\n", rc);
        else
                schedule();

        /* be sure to teardown if kernel_thread() failed */
        remove_wait_queue(&debug_ctlwq, &wait);
        set_current_state(TASK_RUNNING);
}

static int panic_dumplog(struct notifier_block *self, unsigned long unused1,
                         void *unused2)
{
        if (handled_panic)
                return 0;
        else
                handled_panic = 1;

        if (in_interrupt()) {
                trace_debug_print();
                return 0;
        }

        while (current->lock_depth >= 0)
                unlock_kernel();
        portals_debug_dumplog();
        return 0;
}

static struct notifier_block lustre_panic_notifier = {
        notifier_call :     panic_dumplog,
        next :              NULL,
        priority :          10000
};

int portals_debug_init(unsigned long bufsize)
{
        notifier_chain_register(&panic_notifier_list, &lustre_panic_notifier);
        return tracefile_init();
}

int portals_debug_cleanup(void)
{
        tracefile_exit();
        notifier_chain_unregister(&panic_notifier_list, &lustre_panic_notifier);
        return 0;
}

int portals_debug_clear_buffer(void)
{
        trace_flush_pages();
        return 0;
}

/* Debug markers, although printed by S_PORTALS
 * should not be be marked as such. */
#undef DEBUG_SUBSYSTEM
#define DEBUG_SUBSYSTEM S_UNDEFINED
int portals_debug_mark_buffer(char *text)
{
        CDEBUG(D_TRACE,"***************************************************\n");
        CDEBUG(D_WARNING, "DEBUG MARKER: %s\n", text);
        CDEBUG(D_TRACE,"***************************************************\n");

        return 0;
}
#undef DEBUG_SUBSYSTEM
#define DEBUG_SUBSYSTEM S_PORTALS

void portals_debug_set_level(unsigned int debug_level)
{
        printk(KERN_WARNING "Lustre: Setting portals debug level to %08x\n",
               debug_level);
        portal_debug = debug_level;
}

void portals_run_upcall(char **argv)
{
        int   rc;
        int   argc;
        char *envp[] = {
                "HOME=/",
                "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
                NULL};
        ENTRY;

        argv[0] = portals_upcall;
        argc = 1;
        while (argv[argc] != NULL)
                argc++;

        LASSERT(argc >= 2);

        rc = USERMODEHELPER(argv[0], argv, envp);
        if (rc < 0) {
                CERROR("Error %d invoking portals upcall %s %s%s%s%s%s%s%s%s; "
                       "check /proc/sys/portals/upcall\n",
                       rc, argv[0], argv[1],
                       argc < 3 ? "" : ",", argc < 3 ? "" : argv[2],
                       argc < 4 ? "" : ",", argc < 4 ? "" : argv[3],
                       argc < 5 ? "" : ",", argc < 5 ? "" : argv[4],
                       argc < 6 ? "" : ",...");
        } else {
                CERROR("Invoked portals upcall %s %s%s%s%s%s%s%s%s\n",
                       argv[0], argv[1],
                       argc < 3 ? "" : ",", argc < 3 ? "" : argv[2],
                       argc < 4 ? "" : ",", argc < 4 ? "" : argv[3],
                       argc < 5 ? "" : ",", argc < 5 ? "" : argv[4],
                       argc < 6 ? "" : ",...");
        }
}

void portals_run_lbug_upcall(char *file, const char *fn, const int line)
{
        char *argv[6];
        char buf[32];

        ENTRY;
        snprintf (buf, sizeof buf, "%d", line);

        argv[1] = "LBUG";
        argv[2] = file;
        argv[3] = (char *)fn;
        argv[4] = buf;
        argv[5] = NULL;

        portals_run_upcall (argv);
}

char *portals_nid2str(int nal, ptl_nid_t nid, char *str)
{
        if (nid == PTL_NID_ANY) {
                snprintf(str, PTL_NALFMT_SIZE, "%s", "PTL_NID_ANY");
                return str;
        }

        switch(nal){
/* XXX this could be a nal method of some sort, 'cept it's config
 * dependent whether (say) socknal NIDs are actually IP addresses... */
#if !CRAY_PORTALS 
        case TCPNAL:
                /* userspace NAL */
        case IIBNAL:
        case OPENIBNAL:
        case RANAL:
        case SOCKNAL:
                snprintf(str, PTL_NALFMT_SIZE, "%u:%u.%u.%u.%u",
                         (__u32)(nid >> 32), HIPQUAD(nid));
                break;
        case QSWNAL:
        case GMNAL:
        case LONAL:
                snprintf(str, PTL_NALFMT_SIZE, "%u:%u",
                         (__u32)(nid >> 32), (__u32)nid);
                break;
#endif
        default:
                snprintf(str, PTL_NALFMT_SIZE, "?%x? %llx",
                         nal, (long long)nid);
                break;
        }
        return str;
}

char *portals_id2str(int nal, ptl_process_id_t id, char *str)
{
        int   len;
        
        portals_nid2str(nal, id.nid, str);
        len = strlen(str);
        snprintf(str + len, PTL_NALFMT_SIZE - len, "-%u", id.pid);
        return str;
}

#ifdef __KERNEL__

void portals_debug_dumpstack(struct task_struct *tsk)
{
#if defined(__arch_um__)
        if (tsk != NULL)
                CWARN("stack dump for pid %d (%d) requested; wake up gdb.\n",
                      tsk->pid, UML_PID(tsk));
        asm("int $3");
#elif defined(HAVE_SHOW_TASK)
        /* this is exported by lustre kernel version 42 */
        extern void show_task(struct task_struct *);

        if (tsk == NULL)
                tsk = current;
        CWARN("showing stack for process %d\n", tsk->pid);
        show_task(tsk);
#else
        CWARN("can't show stack: kernel doesn't export show_task\n");
#endif
}

struct task_struct *portals_current(void)
{
        CWARN("current task struct is %p\n", current);
        return current;
}

EXPORT_SYMBOL(portals_debug_dumpstack);
EXPORT_SYMBOL(portals_current);
#endif /* __KERNEL__ */

EXPORT_SYMBOL(portals_debug_dumplog);
EXPORT_SYMBOL(portals_debug_set_level);
EXPORT_SYMBOL(portals_run_upcall);
EXPORT_SYMBOL(portals_run_lbug_upcall);
EXPORT_SYMBOL(portals_nid2str);
EXPORT_SYMBOL(portals_id2str);
