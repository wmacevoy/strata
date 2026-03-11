#ifndef STRATA_SANDBOX_H
#define STRATA_SANDBOX_H

/* Apply OS-level sandbox restrictions to the current process.
 * Call after fork(), after bedrock_setup()/local_db_load(),
 * but before compiling or running untrusted code.
 *
 * Linux:  seccomp-bpf syscall filter (no external deps)
 * macOS:  Seatbelt sandbox_init (deny fork/exec)
 *
 * Returns 0 on success, -1 on failure. */
int strata_sandbox_apply(void);

#endif
