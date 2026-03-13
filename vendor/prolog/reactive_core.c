/*
 * reactive_core.c — Signal/memo/effect runtime.
 *
 * Direct C translation of embedded-prolog's reactive.py.
 * ~100 lines of implementation.  No dependencies beyond C11.
 */

#include "reactive_core.h"
#include <string.h>

/* ── Lifecycle ───────────────────────────────────────────── */

void rx_init(rx_ctx *ctx) {
    memset(ctx, 0, sizeof(rx_ctx));
}

/* ── Internal: add observer as subscriber of a computation ── */

static void track_comp_dep(rx_ctx *ctx, rx_comp *c) {
    if (!ctx->observer) return;
    for (uint32_t i = 0; i < c->sub_count; i++) {
        if (c->subs[i] == ctx->observer) return;
    }
    if (c->sub_count < RX_MAX_SUBS_PER_SIG)
        c->subs[c->sub_count++] = ctx->observer;
}

/* Forward declaration */
static void notify_comp_subs(rx_ctx *ctx, rx_comp *c);

/* ── Internal: run a computation ─────────────────────────── */

static int comp_run(rx_ctx *ctx, rx_comp *c) {
    rx_comp *prev = ctx->observer;
    ctx->observer = c;
    c->value = c->fn(c->user_data);
    c->dirty = false;
    ctx->observer = prev;
    /* Notify downstream subscribers (memo→memo/effect chains) */
    notify_comp_subs(ctx, c);
    return c->value;
}

/* ── Internal: notify computation's downstream subscribers ── */

static void notify_comp_subs(rx_ctx *ctx, rx_comp *c) {
    for (uint32_t i = 0; i < c->sub_count; i++) {
        c->subs[i]->dirty = true;
        if (!c->subs[i]->is_memo)
            comp_run(ctx, c->subs[i]);
    }
}

/* ── Internal: notify subscribers ────────────────────────── */

static void notify_subs(rx_ctx *ctx, rx_signal *sig) {
    if (ctx->batch_depth > 0) {
        /* Defer: add to pending list */
        for (uint32_t i = 0; i < sig->sub_count; i++) {
            if (ctx->pending_count < RX_MAX_PENDING)
                ctx->pending[ctx->pending_count++] = sig->subs[i];
        }
    } else {
        /* Immediate: run each subscriber (memos and effects alike) */
        /* Copy list — a subscriber might modify subs during run */
        rx_comp *snap[RX_MAX_SUBS_PER_SIG];
        uint32_t n = sig->sub_count;
        memcpy(snap, sig->subs, n * sizeof(rx_comp *));
        for (uint32_t i = 0; i < n; i++) {
            snap[i]->dirty = true;
            comp_run(ctx, snap[i]);
        }
    }
}

/* ── Internal: add observer as subscriber ────────────────── */

static void track_dep(rx_ctx *ctx, rx_signal *sig) {
    if (!ctx->observer) return;
    /* Check for duplicate */
    for (uint32_t i = 0; i < sig->sub_count; i++) {
        if (sig->subs[i] == ctx->observer) return;
    }
    if (sig->sub_count < RX_MAX_SUBS_PER_SIG)
        sig->subs[sig->sub_count++] = ctx->observer;
}

/* ── Signals ─────────────────────────────────────────────── */

int rx_signal_create(rx_ctx *ctx, int initial) {
    if (ctx->signal_count >= RX_MAX_SIGNALS) return -1;
    int id = (int)ctx->signal_count++;
    rx_signal *s = &ctx->signals[id];
    s->value = initial;
    s->sub_count = 0;
    s->alive = true;
    return id;
}

int rx_signal_read(rx_ctx *ctx, int sig_id) {
    if (sig_id < 0 || (uint32_t)sig_id >= ctx->signal_count) return 0;
    rx_signal *s = &ctx->signals[sig_id];
    track_dep(ctx, s);
    return s->value;
}

void rx_signal_write(rx_ctx *ctx, int sig_id, int value) {
    if (sig_id < 0 || (uint32_t)sig_id >= ctx->signal_count) return;
    rx_signal *s = &ctx->signals[sig_id];
    s->value = value;
    notify_subs(ctx, s);
}

/* ── Memos ───────────────────────────────────────────────── */

int rx_memo_create(rx_ctx *ctx, rx_fn fn, void *user_data) {
    if (ctx->comp_count >= RX_MAX_COMPUTATIONS) return -1;
    int id = (int)ctx->comp_count++;
    rx_comp *c = &ctx->comps[id];
    c->fn = fn;
    c->user_data = user_data;
    c->is_memo = true;
    c->dirty = true;
    c->alive = true;
    comp_run(ctx, c);    /* initial run establishes deps */
    return id;
}

int rx_memo_read(rx_ctx *ctx, int comp_id) {
    if (comp_id < 0 || (uint32_t)comp_id >= ctx->comp_count) return 0;
    rx_comp *c = &ctx->comps[comp_id];
    track_comp_dep(ctx, c);
    if (c->dirty)
        comp_run(ctx, c);
    return c->value;
}

/* ── Effects ─────────────────────────────────────────────── */

int rx_effect_create(rx_ctx *ctx, rx_fn fn, void *user_data) {
    if (ctx->comp_count >= RX_MAX_COMPUTATIONS) return -1;
    int id = (int)ctx->comp_count++;
    rx_comp *c = &ctx->comps[id];
    c->fn = fn;
    c->user_data = user_data;
    c->is_memo = false;
    c->dirty = true;
    c->alive = true;
    comp_run(ctx, c);    /* initial run */
    return id;
}

/* ── Batch ───────────────────────────────────────────────── */

void rx_batch_begin(rx_ctx *ctx) {
    ctx->batch_depth++;
}

void rx_batch_end(rx_ctx *ctx) {
    ctx->batch_depth--;
    if (ctx->batch_depth > 0) return;
    if (ctx->batch_depth < 0) ctx->batch_depth = 0;  /* safety */

    /* Flush pending with dedup */
    rx_comp *queue[RX_MAX_PENDING];
    uint32_t n = ctx->pending_count;
    memcpy(queue, ctx->pending, n * sizeof(rx_comp *));
    ctx->pending_count = 0;

    for (uint32_t i = 0; i < n; i++) {
        /* Dedup: skip if already seen */
        bool dup = false;
        for (uint32_t j = 0; j < i; j++) {
            if (queue[j] == queue[i]) { dup = true; break; }
        }
        if (dup) continue;

        queue[i]->dirty = true;
        if (!queue[i]->is_memo)
            comp_run(ctx, queue[i]);
    }
}
