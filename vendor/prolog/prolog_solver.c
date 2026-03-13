/*
 * prolog_solver.c — Clause database + backtracking solver.
 *
 * Builds on prolog_core.  ~250 lines.
 * Handles: clause matching, backtracking, essential builtins.
 */

#include "prolog_solver.h"
#include <string.h>

/* ── Interned builtin atoms (cached on init) ─────────────── */

static struct {
    uint32_t comma, not_, naf, eq, neq, is_, gt, lt, ge, le,
             eqn, nen, true_, fail_, assert_, assertz, retract,
             findall, member, semicolon, arrow, dot, nil;
} B;  /* populated per-solver in ps_init */

static void intern_builtins(PrologCore *pc) {
    B.comma    = pc_intern_atom(pc, ",");
    B.not_     = pc_intern_atom(pc, "not");
    B.naf      = pc_intern_atom(pc, "\\+");
    B.eq       = pc_intern_atom(pc, "=");
    B.neq      = pc_intern_atom(pc, "\\=");
    B.is_      = pc_intern_atom(pc, "is");
    B.gt       = pc_intern_atom(pc, ">");
    B.lt       = pc_intern_atom(pc, "<");
    B.ge       = pc_intern_atom(pc, ">=");
    B.le       = pc_intern_atom(pc, "=<");
    B.eqn      = pc_intern_atom(pc, "=:=");
    B.nen      = pc_intern_atom(pc, "=\\=");
    B.true_    = pc_intern_atom(pc, "true");
    B.fail_    = pc_intern_atom(pc, "fail");
    B.assert_  = pc_intern_atom(pc, "assert");
    B.assertz  = pc_intern_atom(pc, "assertz");
    B.retract  = pc_intern_atom(pc, "retract");
    B.findall  = pc_intern_atom(pc, "findall");
    B.member   = pc_intern_atom(pc, "member");
    B.semicolon= pc_intern_atom(pc, ";");
    B.arrow    = pc_intern_atom(pc, "->");
    B.dot      = pc_intern_atom(pc, ".");
    B.nil      = pc_intern_atom(pc, "[]");
}

/* ── Lifecycle ───────────────────────────────────────────── */

void ps_init(ps_solver *s) {
    memset(s, 0, sizeof(ps_solver));
    pc_init(&s->core);
    intern_builtins(&s->core);
    s->compound_watermark = s->core.compounds.next;
}

void ps_mark_clauses(ps_solver *s) {
    s->compound_watermark = s->core.compounds.next;
}

void ps_query_reset(ps_solver *s) {
    /* Don't reset compound pool — query goals reference it.
     * The pool is large (64K entries), fine for a den's lifetime. */
    pc_subst_reset(&s->core);
    s->core.var_counter = 0;
}

/* ── Clause management ───────────────────────────────────── */

void ps_add_clause(ps_solver *s, Term head,
                   const Term *body, uint32_t body_len) {
    if (s->clause_count >= PS_MAX_CLAUSES) return;
    ps_clause *c = &s->clauses[s->clause_count++];
    c->terms[0] = head;
    c->len = 1 + body_len;
    if (c->len > PS_MAX_CLAUSE_LEN) c->len = PS_MAX_CLAUSE_LEN;
    for (uint32_t i = 0; i < body_len && i + 1 < PS_MAX_CLAUSE_LEN; i++)
        c->terms[i + 1] = body[i];
    /* Update watermark to include clause compounds */
    s->compound_watermark = s->core.compounds.next;
}

bool ps_retract_first(ps_solver *s, Term pattern) {
    PrologCore *pc = &s->core;
    pc_subst_reset(pc);  /* clean slate for matching */
    for (uint32_t i = 0; i < s->clause_count; i++) {
        uint32_t cp = pc_trail_checkpoint(pc);
        /* Fresh copy for unification */
        Term fresh[PS_MAX_CLAUSE_LEN];
        uint32_t base = ++pc->var_counter * PS_VAR_STRIDE;
        pc_fresh_clause(pc, s->clauses[i].terms, 1, base, fresh, 1);
        if (pc_unify(pc, pattern, fresh[0])) {
            pc_trail_undo(pc, cp);
            /* Remove clause by shifting */
            for (uint32_t j = i; j + 1 < s->clause_count; j++)
                s->clauses[j] = s->clauses[j + 1];
            s->clause_count--;
            return true;
        }
        pc_trail_undo(pc, cp);
    }
    return false;
}

