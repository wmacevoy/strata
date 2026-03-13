// ============================================================
// prolog_core.c — Native term layer implementation
// ============================================================

#include "prolog_core.h"
#include <string.h>
#include <stdlib.h>

// ── Lifecycle ───────────────────────────────────────────────

void pc_init(PrologCore *pc) {
    memset(pc, 0, sizeof(PrologCore));
    pc->atoms.count = 0;
    pc->compounds.next = 0;
    pc_subst_reset(pc);
    pc->var_counter = 0;

    // Pre-intern the empty list atom "[]"
    pc_intern_atom(pc, "[]");  // will be atom 0
}

void pc_reset_compounds(PrologCore *pc) {
    pc->compounds.next = 0;
}

// ── Atom interning ──────────────────────────────────────────

uint32_t pc_intern_atom(PrologCore *pc, const char *name) {
    // Linear scan — fine for <4096 atoms
    for (uint32_t i = 0; i < pc->atoms.count; i++) {
        if (strcmp(pc->atoms.names[i], name) == 0) return i;
    }
    if (pc->atoms.count >= MAX_ATOMS) return 0; // overflow: return "[]"
    uint32_t id = pc->atoms.count++;
    strncpy(pc->atoms.names[id], name, MAX_ATOM_LEN - 1);
    pc->atoms.names[id][MAX_ATOM_LEN - 1] = '\0';
    return id;
}

const char* pc_atom_name(PrologCore *pc, uint32_t atom_id) {
    if (atom_id >= pc->atoms.count) return "?";
    return pc->atoms.names[atom_id];
}

// ── Term construction ───────────────────────────────────────

Term pc_make_atom(PrologCore *pc, const char *name) {
    return TERM_ATOM(pc_intern_atom(pc, name));
}

Term pc_make_var(PrologCore *pc, uint32_t var_id) {
    return TERM_VAR(var_id);
}

Term pc_make_num(int32_t value) {
    return TERM_NUM(value);
}

Term pc_make_compound(PrologCore *pc, uint32_t functor_atom_id,
                      uint32_t arity, const Term *args) {
    uint32_t idx = pc->compounds.next;
    if (idx + 2 + arity > MAX_COMPOUNDS) return TERM_NONE;

    pc->compounds.data[idx]     = functor_atom_id;
    pc->compounds.data[idx + 1] = arity;
    for (uint32_t i = 0; i < arity; i++) {
        pc->compounds.data[idx + 2 + i] = args[i];
    }
    pc->compounds.next = idx + 2 + arity;
    return TERM_COMPOUND(idx);
}

Term pc_make_list(PrologCore *pc, uint32_t count, const Term *items, Term tail) {
    uint32_t dot_atom = pc_intern_atom(pc, ".");
    Term result = (tail != TERM_NONE) ? tail : TERM_ATOM(0); // atom 0 = "[]"
    // Build from right to left
    for (int i = (int)count - 1; i >= 0; i--) {
        Term args[2] = { items[i], result };
        result = pc_make_compound(pc, dot_atom, 2, args);
        if (result == TERM_NONE) return TERM_NONE;
    }
    return result;
}

// ── Compound access ─────────────────────────────────────────

uint32_t pc_compound_functor(PrologCore *pc, Term t) {
    if (TERM_TAG(t) != TAG_COMPOUND) return 0;
    uint32_t idx = COMPOUND_ID(t);
    return pc->compounds.data[idx];
}

uint32_t pc_compound_arity(PrologCore *pc, Term t) {
    if (TERM_TAG(t) != TAG_COMPOUND) return 0;
    uint32_t idx = COMPOUND_ID(t);
    return pc->compounds.data[idx + 1];
}

Term pc_compound_arg(PrologCore *pc, Term t, uint32_t i) {
    if (TERM_TAG(t) != TAG_COMPOUND) return TERM_NONE;
    uint32_t idx = COMPOUND_ID(t);
    uint32_t arity = pc->compounds.data[idx + 1];
    if (i >= arity) return TERM_NONE;
    return pc->compounds.data[idx + 2 + i];
}

// ── Substitution / trail ────────────────────────────────────

void pc_subst_reset(PrologCore *pc) {
    for (uint32_t i = 0; i < MAX_VARS; i++) {
        pc->subst.bindings[i] = TERM_NONE;
    }
    pc->subst.trail_top = 0;
}

uint32_t pc_trail_checkpoint(PrologCore *pc) {
    return pc->subst.trail_top;
}

void pc_trail_undo(PrologCore *pc, uint32_t checkpoint) {
    while (pc->subst.trail_top > checkpoint) {
        pc->subst.trail_top--;
        TrailEntry *e = &pc->subst.trail[pc->subst.trail_top];
        pc->subst.bindings[e->var_id] = e->old_value;
    }
}

void pc_bind(PrologCore *pc, uint32_t var_id, Term value) {
    if (var_id >= MAX_VARS || pc->subst.trail_top >= MAX_TRAIL) return;
    // Record old value on trail
    TrailEntry *e = &pc->subst.trail[pc->subst.trail_top++];
    e->var_id   = var_id;
    e->old_value = pc->subst.bindings[var_id];
    // Set new binding
    pc->subst.bindings[var_id] = value;
}

