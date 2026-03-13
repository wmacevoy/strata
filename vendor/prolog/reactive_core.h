/*
 * reactive_core.h — Signal/memo/effect runtime in C.
 *
 * Direct translation of embedded-prolog's reactive.py.
 * Signals hold values, memos cache derived computations,
 * effects run side-effects.  Dependency tracking is automatic.
 *
 * All state lives in a ReactiveCtx — no globals, fork-safe.
 */
#ifndef REACTIVE_CORE_H
#define REACTIVE_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* ── Limits ──────────────────────────────────────────────── */

#define RX_MAX_SIGNALS      256
#define RX_MAX_COMPUTATIONS  256
#define RX_MAX_SUBS_PER_SIG  32   /* max computations per signal */
#define RX_MAX_PENDING       256

/* ── Forward declarations ────────────────────────────────── */

typedef struct rx_ctx    rx_ctx;
typedef struct rx_signal rx_signal;
typedef struct rx_comp   rx_comp;

/* ── Computation function type ───────────────────────────── */

/* User function: receives opaque user_data, returns an int value.
 * For memos, the return value is cached.
 * For effects, the return value is ignored. */
typedef int (*rx_fn)(void *user_data);

/* ── Signal ──────────────────────────────────────────────── */

struct rx_signal {
    int       value;
    rx_comp  *subs[RX_MAX_SUBS_PER_SIG];
    uint32_t  sub_count;
    bool      alive;
};

/* ── Computation (memo or effect) ────────────────────────── */

struct rx_comp {
    rx_fn     fn;
    void     *user_data;
    int       value;      /* cached result (memos only) */
    bool      dirty;
    bool      is_memo;    /* true = memo (lazy), false = effect (eager) */
    bool      alive;
    /* Downstream subscribers (for memo→memo/effect chaining) */
    rx_comp  *subs[RX_MAX_SUBS_PER_SIG];
    uint32_t  sub_count;
};

/* ── Context (all state, no globals) ─────────────────────── */

struct rx_ctx {
    rx_signal  signals[RX_MAX_SIGNALS];
    uint32_t   signal_count;

    rx_comp    comps[RX_MAX_COMPUTATIONS];
    uint32_t   comp_count;

    rx_comp   *observer;           /* currently executing computation */
    int        batch_depth;
    rx_comp   *pending[RX_MAX_PENDING];
    uint32_t   pending_count;
};

/* ── API ─────────────────────────────────────────────────── */

/* Initialize a reactive context (zero state). */
void rx_init(rx_ctx *ctx);

/* Create a signal with an initial value.
 * Returns signal ID (index), or -1 on overflow. */
int rx_signal_create(rx_ctx *ctx, int initial);

/* Read a signal's current value.
 * Tracks dependency if called inside a computation. */
int rx_signal_read(rx_ctx *ctx, int sig_id);

/* Write a signal's value.  Triggers dependent computations. */
void rx_signal_write(rx_ctx *ctx, int sig_id, int value);

/* Create a memo: a lazy computation that caches its result.
 * Returns computation ID, or -1 on overflow.
 * The fn is called immediately to establish initial value + deps. */
int rx_memo_create(rx_ctx *ctx, rx_fn fn, void *user_data);

/* Read a memo's cached value.  Recomputes if dirty.
 * Tracks dependency if called inside another computation. */
int rx_memo_read(rx_ctx *ctx, int comp_id);

/* Create an effect: an eager computation that runs on changes.
 * Returns computation ID, or -1 on overflow.
 * The fn is called immediately. */
int rx_effect_create(rx_ctx *ctx, rx_fn fn, void *user_data);

/* Begin a batch: defer all triggered computations until rx_batch_end. */
void rx_batch_begin(rx_ctx *ctx);

/* End a batch: flush all pending computations (with dedup). */
void rx_batch_end(rx_ctx *ctx);

#endif /* REACTIVE_CORE_H */