/* ── Arithmetic evaluator ────────────────────────────────── */

static bool eval_arith(PrologCore *pc, Term t, int32_t *result) {
    t = pc_deep_walk(pc, t);
    if (TERM_TAG(t) == TAG_NUM) {
        *result = NUM_VALUE(t);
        return true;
    }
    if (TERM_TAG(t) != TAG_COMPOUND) return false;

    uint32_t idx = COMPOUND_ID(t);
    uint32_t func = pc->compounds.data[idx];
    uint32_t arity = pc->compounds.data[idx + 1];

    if (arity == 2) {
        int32_t a, b;
        if (!eval_arith(pc, pc->compounds.data[idx + 2], &a)) return false;
        if (!eval_arith(pc, pc->compounds.data[idx + 3], &b)) return false;
        const char *name = pc_atom_name(pc, func);
        if (name[0] == '+' && name[1] == '\0') { *result = a + b; return true; }
        if (name[0] == '-' && name[1] == '\0') { *result = a - b; return true; }
        if (name[0] == '*' && name[1] == '\0') { *result = a * b; return true; }
        if (strcmp(name, "//") == 0 && b != 0) { *result = a / b; return true; }
        if (strcmp(name, "mod") == 0 && b != 0) { *result = a % b; return true; }
    }
    if (arity == 1) {
        int32_t a;
        if (!eval_arith(pc, pc->compounds.data[idx + 2], &a)) return false;
        const char *name = pc_atom_name(pc, func);
        if (name[0] == '-' && name[1] == '\0') { *result = -a; return true; }
    }
    return false;
}

/* ── Solver ──────────────────────────────────────────────── */

/* Goal stack: flat array to avoid recursion overhead */
#define MAX_GOALS 256

typedef struct {
    ps_solver     *solver;
    ps_solution_fn cb;
    void          *user_data;
    int            count;
    int            limit;
    bool           stopped;
} solve_ctx;

/* findall collection callback (must be at file scope) */
typedef struct {
    Term tmpl;
    Term results[PS_MAX_RESULTS];
    int  count;
} fa_collect_t;

static bool fa_collect_cb(PrologCore *pc, void *ud) {
    fa_collect_t *fa = (fa_collect_t *)ud;
    if (fa->count < PS_MAX_RESULTS)
        fa->results[fa->count++] = pc_deep_walk(pc, fa->tmpl);
    return fa->count < PS_MAX_RESULTS;
}

static void solve(solve_ctx *sx, Term *goals, uint32_t ngoals, int depth);

