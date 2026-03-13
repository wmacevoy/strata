/*
 * test_reactive_prolog.c — Tests for the reactive Prolog engine.
 *
 * Mirrors the embedded-prolog vending machine test pattern:
 * facts as sensors, reactive queries as live values.
 */
#include <stdio.h>
#include <string.h>
#include "reactive_prolog.h"

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

/* ── Helpers ─────────────────────────────────────────────── */

static rp_engine RP;

static Term at(const char *name) { return pc_make_atom(rp_core(&RP), name); }
static Term n(int32_t val) { return pc_make_num(val); }
static Term v(uint32_t id) { return pc_make_var(rp_core(&RP), id); }

static Term c1(const char *f, Term a) {
    PrologCore *pc = rp_core(&RP);
    return pc_make_compound(pc, pc_intern_atom(pc, f), 1, &a);
}

static Term c2(const char *f, Term a, Term b) {
    PrologCore *pc = rp_core(&RP);
    Term args[2] = {a, b};
    return pc_make_compound(pc, pc_intern_atom(pc, f), 2, args);
}

static Term c3(const char *f, Term a, Term b, Term c) {
    PrologCore *pc = rp_core(&RP);
    Term args[3] = {a, b, c};
    return pc_make_compound(pc, pc_intern_atom(pc, f), 3, args);
}

/* ── Goal builder functions ──────────────────────────────── */

static Term credit_goal(ps_solver *s, void *ud) {
    (void)ud;
    return pc_make_compound(&s->core, pc_intern_atom(&s->core, "credit"), 1,
                           &(Term){ pc_make_var(&s->core, 0) });
}

static Term can_vend_goal(ps_solver *s, void *ud) {
    (void)ud;
    return pc_make_compound(&s->core, pc_intern_atom(&s->core, "can_vend"), 1,
                           &(Term){ pc_make_var(&s->core, 0) });
}

static Term sensor_goal(ps_solver *s, void *ud) {
    const char *name = (const char *)ud;
    PrologCore *pc = &s->core;
    Term args[2] = { pc_make_atom(pc, name), pc_make_var(pc, 0) };
    return pc_make_compound(pc, pc_intern_atom(pc, "sensor"), 2, args);
}

static Term has_fault_goal(ps_solver *s, void *ud) {
    (void)ud;
    return pc_make_atom(&s->core, "has_any_fault");
}

/* ── Tests ───────────────────────────────────────────────── */

static void test_basic_reactive(void) {
    rp_init(&RP);
    ps_solver *s = rp_solver(&RP);

    /* Add initial facts */
    ps_add_clause(s, c1("credit", n(0)), NULL, 0);
    ps_mark_clauses(s);

    /* Create reactive query for credit */
    int q = rp_query_create(&RP, credit_goal, NULL);
    ASSERT(q >= 0, "query created");

    /* Read initial value */
    Term result = rp_query_read(&RP, q);
    ASSERT(result != TERM_NONE, "credit query has result");
    Term arg = pc_compound_arg(rp_core(&RP), result, 0);
    ASSERT(TERM_TAG(arg) == TAG_NUM && NUM_VALUE(arg) == 0, "credit is 0");

    /* Update credit: retract old, assert new, bump */
    rp_retract(&RP, c1("credit", v(99)));
    rp_assert(&RP, c1("credit", n(125)));

    /* Reactive query should auto-update */
    result = rp_query_read(&RP, q);
    ASSERT(result != TERM_NONE, "credit query still has result");
    arg = pc_compound_arg(rp_core(&RP), result, 0);
    ASSERT(TERM_TAG(arg) == TAG_NUM && NUM_VALUE(arg) == 125, "credit is now 125");
}

static void test_sensor_update(void) {
    rp_init(&RP);
    ps_solver *s = rp_solver(&RP);

    /* Initial sensor state */
    ps_add_clause(s, c2("sensor", at("tilt"), at("ok")), NULL, 0);
    ps_mark_clauses(s);

    /* Reactive query for tilt sensor */
    int q = rp_query_create(&RP, sensor_goal, "tilt");
    Term result = rp_query_read(&RP, q);
    ASSERT(result != TERM_NONE, "sensor query has result");
    Term val = pc_compound_arg(rp_core(&RP), result, 1);
    ASSERT(strcmp(pc_atom_name(rp_core(&RP), ATOM_ID(val)), "ok") == 0,
           "tilt sensor is ok");

    /* Simulate sensor change */
    rp_update_sensor(&RP, "tilt", "tilted");

    /* Query auto-updates */
    result = rp_query_read(&RP, q);
    val = pc_compound_arg(rp_core(&RP), result, 1);
    ASSERT(strcmp(pc_atom_name(rp_core(&RP), ATOM_ID(val)), "tilted") == 0,
           "tilt sensor is now tilted");
}

