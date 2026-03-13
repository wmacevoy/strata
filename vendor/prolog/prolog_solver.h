/*
 * prolog_solver.h — Clause database + backtracking solver.
 *
 * Builds on prolog_core (terms, unification, trail).
 * Provides: clause storage, goal resolution, essential builtins.
 *
 * All state in ps_solver — no globals, fork-safe.
 */
#ifndef PROLOG_SOLVER_H
#define PROLOG_SOLVER_H

#include "prolog_core.h"

/* ── Limits ──────────────────────────────────────────────── */

#define PS_MAX_CLAUSES     1024
#define PS_MAX_CLAUSE_LEN   32   /* max terms per clause: head + body goals */
#define PS_MAX_DEPTH        300
#define PS_MAX_RESULTS       50
#define PS_VAR_STRIDE       100  /* max distinct vars per clause (for renaming) */

/* ── Clause ──────────────────────────────────────────────── */

typedef struct {
    Term     terms[PS_MAX_CLAUSE_LEN];  /* [0]=head, [1..]=body goals */
    uint32_t len;                        /* 1 + body_count */
} ps_clause;

/* ── Solution callback ───────────────────────────────────── */

/* Called for each solution found.  pc has bindings still active
 * so the caller can deep_walk the query goal.
 * Return true to continue searching, false to stop. */
typedef bool (*ps_solution_fn)(PrologCore *pc, void *user_data);

/* ── Solver ──────────────────────────────────────────────── */

typedef struct {
    PrologCore  core;
    ps_clause   clauses[PS_MAX_CLAUSES];
    uint32_t    clause_count;
    uint32_t    compound_watermark;  /* pool.next after clause loading */
} ps_solver;

/* ── API ─────────────────────────────────────────────────── */

/* Initialize solver (zeros everything, interns base atoms). */
void ps_init(ps_solver *s);

/* Add a clause: head with optional body goals.
 * body can be NULL if body_len == 0 (fact). */
void ps_add_clause(ps_solver *s, Term head,
                   const Term *body, uint32_t body_len);

/* Remove first clause whose head unifies with pattern.
 * Returns true if a clause was removed. */
bool ps_retract_first(ps_solver *s, Term pattern);

/* Mark current compound pool position as the clause watermark.
 * Call after loading all static clauses, before querying. */
void ps_mark_clauses(ps_solver *s);

/* Reset compound pool and substitution for a fresh query.
 * Preserves compounds below the watermark (clause data). */
void ps_query_reset(ps_solver *s);

/* Query: find all solutions (up to limit).
 * Calls cb for each solution.  Returns number of solutions found. */
int ps_query(ps_solver *s, Term goal, ps_solution_fn cb,
             void *user_data, int limit);

/* Query first: returns deep-walked goal on first solution,
 * or TERM_NONE if no solution. */
Term ps_query_first(ps_solver *s, Term goal);

/* Convenience: query and collect deep-walked results into array.
 * Returns number of results stored. */
int ps_query_all(ps_solver *s, Term goal, Term *results,
                 int max_results);

#endif /* PROLOG_SOLVER_H */
