/*
 * reactive_prolog.c — Reactive Prolog engine.
 *
 * Connects prolog_solver to reactive_core via a generation signal.
 * ~100 lines.
 */

#include "reactive_prolog.h"
#include <string.h>

/* ── Query memo function ─────────────────────────────────── */

/* Each reactive query is a memo whose function:
 * 1. Reads the generation signal (establishes dependency)
 * 2. Builds a fresh goal via goal_fn
 * 3. Runs ps_query_first
 * 4. Stores the result
 *
 * The memo's int return value is the query index (for lookup). */

static int query_memo_fn(void *ud) {
    rp_engine *rp = (rp_engine *)ud;
    /* Find which query this memo belongs to.
     * We use the observer's computation ID to find the query. */
    rx_comp *current = rp->reactive.observer;
    int qi = -1;
    for (uint32_t i = 0; i < rp->query_count; i++) {
        if (rp->query_comps[i] >= 0 &&
            &rp->reactive.comps[rp->query_comps[i]] == current) {
            qi = (int)i;
            break;
        }
    }
    if (qi < 0) return -1;

    /* Read generation signal to track dependency */
    rx_signal_read(&rp->reactive, rp->gen_signal);

    /* Build goal */
    Term goal = rp->goal_fns[qi](&rp->solver, rp->goal_user[qi]);
    rp->queries[qi].goal = goal;

    /* Run query */
    rp->queries[qi].result = ps_query_first(&rp->solver, goal);
    return qi;
}

/* ── API ─────────────────────────────────────────────────── */

void rp_init(rp_engine *rp) {
    memset(rp, 0, sizeof(rp_engine));
    ps_init(&rp->solver);
    rx_init(&rp->reactive);
    rp->gen_signal = rx_signal_create(&rp->reactive, 0);
}

ps_solver *rp_solver(rp_engine *rp) {
    return &rp->solver;
}

PrologCore *rp_core(rp_engine *rp) {
    return &rp->solver.core;
}

void rp_bump(rp_engine *rp) {
    int gen = rx_signal_read(&rp->reactive, rp->gen_signal);
    rx_signal_write(&rp->reactive, rp->gen_signal, gen + 1);
}

int rp_query_create(rp_engine *rp, rp_goal_fn goal_fn, void *user_data) {
    if (rp->query_count >= RP_MAX_QUERIES) return -1;
    int qi = (int)rp->query_count++;
    rp->queries[qi].active = true;
    rp->queries[qi].result = TERM_NONE;
    rp->goal_fns[qi] = goal_fn;
    rp->goal_user[qi] = user_data;

    /* Pre-set comp ID before rx_memo_create, which runs comp_run
     * internally — query_memo_fn needs this to identify itself. */
    rp->query_comps[qi] = (int)rp->reactive.comp_count;
    rx_memo_create(&rp->reactive, query_memo_fn, rp);
    return qi;
}

Term rp_query_read(rp_engine *rp, int query_id) {
    if (query_id < 0 || (uint32_t)query_id >= rp->query_count) return TERM_NONE;
    /* Reading the memo ensures it's up to date */
    rx_memo_read(&rp->reactive, rp->query_comps[query_id]);
    return rp->queries[query_id].result;
}

void rp_assert(rp_engine *rp, Term fact) {
    ps_add_clause(&rp->solver, fact, NULL, 0);
    rp_bump(rp);
}

bool rp_retract(rp_engine *rp, Term pattern) {
    bool ok = ps_retract_first(&rp->solver, pattern);
    if (ok) rp_bump(rp);
    return ok;
}

void rp_update_sensor(rp_engine *rp, const char *sensor_name, const char *value) {
    PrologCore *pc = &rp->solver.core;
    uint32_t sensor_atom = pc_intern_atom(pc, "sensor");

    /* Retract sensor(Name, _) */
    Term args_old[2] = {
        pc_make_atom(pc, sensor_name),
        pc_make_var(pc, 0)  /* wildcard variable */
    };
    Term old_fact = pc_make_compound(pc, sensor_atom, 2, args_old);
    ps_retract_first(&rp->solver, old_fact);

    /* Assert sensor(Name, Value) */
    Term args_new[2] = {
        pc_make_atom(pc, sensor_name),
        pc_make_atom(pc, value)
    };
    Term new_fact = pc_make_compound(pc, sensor_atom, 2, args_new);
    ps_add_clause(&rp->solver, new_fact, NULL, 0);

    rp_bump(rp);
}

void rp_batch_begin(rp_engine *rp) {
    rx_batch_begin(&rp->reactive);
}

void rp_batch_end(rp_engine *rp) {
    rx_batch_end(&rp->reactive);
}
