/*
 * test_reactive.c — Tests for reactive_core (signals, memos, effects, batch).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reactive_core.h"

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

/* ── Shared state for test closures ──────────────────────── */

static rx_ctx ctx;
static int sig_a, sig_b;
static int memo_a;
static int effect_log;
static int call_count;
static int run_count;

/* ── Computation functions ───────────────────────────────── */

static int double_fn(void *ud) {
    return rx_signal_read((rx_ctx *)ud, sig_a) * 2;
}

static int effect_fn(void *ud) {
    effect_log = rx_signal_read((rx_ctx *)ud, sig_a);
    return 0;
}

static int chain_fn(void *ud) {
    return rx_memo_read((rx_ctx *)ud, memo_a) + 10;
}

static int counted_fn(void *ud) {
    call_count++;
    return rx_signal_read((rx_ctx *)ud, sig_a) * 2;
}

static int counting_effect_fn(void *ud) {
    run_count++;
    effect_log = rx_signal_read((rx_ctx *)ud, sig_a);
    return 0;
}

static int sum_fn(void *ud) {
    return rx_signal_read((rx_ctx *)ud, sig_a) +
           rx_signal_read((rx_ctx *)ud, sig_b);
}

/* ── Tests ───────────────────────────────────────────────── */

static void test_signal_read_write(void) {
    rx_init(&ctx);
    int s = rx_signal_create(&ctx, 42);
    ASSERT(rx_signal_read(&ctx, s) == 42, "signal initial value");

    rx_signal_write(&ctx, s, 99);
    ASSERT(rx_signal_read(&ctx, s) == 99, "signal after write");
}

static void test_memo_basic(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 5);
    int m = rx_memo_create(&ctx, double_fn, &ctx);

    ASSERT(rx_memo_read(&ctx, m) == 10, "memo initial = 5*2");

    rx_signal_write(&ctx, sig_a, 7);
    ASSERT(rx_memo_read(&ctx, m) == 14, "memo after write = 7*2");
}

static void test_memo_lazy(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 3);
    call_count = 0;

    int m = rx_memo_create(&ctx, counted_fn, &ctx);
    ASSERT(call_count == 1, "memo ran once on create");

    /* Reading twice without signal change should not recompute */
    rx_memo_read(&ctx, m);
    rx_memo_read(&ctx, m);
    ASSERT(call_count == 1, "memo not recomputed when clean");

    /* Signal change triggers eager recomputation */
    rx_signal_write(&ctx, sig_a, 10);
    ASSERT(call_count == 2, "memo recomputed on signal change");
    ASSERT(rx_memo_read(&ctx, m) == 20, "memo read returns cached value");
    ASSERT(call_count == 2, "memo not recomputed again (clean)");
}

static void test_effect_basic(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 100);
    effect_log = -1;

    rx_effect_create(&ctx, effect_fn, &ctx);
    ASSERT(effect_log == 100, "effect ran on create");

    rx_signal_write(&ctx, sig_a, 200);
    ASSERT(effect_log == 200, "effect ran on signal change");
}

static void test_batch(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 0);
    effect_log = -1;
    run_count = 0;

    rx_effect_create(&ctx, counting_effect_fn, &ctx);
    ASSERT(run_count == 1, "effect ran once on create");

    rx_batch_begin(&ctx);
    rx_signal_write(&ctx, sig_a, 10);
    rx_signal_write(&ctx, sig_a, 20);
    rx_signal_write(&ctx, sig_a, 30);
    ASSERT(run_count == 1, "effect deferred during batch");
    ASSERT(effect_log == 0, "effect still has old value during batch");

    rx_batch_end(&ctx);
    ASSERT(run_count == 2, "effect ran once after batch (dedup)");
    ASSERT(effect_log == 30, "effect sees final value");
}

static void test_chained_memo(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 5);
    memo_a = rx_memo_create(&ctx, double_fn, &ctx);
    int m2 = rx_memo_create(&ctx, chain_fn, &ctx);

    ASSERT(rx_memo_read(&ctx, m2) == 20, "chained memo: 5*2+10=20");

    rx_signal_write(&ctx, sig_a, 8);
    ASSERT(rx_memo_read(&ctx, m2) == 26, "chained memo: 8*2+10=26");
}

static void test_multiple_signals(void) {
    rx_init(&ctx);
    sig_a = rx_signal_create(&ctx, 1);
    sig_b = rx_signal_create(&ctx, 2);

    int m = rx_memo_create(&ctx, sum_fn, &ctx);
    ASSERT(rx_memo_read(&ctx, m) == 3, "sum memo: 1+2=3");

    rx_signal_write(&ctx, sig_a, 10);
    ASSERT(rx_memo_read(&ctx, m) == 12, "sum memo: 10+2=12");

    rx_signal_write(&ctx, sig_b, 20);
    ASSERT(rx_memo_read(&ctx, m) == 30, "sum memo: 10+20=30");
}

int main(void) {
    test_signal_read_write();
    test_memo_basic();
    test_memo_lazy();
    test_effect_basic();
    test_batch();
    test_chained_memo();
    test_multiple_signals();

    printf("reactive_core: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
