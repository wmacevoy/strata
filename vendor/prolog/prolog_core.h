// ============================================================
// prolog_core.h — Native term layer for Mini Prolog
//
// Provides: arena-allocated terms, interned atoms, trail-based
// substitution, unification, and deepWalk.  The solver loop
// stays in JS — this is just the hot inner layer.
//
// Terms are 32-bit tagged values:
//   bits [31:30] = tag  (0=atom, 1=var, 2=num, 3=compound)
//   bits [29:0]  = payload (index or value)
//
// Designed to compile as:
//   - A shared library (for QuickJS native module)
//   - WASM (via Emscripten or wasi-sdk, for browser/WAMR)
//   - Statically linked into QuickJS
// ============================================================

#ifndef PROLOG_CORE_H
#define PROLOG_CORE_H

#include <stdint.h>
#include <stdbool.h>

// ── Term representation ─────────────────────────────────────

typedef uint32_t Term;

#define TAG_ATOM     0
#define TAG_VAR      1
#define TAG_NUM      2
#define TAG_COMPOUND 3

#define TAG_BITS     2
#define TAG_MASK     0x3
#define PAYLOAD_BITS 30
#define PAYLOAD_MASK 0x3FFFFFFF

#define TERM_TAG(t)     ((t) & TAG_MASK)
#define TERM_PAYLOAD(t) ((t) >> TAG_BITS)
#define MAKE_TERM(tag, payload) (((uint32_t)(payload) << TAG_BITS) | (tag))

#define TERM_NONE 0xFFFFFFFF  // sentinel: no term / failure

// Convenience constructors
#define TERM_ATOM(id)    MAKE_TERM(TAG_ATOM, (id))
#define TERM_VAR(id)     MAKE_TERM(TAG_VAR, (id))
#define TERM_NUM(val)    MAKE_TERM(TAG_NUM, (uint32_t)(int32_t)(val))
#define TERM_COMPOUND(id) MAKE_TERM(TAG_COMPOUND, (id))

// Extract typed payloads
#define ATOM_ID(t)       TERM_PAYLOAD(t)
#define VAR_ID(t)        TERM_PAYLOAD(t)
#define NUM_VALUE(t)     ((int32_t)TERM_PAYLOAD(t))
#define COMPOUND_ID(t)   TERM_PAYLOAD(t)

// ── Limits ──────────────────────────────────────────────────

#define MAX_ATOMS       4096
#define MAX_ATOM_LEN    256
#define MAX_COMPOUNDS   65536   // compound pool entries
#define MAX_VARS        4096
#define MAX_TRAIL       8192
#define MAX_CLAUSE_TERMS 256    // max terms in a single clause template

// ── Atom table ──────────────────────────────────────────────

typedef struct {
    char names[MAX_ATOMS][MAX_ATOM_LEN];
    uint32_t count;
} AtomTable;

// ── Compound pool ───────────────────────────────────────────
// Flat array.  A compound at index i:
//   pool[i]   = functor (atom ID)
//   pool[i+1] = arity
//   pool[i+2 .. i+1+arity] = argument terms

typedef struct {
    uint32_t data[MAX_COMPOUNDS];
    uint32_t next;  // next free index
} CompoundPool;

// ── Trail-based substitution ────────────────────────────────

typedef struct {
    uint32_t var_id;
    Term     old_value;
} TrailEntry;

typedef struct {
    Term        bindings[MAX_VARS];  // var_id → bound term (TERM_NONE = unbound)
    TrailEntry  trail[MAX_TRAIL];
    uint32_t    trail_top;
} Substitution;

// ── Engine state ────────────────────────────────────────────

typedef struct {
    AtomTable    atoms;
    CompoundPool compounds;
    Substitution subst;
    uint32_t     var_counter;    // for fresh variable generation
} PrologCore;

// ── API ─────────────────────────────────────────────────────

// Lifecycle
void     pc_init(PrologCore *pc);
void     pc_reset_compounds(PrologCore *pc);  // clear compound pool (between queries)

// Atom interning (returns atom ID, creates if new)
uint32_t pc_intern_atom(PrologCore *pc, const char *name);
const char* pc_atom_name(PrologCore *pc, uint32_t atom_id);

// Term construction
Term     pc_make_atom(PrologCore *pc, const char *name);
Term     pc_make_var(PrologCore *pc, uint32_t var_id);
Term     pc_make_num(int32_t value);
Term     pc_make_compound(PrologCore *pc, uint32_t functor_atom_id,
                          uint32_t arity, const Term *args);
Term     pc_make_list(PrologCore *pc, uint32_t count, const Term *items, Term tail);

// Compound access
uint32_t pc_compound_functor(PrologCore *pc, Term t);   // atom ID
uint32_t pc_compound_arity(PrologCore *pc, Term t);
Term     pc_compound_arg(PrologCore *pc, Term t, uint32_t i);  // 0-based

// Substitution / trail
void     pc_subst_reset(PrologCore *pc);
uint32_t pc_trail_checkpoint(PrologCore *pc);
void     pc_trail_undo(PrologCore *pc, uint32_t checkpoint);
void     pc_bind(PrologCore *pc, uint32_t var_id, Term value);

// Walk / deepWalk
Term     pc_walk(PrologCore *pc, Term t);
Term     pc_deep_walk(PrologCore *pc, Term t);

// Unification (modifies substitution via trail; returns true on success)
bool     pc_unify(PrologCore *pc, Term a, Term b);

// Fresh variable renaming
// Given a template clause (array of terms: head + body), produce a copy
// with all variables offset by `base`.  Returns number of terms written.
uint32_t pc_fresh_clause(PrologCore *pc,
                         const Term *template_terms, uint32_t template_count,
                         uint32_t var_base,
                         Term *out_terms, uint32_t out_max);

// Term inspection (for JS bridge)
int      pc_term_tag(Term t);
int32_t  pc_term_num_value(Term t);
uint32_t pc_term_atom_id(Term t);
uint32_t pc_term_var_id(Term t);
uint32_t pc_term_compound_id(Term t);

#endif // PROLOG_CORE_H