static void test_vending_machine(void) {
    rp_init(&RP);
    ps_solver *s = rp_solver(&RP);

    /* Product catalog */
    ps_add_clause(s, c3("product", at("a1"), at("cola"), n(125)), NULL, 0);
    ps_add_clause(s, c3("product", at("b1"), at("water"), n(100)), NULL, 0);

    /* Initial state */
    ps_add_clause(s, c1("machine_state", at("idle")), NULL, 0);
    ps_add_clause(s, c1("credit", n(0)), NULL, 0);
    ps_add_clause(s, c2("sensor", at("delivery"), at("clear")), NULL, 0);
    ps_add_clause(s, c2("sensor", at("tilt"), at("ok")), NULL, 0);

    /* fault_condition(tilt_detected) :- sensor(tilt, tilted). */
    Term tilt_body[1] = { c2("sensor", at("tilt"), at("tilted")) };
    ps_add_clause(s, c1("fault_condition", at("tilt_detected")), tilt_body, 1);

    /* has_any_fault :- fault_condition(_). */
    Term fault_body[1] = { c1("fault_condition", v(0)) };
    ps_add_clause(s, at("has_any_fault"), fault_body, 1);

    /* can_vend(Slot) :- machine_state(idle), \+(has_any_fault),
     *    product(Slot, _, Price), credit(C), C >= Price,
     *    sensor(delivery, clear). */
    Term vend_body[6] = {
        c1("machine_state", at("idle")),
        c1("\\+", at("has_any_fault")),
        c3("product", v(0), v(1), v(2)),
        c1("credit", v(3)),
        c2(">=", v(3), v(2)),
        c2("sensor", at("delivery"), at("clear"))
    };
    ps_add_clause(s, c1("can_vend", v(0)), vend_body, 6);
    ps_mark_clauses(s);

    /* Reactive queries */
    int q_credit = rp_query_create(&RP, credit_goal, NULL);
    int q_vend = rp_query_create(&RP, can_vend_goal, NULL);
    int q_fault = rp_query_create(&RP, has_fault_goal, NULL);

    /* Initially: no credit, can't vend, no fault */
    Term r = rp_query_read(&RP, q_credit);
    ASSERT(r != TERM_NONE, "credit query works");
    Term credit_val = pc_compound_arg(rp_core(&RP), r, 0);
    ASSERT(NUM_VALUE(credit_val) == 0, "initial credit is 0");

    r = rp_query_read(&RP, q_vend);
    ASSERT(r == TERM_NONE, "can't vend with 0 credit");

    r = rp_query_read(&RP, q_fault);
    ASSERT(r == TERM_NONE, "no fault initially");

    /* Insert credit */
    rp_retract(&RP, c1("credit", v(99)));
    rp_assert(&RP, c1("credit", n(200)));

    r = rp_query_read(&RP, q_credit);
    credit_val = pc_compound_arg(rp_core(&RP), r, 0);
    ASSERT(NUM_VALUE(credit_val) == 200, "credit updated to 200");

    r = rp_query_read(&RP, q_vend);
    ASSERT(r != TERM_NONE, "can vend with 200 credit");
    Term slot = pc_compound_arg(rp_core(&RP), r, 0);
    ASSERT(strcmp(pc_atom_name(rp_core(&RP), ATOM_ID(slot)), "a1") == 0,
           "can vend a1");

    /* Tilt sensor triggers fault → can't vend */
    rp_update_sensor(&RP, "tilt", "tilted");

    r = rp_query_read(&RP, q_fault);
    ASSERT(r != TERM_NONE, "fault detected after tilt");

    r = rp_query_read(&RP, q_vend);
    ASSERT(r == TERM_NONE, "can't vend during fault");

    /* Recover tilt → can vend again */
    rp_update_sensor(&RP, "tilt", "ok");

    r = rp_query_read(&RP, q_fault);
    ASSERT(r == TERM_NONE, "no fault after recovery");

    r = rp_query_read(&RP, q_vend);
    ASSERT(r != TERM_NONE, "can vend after recovery");
}

static void test_batch_updates(void) {
    rp_init(&RP);
    ps_solver *s = rp_solver(&RP);

    ps_add_clause(s, c1("credit", n(0)), NULL, 0);
    ps_add_clause(s, c2("sensor", at("tilt"), at("ok")), NULL, 0);
    ps_mark_clauses(s);

    int q = rp_query_create(&RP, credit_goal, NULL);

    /* Batch: multiple changes, one recomputation */
    rp_batch_begin(&RP);
    rp_retract(&RP, c1("credit", v(99)));
    rp_assert(&RP, c1("credit", n(50)));
    rp_retract(&RP, c1("credit", v(99)));
    rp_assert(&RP, c1("credit", n(300)));
    rp_batch_end(&RP);

    Term r = rp_query_read(&RP, q);
    Term val = pc_compound_arg(rp_core(&RP), r, 0);
    ASSERT(NUM_VALUE(val) == 300, "batch: final credit is 300");
}

int main(void) {
    test_basic_reactive();
    test_sensor_update();
    test_vending_machine();
    test_batch_updates();

    printf("reactive_prolog: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
