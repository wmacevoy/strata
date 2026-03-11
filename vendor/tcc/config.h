/* Minimal config.h for vendored TCC in Strata.
 * Target architecture is auto-detected from host platform defines in tcc.h.
 * ONE_SOURCE mode: libtcc.c includes all other .c files.
 */

#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0

/* Suppress TCC's built-in lib path — we don't install runtime libs */
#define CONFIG_TCCDIR "/dev/null"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
