/*
 * reactive_prolog.h — Reactive Prolog engine.
 *
 * Connects prolog_solver to reactive_core.
 * A single generation signal drives all reactive queries.
 * When facts change, bump the generation → all queries recompute.
 *
 * All state in rp_engine — no globals, fork-safe.
 */
#ifndef REACTIVE_PROLOG_H
#define REACTIVE_PROLOG_H

#include "prolog_solver.h"
#include "reactive_core.h"

/* ── Limits ──────────────────────────────────────────────── */

#define RP_MAX_QUERIES 64

/* ── Reactive query slot ─────────────────────────────────── */

typedef struct {
    Term  goal;       /* query goal (rebuilt each time via goal_fn) */
    Term  result;     /* cached deep-walked result */
    bool  active;
} rp_query;

/* ── Goal builder function ───────────────────────────────── */

/* Called to build a fresh goal term for a query.
 * The engine is passed so the function can construct terms. */
typedef Term (*rp_goal_fn)(ps_solver *solver, void *user_data);

/* ── Reactive engine ─────────────────────────────────────── */

typedef struct {
    ps_solver    solver;
    rx_ctx       reactive;
    int          gen_signal;    /* generation counter signal */

    rp_query     queries[RP_MAX_QUERIES];
    rp_goal_fn   goal_fns[RP_MAX_QUERIES];
    void        *goal_user[RP_MAX_QUERIES];
    int          query_comps[RP_MAX_QUERIES]; /* memo computation IDs */
    uint32_t     query_count;
} rp_engine;

/* ── API ─────────────────────────────────────────────────── */

/* Initialize the reactive prolog engine. */
void rp_init(rp_engine *rp);

/* Access the underlying solver (for adding clauses, etc). */
ps_solver *rp_solver(rp_engine *rp);

/* Access the underlying PrologCore (for term construction). */
PrologCore *rp_core(rp_engine *rp);

/* Bump the generation counter.
 * Call after modifying facts (assert/retract/sensor update).
 * All reactive queries recompute. */
void rp_bump(rp_engine *rp);

/* Create a reactive query.
 * goal_fn builds the goal term; it's called on each recomputation.
 * Returns query ID, or -1 on overflow. */
int rp_query_create(rp_engine *rp, rp_goal_fn goal_fn, void *user_data);

/* Read the cached result of a reactive query.
 * Returns the deep-walked result term, or TERM_NONE. */
Term rp_query_read(rp_engine *rp, int query_id);

/* Assert a fact and bump the generation. */
void rp_assert(rp_engine *rp, Term fact);

/* Retract a fact and bump the generation. */
bool rp_retract(rp_engine *rp, Term pattern);

/* Update a sensor-style fact: retract old, assert new, bump once.
 * sensor_name and value are atom names.
 * Equivalent to: retract(sensor(Name,_)), assert(sensor(Name,Value)), bump. */
void rp_update_sensor(rp_engine *rp, const char *sensor_name, const char *value);

/* Batch multiple fact changes, bump once at the end. */
void rp_batch_begin(rp_engine *rp);
void rp_batch_end(rp_engine *rp);

#endif /* REACTIVE_PROLOG_H */
