#include "strata/sandbox.h"
#include <stdio.h>

#ifdef __linux__

#include <stddef.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <errno.h>

/* Syscall numbers vary by arch — use __NR_ defines */
#include <sys/syscall.h>

/* BPF helpers */
#define SC_ALLOW(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

int strata_sandbox_apply(void) {
    struct sock_filter filter[] = {
        /* Load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr)),

        /* Process lifecycle */
        SC_ALLOW(__NR_exit_group),
        SC_ALLOW(__NR_exit),

        /* Memory management */
        SC_ALLOW(__NR_brk),
        SC_ALLOW(__NR_mmap),
        SC_ALLOW(__NR_munmap),
        SC_ALLOW(__NR_mprotect),
        SC_ALLOW(__NR_mremap),
        SC_ALLOW(__NR_madvise),

        /* Basic I/O */
        SC_ALLOW(__NR_read),
        SC_ALLOW(__NR_write),
        SC_ALLOW(__NR_close),
        SC_ALLOW(__NR_fstat),
        SC_ALLOW(__NR_lseek),
        SC_ALLOW(__NR_fsync),
        SC_ALLOW(__NR_ioctl),
        SC_ALLOW(__NR_fcntl),

        /* File access (SQLite, TCC) */
        SC_ALLOW(__NR_openat),
        SC_ALLOW(__NR_stat),
        SC_ALLOW(__NR_unlink),
        SC_ALLOW(__NR_access),
        SC_ALLOW(__NR_getcwd),
        SC_ALLOW(__NR_readlink),
        SC_ALLOW(__NR_newfstatat),

        /* ZMQ networking */
        SC_ALLOW(__NR_socket),
        SC_ALLOW(__NR_connect),
        SC_ALLOW(__NR_bind),
        SC_ALLOW(__NR_sendmsg),
        SC_ALLOW(__NR_recvmsg),
        SC_ALLOW(__NR_sendto),
        SC_ALLOW(__NR_recvfrom),
        SC_ALLOW(__NR_setsockopt),
        SC_ALLOW(__NR_getsockopt),
        SC_ALLOW(__NR_getsockname),
        SC_ALLOW(__NR_getpeername),

        /* Polling (ZMQ) */
        SC_ALLOW(__NR_poll),
        SC_ALLOW(__NR_ppoll),
        SC_ALLOW(__NR_epoll_create1),
        SC_ALLOW(__NR_epoll_ctl),
        SC_ALLOW(__NR_epoll_wait),
        SC_ALLOW(__NR_eventfd2),

        /* Time */
        SC_ALLOW(__NR_clock_gettime),
        SC_ALLOW(__NR_gettimeofday),
        SC_ALLOW(__NR_nanosleep),

        /* Threading/sync (ZMQ internals) */
        SC_ALLOW(__NR_futex),
        SC_ALLOW(__NR_clone),
        SC_ALLOW(__NR_set_robust_list),
        SC_ALLOW(__NR_rseq),
        SC_ALLOW(__NR_sched_yield),

        /* Misc required */
        SC_ALLOW(__NR_getpid),
        SC_ALLOW(__NR_gettid),
        SC_ALLOW(__NR_rt_sigaction),
        SC_ALLOW(__NR_rt_sigprocmask),
        SC_ALLOW(__NR_rt_sigreturn),
        SC_ALLOW(__NR_sigaltstack),
        SC_ALLOW(__NR_pipe2),
        SC_ALLOW(__NR_getrandom),
        SC_ALLOW(__NR_prlimit64),

        /* Default: kill the process */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("[sandbox] PR_SET_NO_NEW_PRIVS");
        return -1;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        perror("[sandbox] PR_SET_SECCOMP");
        return -1;
    }

    return 0;
}

#elif defined(__APPLE__)

#include <sandbox.h>

int strata_sandbox_apply(void) {
    char *err = NULL;
    /* Deny fork and exec — the den should not spawn subprocesses */
    const char *profile =
        "(version 1)"
        "(allow default)"
        "(deny process-fork)"
        "(deny process-exec)";

    if (sandbox_init(profile, 0, &err) != 0) {
        fprintf(stderr, "[sandbox] sandbox_init: %s\n", err);
        sandbox_free_error(err);
        return -1;
    }
    return 0;
}

#else

/* Unsupported platform — no sandbox, rely on fork isolation */
int strata_sandbox_apply(void) {
    fprintf(stderr, "[sandbox] warning: no sandbox support on this platform\n");
    return 0;
}

#endif
