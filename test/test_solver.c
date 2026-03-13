/*
 * test_solver.c — Tests for prolog_solver (clause db + backtracking).
 */
#include <stdio.h>
#include <string.h>
#include "prolog_solver.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

static ps_solver S;

/* Helper: make compound(functor, arg1, arg2) */
static Term c2(const char *f, Term a, Term b) {
    Term args[2] = {a, b};
    return pc_make_compound(&S.core, pc_intern_atom(&S.core, f), 2, args);
}

static Term c1(const char *f, Term a) {
    return pc_make_compound(&S.core, pc_intern_atom(&S.core, f), 1, &a);
}

static Term c3(const char *f, Term a, Term b, Term c) {
    Term args[3] = {a, b, c};
    return pc_make_compound(&S.core, pc_intern_atom(&S.core, f), 3, args);
}

static Term at(const char *name) { return pc_make_atom(&S.core, name); }
static Term v(uint32_t id) { return pc_make_var(&S.core, id); }
static Term n(int32_t val) { return pc_make_num(val); }

/* ── Tests ───────────────────────────────────────────────── */

static void test_simple_fact(void) {
    ps_init(&S);
    /* parent(tom, bob). */
    ps_add_clause(&S, c2("parent", at("tom"), at("bob")), NULL, 0);
    ps_add_clause(&S, c2("parent", at("tom"), at("liz")), NULL, 0);
    ps_mark_clauses(&S);

    /* ?- parent(tom, bob). */
    Term goal = c2("parent", at("tom"), at("bob"));
    int count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 1, "parent(tom,bob) has 1 solution");

    /* ?- parent(tom, X). */
    goal = c2("parent", at("tom"), v(0));
    count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 2, "parent(tom,X) has 2 solutions");
}

static void test_query_first(void) {
    ps_init(&S);
    ps_add_clause(&S, c2("parent", at("tom"), at("bob")), NULL, 0);
    ps_add_clause(&S, c2("parent", at("tom"), at("liz")), NULL, 0);
    ps_mark_clauses(&S);

    Term goal = c2("parent", at("tom"), v(0));
    Term result = ps_query_first(&S, goal);
    ASSERT(result != TERM_NONE, "query_first finds a result");

    /* Result should be parent(tom, bob) — first matching clause */
    ASSERT(TERM_TAG(result) == TAG_COMPOUND, "result is compound");
    Term arg1 = pc_compound_arg(&S.core, result, 1);
    ASSERT(TERM_TAG(arg1) == TAG_ATOM, "second arg is atom");
    ASSERT(strcmp(pc_atom_name(&S.core, ATOM_ID(arg1)), "bob") == 0,
           "first result is bob");
}

static void test_rules(void) {
    ps_init(&S);
    /* parent(tom, bob). parent(bob, ann). */
    ps_add_clause(&S, c2("parent", at("tom"), at("bob")), NULL, 0);
    ps_add_clause(&S, c2("parent", at("bob"), at("ann")), NULL, 0);

    /* grandparent(X, Z) :- parent(X, Y), parent(Y, Z). */
    Term body[2] = {
        c2("parent", v(0), v(1)),
        c2("parent", v(1), v(2))
    };
    ps_add_clause(&S, c2("grandparent", v(0), v(2)), body, 2);
    ps_mark_clauses(&S);

    /* ?- grandparent(tom, ann). */
    Term goal = c2("grandparent", at("tom"), at("ann"));
    int count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 1, "grandparent(tom,ann) succeeds");

    /* ?- grandparent(tom, X). */
    goal = c2("grandparent", at("tom"), v(0));
    Term result = ps_query_first(&S, goal);
    ASSERT(result != TERM_NONE, "grandparent(tom,X) finds result");
    Term arg = pc_compound_arg(&S.core, result, 1);
    ASSERT(strcmp(pc_atom_name(&S.core, ATOM_ID(arg)), "ann") == 0,
           "grandparent result is ann");
}

static void test_negation(void) {
    ps_init(&S);
    ps_add_clause(&S, at("sunny"), NULL, 0);
    ps_mark_clauses(&S);

    /* ?- \+(rainy). should succeed (rainy is not provable) */
    Term goal = c1("\\+", at("rainy"));
    int count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 1, "\\+(rainy) succeeds");

    /* ?- \+(sunny). should fail */
    goal = c1("\\+", at("sunny"));
    count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 0, "\\+(sunny) fails");
}

static void test_arithmetic(void) {
    ps_init(&S);
    ps_mark_clauses(&S);

    /* ?- X is 3 + 4. */
    Term goal = c2("is", v(0), c2("+", n(3), n(4)));
    Term result = ps_query_first(&S, goal);
    ASSERT(result != TERM_NONE, "3+4 computes");
    Term arg = pc_compound_arg(&S.core, result, 0);
    ASSERT(TERM_TAG(arg) == TAG_NUM && NUM_VALUE(arg) == 7, "3+4=7");
}