static void solve(solve_ctx *sx, Term *goals, uint32_t ngoals, int depth) {
    if (sx->stopped || depth > PS_MAX_DEPTH) return;
    if (ngoals == 0) {
        sx->count++;
        if (sx->cb) {
            if (!sx->cb(&sx->solver->core, sx->user_data))
                sx->stopped = true;
        }
        if (sx->limit > 0 && sx->count >= sx->limit)
            sx->stopped = true;
        return;
    }

    PrologCore *pc = &sx->solver->core;
    Term goal = pc_deep_walk(pc, goals[0]);
    Term *rest = goals + 1;
    uint32_t nrest = ngoals - 1;

    /* ── Builtins ──────────────────────────────────────── */

    if (TERM_TAG(goal) == TAG_ATOM) {
        uint32_t aid = ATOM_ID(goal);
        if (aid == B.true_) { solve(sx, rest, nrest, depth + 1); return; }
        if (aid == B.fail_) return;
        /* Other atoms: fall through to user clause matching */
        goto user_clauses;
    }

    if (TERM_TAG(goal) != TAG_COMPOUND) return;

    uint32_t idx = COMPOUND_ID(goal);
    uint32_t func = pc->compounds.data[idx];
    uint32_t arity = pc->compounds.data[idx + 1];

    /* ,/2 conjunction */
    if (func == B.comma && arity == 2) {
        Term expanded[MAX_GOALS];
        expanded[0] = pc->compounds.data[idx + 2];
        expanded[1] = pc->compounds.data[idx + 3];
        uint32_t n = 2;
        for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
            expanded[n++] = rest[i];
        solve(sx, expanded, n, depth + 1);
        return;
    }

    /* not/1, \+/1 */
    if ((func == B.not_ || func == B.naf) && arity == 1) {
        Term inner = pc_deep_walk(pc, pc->compounds.data[idx + 2]);
        uint32_t cp = pc_trail_checkpoint(pc);
        uint32_t saved_vc = pc->var_counter;  /* inner vars are temporary */
        solve_ctx inner_sx = *sx;
        inner_sx.cb = NULL;
        inner_sx.count = 0;
        inner_sx.limit = 1;
        inner_sx.stopped = false;
        solve(&inner_sx, &inner, 1, depth + 1);
        pc_trail_undo(pc, cp);
        pc->var_counter = saved_vc;  /* restore — inner vars fully undone */
        if (inner_sx.count == 0)
            solve(sx, rest, nrest, depth + 1);
        return;
    }

    /* =/2 */
    if (func == B.eq && arity == 2) {
        uint32_t cp = pc_trail_checkpoint(pc);
        if (pc_unify(pc, pc->compounds.data[idx + 2], pc->compounds.data[idx + 3]))
            solve(sx, rest, nrest, depth + 1);
        if (sx->stopped) return;
        pc_trail_undo(pc, cp);
        return;
    }

    /* \=/2 */
    if (func == B.neq && arity == 2) {
        uint32_t cp = pc_trail_checkpoint(pc);
        bool unified = pc_unify(pc, pc->compounds.data[idx + 2], pc->compounds.data[idx + 3]);
        pc_trail_undo(pc, cp);
        if (!unified)
            solve(sx, rest, nrest, depth + 1);
        return;
    }

    /* is/2 */
    if (func == B.is_ && arity == 2) {
        int32_t val;
        if (eval_arith(pc, pc->compounds.data[idx + 3], &val)) {
            Term num_term = pc_make_num(val);
            uint32_t cp = pc_trail_checkpoint(pc);
            if (pc_unify(pc, pc->compounds.data[idx + 2], num_term))
                solve(sx, rest, nrest, depth + 1);
            if (sx->stopped) return;
            pc_trail_undo(pc, cp);
        }
        return;
    }

    /* Comparison operators */
    if (arity == 2) {
        int cmp_type = 0;  /* 0=not a comparison */
        if (func == B.gt) cmp_type = 1;
        else if (func == B.lt) cmp_type = 2;
        else if (func == B.ge) cmp_type = 3;
        else if (func == B.le) cmp_type = 4;
        else if (func == B.eqn) cmp_type = 5;
        else if (func == B.nen) cmp_type = 6;

        if (cmp_type) {
            int32_t a, b;
            if (eval_arith(pc, pc->compounds.data[idx + 2], &a) &&
                eval_arith(pc, pc->compounds.data[idx + 3], &b)) {
                bool ok = false;
                switch (cmp_type) {
                    case 1: ok = a > b; break;
                    case 2: ok = a < b; break;
                    case 3: ok = a >= b; break;
                    case 4: ok = a <= b; break;
                    case 5: ok = a == b; break;
                    case 6: ok = a != b; break;
                }
                if (ok) solve(sx, rest, nrest, depth + 1);
            }
            return;
        }
    }

    /* assert/1, assertz/1 */
    if ((func == B.assert_ || func == B.assertz) && arity == 1) {
        Term term = pc_deep_walk(pc, pc->compounds.data[idx + 2]);
        ps_add_clause(sx->solver, term, NULL, 0);
        solve(sx, rest, nrest, depth + 1);
        return;
    }

    /* retract/1 */
    if (func == B.retract && arity == 1) {
        Term term = pc_deep_walk(pc, pc->compounds.data[idx + 2]);
        if (ps_retract_first(sx->solver, term))
            solve(sx, rest, nrest, depth + 1);
        return;
    }

    /* member/2 */
    if (func == B.member && arity == 2) {
        Term elem = pc->compounds.data[idx + 2];
        Term list = pc_deep_walk(pc, pc->compounds.data[idx + 3]);
        while (TERM_TAG(list) == TAG_COMPOUND) {
            uint32_t lidx = COMPOUND_ID(list);
            if (pc->compounds.data[lidx] != B.dot ||
                pc->compounds.data[lidx + 1] != 2) break;
            uint32_t cp = pc_trail_checkpoint(pc);
            if (pc_unify(pc, elem, pc->compounds.data[lidx + 2]))
                solve(sx, rest, nrest, depth + 1);
            if (sx->stopped) return;
            pc_trail_undo(pc, cp);
            list = pc_deep_walk(pc, pc->compounds.data[lidx + 3]);
        }
        return;
    }

    /* ;/2 disjunction / if-then-else */
    if (func == B.semicolon && arity == 2) {
        Term left = pc_deep_walk(pc, pc->compounds.data[idx + 2]);
        Term right = pc_deep_walk(pc, pc->compounds.data[idx + 3]);

        /* Check if left is ->/2 (if-then-else) */
        if (TERM_TAG(left) == TAG_COMPOUND) {
            uint32_t lidx = COMPOUND_ID(left);
            if (pc->compounds.data[lidx] == B.arrow &&
                pc->compounds.data[lidx + 1] == 2) {
                /* (Cond -> Then ; Else) */
                Term cond = pc->compounds.data[lidx + 2];
                Term then = pc->compounds.data[lidx + 3];

                /* Try cond; if succeeds, commit to then */
                uint32_t cp = pc_trail_checkpoint(pc);
                solve_ctx cond_sx = *sx;
                cond_sx.count = 0;
                cond_sx.limit = 1;
                cond_sx.stopped = false;
                /* Build then + rest */
                Term then_goals[MAX_GOALS];
                then_goals[0] = then;
                uint32_t n = 1;
                for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
                    then_goals[n++] = rest[i];
                cond_sx.cb = sx->cb;
                cond_sx.user_data = sx->user_data;

                /* Solve cond, then on success solve then+rest */
                /* We need a special handler: solve cond, if solution found, solve then+rest */
                bool cond_found = false;
                solve_ctx probe = *sx;
                probe.cb = NULL;
                probe.count = 0;
                probe.limit = 1;
                probe.stopped = false;
                solve(&probe, &cond, 1, depth + 1);
                if (probe.count > 0) {
                    /* Cond succeeded (bindings still active) — solve then+rest */
                    solve(sx, then_goals, n, depth + 1);
                    cond_found = true;
                }
                if (!cond_found) {
                    pc_trail_undo(pc, cp);
                    /* Else branch */
                    Term else_goals[MAX_GOALS];
                    else_goals[0] = right;
                    uint32_t m = 1;
                    for (uint32_t i = 0; i < nrest && m < MAX_GOALS; i++)
                        else_goals[m++] = rest[i];
                    solve(sx, else_goals, m, depth + 1);
                }
                return;
            }
        }
        /* Plain disjunction: try left, then right */
        {
            uint32_t cp = pc_trail_checkpoint(pc);
            Term lgoals[MAX_GOALS];
            lgoals[0] = left;
            uint32_t n = 1;
            for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
                lgoals[n++] = rest[i];
            solve(sx, lgoals, n, depth + 1);
            if (sx->stopped) return;
            pc_trail_undo(pc, cp);

            Term rgoals[MAX_GOALS];
            rgoals[0] = right;
            n = 1;
            for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
                rgoals[n++] = rest[i];
            solve(sx, rgoals, n, depth + 1);
        }
        return;
    }

    /* ->/2 (standalone, without ;) */
    if (func == B.arrow && arity == 2) {
        Term cond = pc->compounds.data[idx + 2];
        Term then = pc->compounds.data[idx + 3];
        solve_ctx probe = *sx;
        probe.cb = NULL;
        probe.count = 0;
        probe.limit = 1;
        probe.stopped = false;
        solve(&probe, &cond, 1, depth + 1);
        if (probe.count > 0) {
            Term then_goals[MAX_GOALS];
            then_goals[0] = then;
            uint32_t n = 1;
            for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
                then_goals[n++] = rest[i];
            solve(sx, then_goals, n, depth + 1);
        }
        return;
    }

    /* findall/3 */
    if (func == B.findall && arity == 3) {
        Term tmpl = pc->compounds.data[idx + 2];
        Term query_goal = pc_deep_walk(pc, pc->compounds.data[idx + 3]);
        Term bag = pc->compounds.data[idx + 4];

        /* Collect solutions using the findall callback */
        fa_collect_t fa_data;
        fa_data.tmpl = tmpl;
        fa_data.count = 0;

        uint32_t cp = pc_trail_checkpoint(pc);
        uint32_t saved_vc = pc->var_counter;  /* inner vars are temporary */
        solve_ctx fa_sx = *sx;
        fa_sx.cb = fa_collect_cb;
        fa_sx.user_data = &fa_data;
        fa_sx.count = 0;
        fa_sx.limit = PS_MAX_RESULTS;
        fa_sx.stopped = false;
        solve(&fa_sx, &query_goal, 1, depth + 1);
        pc_trail_undo(pc, cp);
        pc->var_counter = saved_vc;  /* restore — inner vars fully undone */

        /* Build result list */
        Term result_list = pc_make_list(pc, (uint32_t)fa_data.count,
                                        fa_data.results, TERM_NONE);
        uint32_t cp2 = pc_trail_checkpoint(pc);
        if (pc_unify(pc, bag, result_list))
            solve(sx, rest, nrest, depth + 1);
        if (sx->stopped) return;
        pc_trail_undo(pc, cp2);
        return;
    }

    /* ── User clauses ────────────────────────────────────── */
user_clauses:

    for (uint32_t ci = 0; ci < sx->solver->clause_count; ci++) {
        if (sx->stopped) return;
        ps_clause *cl = &sx->solver->clauses[ci];

        uint32_t cp = pc_trail_checkpoint(pc);
        uint32_t pool_save = pc->compounds.next;

        /* Fresh copy with renamed variables */
        Term fresh[PS_MAX_CLAUSE_LEN];
        uint32_t base = ++pc->var_counter * PS_VAR_STRIDE;
        pc_fresh_clause(pc, cl->terms, cl->len, base, fresh, PS_MAX_CLAUSE_LEN);

        if (pc_unify(pc, goal, fresh[0])) {
            /* Build body + rest goals */
            uint32_t body_len = cl->len - 1;
            Term new_goals[MAX_GOALS];
            uint32_t n = 0;
            for (uint32_t i = 0; i < body_len && n < MAX_GOALS; i++)
                new_goals[n++] = fresh[i + 1];
            for (uint32_t i = 0; i < nrest && n < MAX_GOALS; i++)
                new_goals[n++] = rest[i];
            solve(sx, new_goals, n, depth + 1);
        }

        pc_trail_undo(pc, cp);
        /* Don't reset compound pool here — deep_walk results may reference it */
        (void)pool_save;
    }
}