// ── Walk ────────────────────────────────────────────────────

Term pc_walk(PrologCore *pc, Term t) {
    while (TERM_TAG(t) == TAG_VAR) {
        uint32_t vid = VAR_ID(t);
        if (vid >= MAX_VARS) break;  /* out of bounds: treat as unbound */
        Term bound = pc->subst.bindings[vid];
        if (bound == TERM_NONE) break;
        t = bound;
    }
    return t;
}

// ── DeepWalk ────────────────────────────────────────────────

Term pc_deep_walk(PrologCore *pc, Term t) {
    t = pc_walk(pc, t);
    if (TERM_TAG(t) != TAG_COMPOUND) return t;

    uint32_t idx = COMPOUND_ID(t);
    uint32_t functor = pc->compounds.data[idx];
    uint32_t arity   = pc->compounds.data[idx + 1];

    // Check if any arg actually changed (avoid allocation)
    bool changed = false;
    Term resolved[64]; // stack buffer for small arities
    Term *args = (arity <= 64) ? resolved : (Term*)malloc(arity * sizeof(Term));

    for (uint32_t i = 0; i < arity; i++) {
        Term orig = pc->compounds.data[idx + 2 + i];
        args[i] = pc_deep_walk(pc, orig);
        if (args[i] != orig) changed = true;
    }

    Term result;
    if (!changed) {
        result = t; // no allocation needed!
    } else {
        result = pc_make_compound(pc, functor, arity, args);
    }

    if (arity > 64) free(args);
    return result;
}

// ── Unification ─────────────────────────────────────────────

bool pc_unify(PrologCore *pc, Term a, Term b) {
    a = pc_walk(pc, a);
    b = pc_walk(pc, b);

    if (a == b) return true;  // same term — instant success

    uint32_t tag_a = TERM_TAG(a);
    uint32_t tag_b = TERM_TAG(b);

    // Variable binding
    if (tag_a == TAG_VAR) {
        pc_bind(pc, VAR_ID(a), b);
        return true;
    }
    if (tag_b == TAG_VAR) {
        pc_bind(pc, VAR_ID(b), a);
        return true;
    }

    // Atoms: already handled by a == b check (same interned ID)
    if (tag_a == TAG_ATOM && tag_b == TAG_ATOM) return false; // different atoms
    if (tag_a == TAG_NUM  && tag_b == TAG_NUM)  return false;  // different nums

    // Compound terms
    if (tag_a == TAG_COMPOUND && tag_b == TAG_COMPOUND) {
        uint32_t idx_a = COMPOUND_ID(a);
        uint32_t idx_b = COMPOUND_ID(b);
        uint32_t func_a = pc->compounds.data[idx_a];
        uint32_t func_b = pc->compounds.data[idx_b];
        uint32_t arity_a = pc->compounds.data[idx_a + 1];
        uint32_t arity_b = pc->compounds.data[idx_b + 1];

        if (func_a != func_b || arity_a != arity_b) return false;

        for (uint32_t i = 0; i < arity_a; i++) {
            if (!pc_unify(pc,
                          pc->compounds.data[idx_a + 2 + i],
                          pc->compounds.data[idx_b + 2 + i])) {
                return false;
            }
        }
        return true;
    }

    return false; // type mismatch
}

// ── Fresh variable renaming ─────────────────────────────────

static Term rename_term(PrologCore *pc, Term t, uint32_t var_base) {
    switch (TERM_TAG(t)) {
        case TAG_VAR:
            return TERM_VAR(VAR_ID(t) + var_base);
        case TAG_COMPOUND: {
            uint32_t idx = COMPOUND_ID(t);
            uint32_t functor = pc->compounds.data[idx];
            uint32_t arity   = pc->compounds.data[idx + 1];
            Term args[64] = {0};
            Term *buf = (arity <= 64) ? args : (Term*)malloc(arity * sizeof(Term));
            for (uint32_t i = 0; i < arity; i++) {
                buf[i] = rename_term(pc, pc->compounds.data[idx + 2 + i], var_base);
            }
            Term result = pc_make_compound(pc, functor, arity, buf);
            if (arity > 64) free(buf);
            return result;
        }
        default:
            return t; // atoms and nums don't change
    }
}

uint32_t pc_fresh_clause(PrologCore *pc,
                         const Term *template_terms, uint32_t template_count,
                         uint32_t var_base,
                         Term *out_terms, uint32_t out_max) {
    uint32_t count = (template_count < out_max) ? template_count : out_max;
    for (uint32_t i = 0; i < count; i++) {
        out_terms[i] = rename_term(pc, template_terms[i], var_base);
    }
    return count;
}

// ── Term inspection ─────────────────────────────────────────

int      pc_term_tag(Term t)            { return TERM_TAG(t); }
int32_t  pc_term_num_value(Term t)      { return NUM_VALUE(t); }
uint32_t pc_term_atom_id(Term t)        { return ATOM_ID(t); }
uint32_t pc_term_var_id(Term t)         { return VAR_ID(t); }
uint32_t pc_term_compound_id(Term t)    { return COMPOUND_ID(t); }