static void test_comparison(void) {
    ps_init(&S);
    ps_mark_clauses(&S);

    /* ?- 5 > 3. */
    int count = ps_query(&S, c2(">", n(5), n(3)), NULL, NULL, 10);
    ASSERT(count == 1, "5 > 3 succeeds");

    /* ?- 3 > 5. */
    count = ps_query(&S, c2(">", n(3), n(5)), NULL, NULL, 10);
    ASSERT(count == 0, "3 > 5 fails");

    /* ?- 5 >= 5. */
    count = ps_query(&S, c2(">=", n(5), n(5)), NULL, NULL, 10);
    ASSERT(count == 1, "5 >= 5 succeeds");
}

static void test_unification(void) {
    ps_init(&S);
    ps_mark_clauses(&S);

    /* ?- X = hello. */
    Term goal = c2("=", v(0), at("hello"));
    Term result = ps_query_first(&S, goal);
    ASSERT(result != TERM_NONE, "X=hello succeeds");

    /* ?- hello \= world. */
    int count = ps_query(&S, c2("\\=", at("hello"), at("world")), NULL, NULL, 10);
    ASSERT(count == 1, "hello \\= world succeeds");

    /* ?- hello \= hello. */
    count = ps_query(&S, c2("\\=", at("hello"), at("hello")), NULL, NULL, 10);
    ASSERT(count == 0, "hello \\= hello fails");
}

static void test_assert_retract(void) {
    ps_init(&S);
    ps_mark_clauses(&S);

    /* Assert a fact, then query */
    ps_add_clause(&S, c1("color", at("red")), NULL, 0);
    int count = ps_query(&S, c1("color", v(0)), NULL, NULL, 10);
    ASSERT(count == 1, "color(X) has 1 result after assert");

    /* Retract it */
    bool ok = ps_retract_first(&S, c1("color", at("red")));
    ASSERT(ok, "retract color(red) succeeds");

    count = ps_query(&S, c1("color", v(0)), NULL, NULL, 10);
    ASSERT(count == 0, "color(X) has 0 results after retract");
}

static void test_member(void) {
    ps_init(&S);
    ps_mark_clauses(&S);

    /* ?- member(X, [a, b, c]). */
    Term items[3] = { at("a"), at("b"), at("c") };
    Term list = pc_make_list(&S.core, 3, items, TERM_NONE);
    Term goal = c2("member", v(0), list);
    int count = ps_query(&S, goal, NULL, NULL, 10);
    ASSERT(count == 3, "member(X,[a,b,c]) has 3 solutions");
}

static void test_backtracking(void) {
    ps_init(&S);
    /* likes(mary, food). likes(mary, wine). likes(john, wine). */
    ps_add_clause(&S, c2("likes", at("mary"), at("food")), NULL, 0);
    ps_add_clause(&S, c2("likes", at("mary"), at("wine")), NULL, 0);
    ps_add_clause(&S, c2("likes", at("john"), at("wine")), NULL, 0);
    ps_mark_clauses(&S);

    /* ?- likes(mary, X). should find 2 solutions */
    Term results[10];
    int count = ps_query_all(&S, c2("likes", at("mary"), v(0)), results, 10);
    ASSERT(count == 2, "likes(mary,X) has 2 solutions");
}

static void test_vending_style(void) {
    ps_init(&S);

    /* Simplified vending machine rules */
    /* machine_state(idle). */
    ps_add_clause(&S, c1("machine_state", at("idle")), NULL, 0);
    /* credit(0). */
    ps_add_clause(&S, c1("credit", n(0)), NULL, 0);
    /* sensor(delivery, clear). */
    ps_add_clause(&S, c2("sensor", at("delivery"), at("clear")), NULL, 0);
    /* product(a1, cola, 125). */
    ps_add_clause(&S, c3("product", at("a1"), at("cola"), n(125)), NULL, 0);

    /* can_vend(Slot) :- machine_state(idle), product(Slot, _, Price),
     *                   credit(C), C >= Price. */
    Term body_vend[4] = {
        c1("machine_state", at("idle")),
        c3("product", v(0), v(1), v(2)),
        c1("credit", v(3)),
        c2(">=", v(3), v(2))
    };
    ps_add_clause(&S, c1("can_vend", v(0)), body_vend, 4);
    ps_mark_clauses(&S);

    /* ?- can_vend(a1). — should fail (credit=0 < 125) */
    int count = ps_query(&S, c1("can_vend", at("a1")), NULL, NULL, 10);
    ASSERT(count == 0, "can_vend(a1) fails with 0 credit");

    /* Add credit: retract credit(0), assert credit(200). */
    ps_retract_first(&S, c1("credit", n(0)));
    ps_add_clause(&S, c1("credit", n(200)), NULL, 0);

    /* ?- can_vend(a1). — should succeed now */
    count = ps_query(&S, c1("can_vend", at("a1")), NULL, NULL, 10);
    ASSERT(count == 1, "can_vend(a1) succeeds with 200 credit");
}

int main(void) {
    test_simple_fact();
    test_query_first();
    test_rules();
    test_negation();
    test_arithmetic();
    test_comparison();
    test_unification();
    test_assert_retract();
    test_member();
    test_backtracking();
    test_vending_style();

    printf("prolog_solver: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