/* ── Public API ──────────────────────────────────────────── */

int ps_query(ps_solver *s, Term goal, ps_solution_fn cb,
             void *user_data, int limit) {
    ps_query_reset(s);
    solve_ctx sx;
    sx.solver = s;
    sx.cb = cb;
    sx.user_data = user_data;
    sx.count = 0;
    sx.limit = limit;
    sx.stopped = false;
    solve(&sx, &goal, 1, 0);
    return sx.count;
}

/* Query-first helper */
typedef struct {
    Term goal;
    Term result;
} qf_data;

static bool qf_cb(PrologCore *pc, void *ud) {
    qf_data *d = (qf_data *)ud;
    d->result = pc_deep_walk(pc, d->goal);
    return false;  /* stop after first */
}

Term ps_query_first(ps_solver *s, Term goal) {
    qf_data d;
    d.goal = goal;
    d.result = TERM_NONE;
    ps_query(s, goal, qf_cb, &d, 1);
    return d.result;
}

/* Query-all helper */
typedef struct {
    Term  goal;
    Term *results;
    int   count;
    int   max;
} qa_data;

static bool qa_cb(PrologCore *pc, void *ud) {
    qa_data *d = (qa_data *)ud;
    if (d->count < d->max)
        d->results[d->count++] = pc_deep_walk(pc, d->goal);
    return d->count < d->max;
}

int ps_query_all(ps_solver *s, Term goal, Term *results, int max_results) {
    qa_data d;
    d.goal = goal;
    d.results = results;
    d.count = 0;
    d.max = max_results;
    ps_query(s, goal, qa_cb, &d, max_results);
    return d.count;
}
