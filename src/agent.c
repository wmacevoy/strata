#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <zmq.h>
#include <wasm_export.h>

#include "strata/agent.h"

/* ------------------------------------------------------------------ */
/*  Bedrock context — shared by WASM natives and QuickJS bindings   */
/* ------------------------------------------------------------------ */

typedef struct {
    void *zmq_ctx;
    void *sub_sock;
    void *req_sock;
    void *pub_sock;
    void *rep_sock;
} bedrock_ctx_t;

/* ------------------------------------------------------------------ */
/*  ZMQ socket setup helpers                                           */
/* ------------------------------------------------------------------ */

static void bedrock_setup(bedrock_ctx_t *bedrock, strata_agent_def *def) {
    memset(bedrock, 0, sizeof(*bedrock));
    bedrock->zmq_ctx = zmq_ctx_new();
    int timeout = 5000;

    if (def->sub_endpoint[0]) {
        bedrock->sub_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_SUB);
        zmq_connect(bedrock->sub_sock, def->sub_endpoint);
        zmq_setsockopt(bedrock->sub_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
    if (def->req_endpoint[0]) {
        bedrock->req_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_REQ);
        zmq_connect(bedrock->req_sock, def->req_endpoint);
        zmq_setsockopt(bedrock->req_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
    if (def->pub_endpoint[0]) {
        bedrock->pub_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_PUB);
        zmq_bind(bedrock->pub_sock, def->pub_endpoint);
    }
    if (def->rep_endpoint[0]) {
        bedrock->rep_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_REP);
        zmq_bind(bedrock->rep_sock, def->rep_endpoint);
        zmq_setsockopt(bedrock->rep_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
}

static void bedrock_teardown(bedrock_ctx_t *bedrock) {
    if (bedrock->sub_sock) zmq_close(bedrock->sub_sock);
    if (bedrock->req_sock) zmq_close(bedrock->req_sock);
    if (bedrock->pub_sock) zmq_close(bedrock->pub_sock);
    if (bedrock->rep_sock) zmq_close(bedrock->rep_sock);
    if (bedrock->zmq_ctx)  zmq_ctx_destroy(bedrock->zmq_ctx);
}

/* ------------------------------------------------------------------ */
/*  WASM native functions                                              */
/* ------------------------------------------------------------------ */

static void wasm_bedrock_log(wasm_exec_env_t exec_env,
                          char *msg, int32_t msg_len) {
    (void)exec_env;
    fprintf(stderr, "[wasm] %.*s\n", msg_len, msg);
}

static int32_t wasm_bedrock_subscribe(wasm_exec_env_t exec_env,
                                   char *filter, int32_t filter_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->sub_sock) return -1;
    return zmq_setsockopt(bedrock->sub_sock, ZMQ_SUBSCRIBE, filter, filter_len);
}

static int32_t wasm_bedrock_receive(wasm_exec_env_t exec_env,
                                 char *topic_buf, int32_t topic_cap,
                                 char *payload_buf, int32_t payload_cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->sub_sock) return -1;
    int rc = zmq_recv(bedrock->sub_sock, topic_buf, topic_cap, 0);
    if (rc < 0) return -1;
    return zmq_recv(bedrock->sub_sock, payload_buf, payload_cap, 0);
}

static int32_t wasm_bedrock_request(wasm_exec_env_t exec_env,
                                 char *req, int32_t req_len,
                                 char *resp_buf, int32_t resp_cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->req_sock) return -1;
    int rc = zmq_send(bedrock->req_sock, req, req_len, 0);
    if (rc < 0) return -1;
    return zmq_recv(bedrock->req_sock, resp_buf, resp_cap, 0);
}

static int32_t wasm_bedrock_publish(wasm_exec_env_t exec_env,
                                 char *topic, int32_t topic_len,
                                 char *payload, int32_t payload_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->pub_sock) return -1;
    zmq_send(bedrock->pub_sock, topic, topic_len, ZMQ_SNDMORE);
    return zmq_send(bedrock->pub_sock, payload, payload_len, 0);
}

static int32_t wasm_bedrock_serve_recv(wasm_exec_env_t exec_env,
                                    char *buf, int32_t cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->rep_sock) return -1;
    return zmq_recv(bedrock->rep_sock, buf, cap, 0);
}

static int32_t wasm_bedrock_serve_send(wasm_exec_env_t exec_env,
                                    char *resp, int32_t resp_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->rep_sock) return -1;
    return zmq_send(bedrock->rep_sock, resp, resp_len, 0);
}

/* ------------------------------------------------------------------ */
/*  QuickJS strata runner                                              */
/* ------------------------------------------------------------------ */

#include "quickjs.h"

/* JS binding: bedrock.log(msg) */
static JSValue js_bedrock_log(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (msg) {
        fprintf(stderr, "[js] %s\n", msg);
        JS_FreeCString(ctx, msg);
    }
    return JS_UNDEFINED;
}

/* JS binding: bedrock.publish(topic, payload) -> bytes sent or -1 */
static JSValue js_bedrock_publish(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->pub_sock || argc < 2) return JS_NewInt32(ctx, -1);

    const char *topic = JS_ToCString(ctx, argv[0]);
    const char *payload = JS_ToCString(ctx, argv[1]);
    int rc = -1;
    if (topic && payload) {
        zmq_send(bedrock->pub_sock, topic, strlen(topic), ZMQ_SNDMORE);
        rc = zmq_send(bedrock->pub_sock, payload, strlen(payload), 0);
    }
    if (topic) JS_FreeCString(ctx, topic);
    if (payload) JS_FreeCString(ctx, payload);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.request(msg) -> response string or null */
static JSValue js_bedrock_request(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->req_sock || argc < 1) return JS_NULL;

    const char *req = JS_ToCString(ctx, argv[0]);
    if (!req) return JS_NULL;

    int rc = zmq_send(bedrock->req_sock, req, strlen(req), 0);
    JS_FreeCString(ctx, req);
    if (rc < 0) return JS_NULL;

    char resp[8192];
    rc = zmq_recv(bedrock->req_sock, resp, sizeof(resp) - 1, 0);
    if (rc < 0) return JS_NULL;
    resp[rc] = '\0';
    return JS_NewString(ctx, resp);
}

/* JS binding: bedrock.serve_recv() -> request string or null */
static JSValue js_bedrock_serve_recv(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_sock) return JS_NULL;

    char buf[8192];
    int rc = zmq_recv(bedrock->rep_sock, buf, sizeof(buf) - 1, 0);
    if (rc < 0) return JS_NULL;
    buf[rc] = '\0';
    return JS_NewString(ctx, buf);
}

/* JS binding: bedrock.serve_send(response) -> bytes sent or -1 */
static JSValue js_bedrock_serve_send(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_sock || argc < 1) return JS_NewInt32(ctx, -1);

    const char *resp = JS_ToCString(ctx, argv[0]);
    if (!resp) return JS_NewInt32(ctx, -1);

    int rc = zmq_send(bedrock->rep_sock, resp, strlen(resp), 0);
    JS_FreeCString(ctx, resp);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.subscribe(filter) */
static JSValue js_bedrock_subscribe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->sub_sock || argc < 1) return JS_NewInt32(ctx, -1);

    const char *filter = JS_ToCString(ctx, argv[0]);
    if (!filter) return JS_NewInt32(ctx, -1);

    int rc = zmq_setsockopt(bedrock->sub_sock, ZMQ_SUBSCRIBE, filter, strlen(filter));
    JS_FreeCString(ctx, filter);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.receive() -> {topic, payload} or null */
static JSValue js_bedrock_receive(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->sub_sock) return JS_NULL;

    char topic[512] = {0};
    char payload[8192] = {0};
    int rc = zmq_recv(bedrock->sub_sock, topic, sizeof(topic) - 1, 0);
    if (rc < 0) return JS_NULL;
    topic[rc < (int)sizeof(topic) ? rc : (int)sizeof(topic) - 1] = '\0';

    rc = zmq_recv(bedrock->sub_sock, payload, sizeof(payload) - 1, 0);
    if (rc < 0) return JS_NULL;
    payload[rc < (int)sizeof(payload) ? rc : (int)sizeof(payload) - 1] = '\0';

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "topic", JS_NewString(ctx, topic));
    JS_SetPropertyStr(ctx, obj, "payload", JS_NewString(ctx, payload));
    return obj;
}

static void js_child_run(strata_agent_def *def,
                         const char *event_json, int event_len) {
    bedrock_ctx_t bedrock;
    bedrock_setup(&bedrock, def);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetContextOpaque(ctx, &bedrock);

    /* Create bedrock global object */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bedrock_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, bedrock_obj, "log",
        JS_NewCFunction(ctx, js_bedrock_log, "log", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "publish",
        JS_NewCFunction(ctx, js_bedrock_publish, "publish", 2));
    JS_SetPropertyStr(ctx, bedrock_obj, "request",
        JS_NewCFunction(ctx, js_bedrock_request, "request", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "serve_recv",
        JS_NewCFunction(ctx, js_bedrock_serve_recv, "serve_recv", 0));
    JS_SetPropertyStr(ctx, bedrock_obj, "serve_send",
        JS_NewCFunction(ctx, js_bedrock_serve_send, "serve_send", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "subscribe",
        JS_NewCFunction(ctx, js_bedrock_subscribe, "subscribe", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "receive",
        JS_NewCFunction(ctx, js_bedrock_receive, "receive", 0));

    JS_SetPropertyStr(ctx, global, "bedrock", bedrock_obj);

    /* Set __event__ global with the trigger event */
    if (event_json && event_len > 0) {
        JSValue ev = JS_NewStringLen(ctx, event_json, event_len);
        JS_SetPropertyStr(ctx, global, "__event__", ev);
    }

    JS_FreeValue(ctx, global);

    /* Evaluate the JS source */
    JSValue result = JS_Eval(ctx, def->js_source, strlen(def->js_source),
                             def->js_path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        fprintf(stderr, "[js] exception: %s\n", str ? str : "unknown");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    bedrock_teardown(&bedrock);
}

/* ------------------------------------------------------------------ */
/*  Agent host                                                         */
/* ------------------------------------------------------------------ */

struct strata_agent_host {
    strata_agent_def agents[STRATA_MAX_AGENTS];
    int agent_count;
};

strata_agent_host *strata_agent_host_create(void) {
    return calloc(1, sizeof(strata_agent_host));
}

void strata_agent_host_free(strata_agent_host *host) {
    if (!host) return;
    for (int i = 0; i < host->agent_count; i++) {
        free(host->agents[i].wasm_buf);
        free(host->agents[i].js_source);
    }
    free(host);
}

static char *load_text_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len) {
        free(buf); fclose(f); return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static unsigned char *load_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(len);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

int strata_agent_register(strata_agent_host *host,
                          const char *name, const char *wasm_path,
                          const char *trigger_filter,
                          const char *sub_endpoint, const char *req_endpoint) {
    if (!host || host->agent_count >= STRATA_MAX_AGENTS) return -1;

    strata_agent_def *def = &host->agents[host->agent_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_WASM;
    strncpy(def->name, name, sizeof(def->name) - 1);
    strncpy(def->wasm_path, wasm_path, sizeof(def->wasm_path) - 1);
    if (trigger_filter) strncpy(def->trigger_filter, trigger_filter, sizeof(def->trigger_filter) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->wasm_buf = load_file(wasm_path, &def->wasm_len);
    if (!def->wasm_buf) {
        fprintf(stderr, "failed to load %s\n", wasm_path);
        return -1;
    }
    host->agent_count++;
    return 0;
}

int strata_js_register(strata_agent_host *host,
                       const char *name, const char *js_path,
                       const char *sub_endpoint, const char *req_endpoint,
                       const char *pub_endpoint, const char *rep_endpoint) {
    if (!host || host->agent_count >= STRATA_MAX_AGENTS) return -1;

    strata_agent_def *def = &host->agents[host->agent_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_JS;
    strncpy(def->name, name, sizeof(def->name) - 1);
    strncpy(def->js_path, js_path, sizeof(def->js_path) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);
    if (pub_endpoint) strncpy(def->pub_endpoint, pub_endpoint, sizeof(def->pub_endpoint) - 1);
    if (rep_endpoint) strncpy(def->rep_endpoint, rep_endpoint, sizeof(def->rep_endpoint) - 1);

    def->js_source = load_text_file(js_path);
    if (!def->js_source) {
        fprintf(stderr, "failed to load %s\n", js_path);
        return -1;
    }
    host->agent_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  WASM child runner                  */
/* ------------------------------------------------------------------ */

static void wasm_child_run(strata_agent_def *def,
                           const char *event_json, int event_len) {
    char err[128];
    bedrock_ctx_t bedrock;
    bedrock_setup(&bedrock, def);

    if (!wasm_runtime_init()) {
        fprintf(stderr, "wasm_runtime_init failed\n");
        goto cleanup;
    }

    static NativeSymbol natives[] = {
        { "log",        (void *)wasm_bedrock_log,        "(*~)",     NULL },
        { "subscribe",  (void *)wasm_bedrock_subscribe,   "(*~)i",    NULL },
        { "receive",    (void *)wasm_bedrock_receive,     "(*~*~)i",  NULL },
        { "request",    (void *)wasm_bedrock_request,     "(*~*~)i",  NULL },
        { "publish",    (void *)wasm_bedrock_publish,     "(*~*~)i",  NULL },
        { "serve_recv", (void *)wasm_bedrock_serve_recv,  "(*~)i",    NULL },
        { "serve_send", (void *)wasm_bedrock_serve_send,  "(*~)i",    NULL },
    };
    for (int i = 0; i < 7; i++) natives[i].attachment = &bedrock;

    if (!wasm_runtime_register_natives("bedrock", natives, 7)) {
        fprintf(stderr, "register_natives failed\n");
        goto cleanup_wamr;
    }

    unsigned char *wasm_copy = malloc(def->wasm_len);
    if (!wasm_copy) goto cleanup_wamr;
    memcpy(wasm_copy, def->wasm_buf, def->wasm_len);

    wasm_module_t module = wasm_runtime_load(wasm_copy, (uint32_t)def->wasm_len,
                                             err, sizeof(err));
    if (!module) {
        fprintf(stderr, "wasm_runtime_load: %s\n", err);
        free(wasm_copy);
        goto cleanup_wamr;
    }

    wasm_module_inst_t inst = wasm_runtime_instantiate(module, 16384, 16384,
                                                       err, sizeof(err));
    if (!inst) {
        fprintf(stderr, "wasm_runtime_instantiate: %s\n", err);
        goto cleanup_module;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 16384);
    if (!exec_env) goto cleanup_inst;

    /* Try serve() first (long-lived strata), fall back to on_event */
    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "serve");
    if (func) {
        wasm_runtime_call_wasm(exec_env, func, 0, NULL);
    } else {
        func = wasm_runtime_lookup_function(inst, "on_event");
        if (!func) {
            fprintf(stderr, "no serve() or on_event() in WASM module\n");
            goto cleanup_env;
        }
        void *native_ptr = NULL;
        uint32_t wasm_ptr = wasm_runtime_module_malloc(inst, event_len, &native_ptr);
        if (!wasm_ptr) goto cleanup_env;
        memcpy(native_ptr, event_json, event_len);

        uint32_t argv[2] = { wasm_ptr, (uint32_t)event_len };
        if (!wasm_runtime_call_wasm(exec_env, func, 2, argv)) {
            const char *exc = wasm_runtime_get_exception(inst);
            fprintf(stderr, "on_event failed: %s\n", exc ? exc : "unknown");
        }
        wasm_runtime_module_free(inst, wasm_ptr);
    }

cleanup_env:
    wasm_runtime_destroy_exec_env(exec_env);
cleanup_inst:
    wasm_runtime_deinstantiate(inst);
cleanup_module:
    wasm_runtime_unload(module);
    free(wasm_copy);
cleanup_wamr:
    wasm_runtime_destroy();
cleanup:
    bedrock_teardown(&bedrock);
}

/* ------------------------------------------------------------------ */
/*  Spawn: fork + dispatch by mode                                     */
/* ------------------------------------------------------------------ */

static strata_agent_def *find_agent(strata_agent_host *host, const char *name) {
    for (int i = 0; i < host->agent_count; i++) {
        if (strcmp(host->agents[i].name, name) == 0)
            return &host->agents[i];
    }
    return NULL;
}

pid_t strata_agent_spawn(strata_agent_host *host,
                         const char *agent_name,
                         const char *event_json, int event_len) {
    if (!host || !agent_name) return -1;

    strata_agent_def *def = find_agent(host, agent_name);
    if (!def) {
        fprintf(stderr, "agent '%s' not registered\n", agent_name);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (def->mode == STRATA_MODE_JS)
            js_child_run(def, event_json, event_len);
        else
            wasm_child_run(def, event_json, event_len);
        _exit(0);
    }

    return pid;
}

int strata_agent_host_reap(strata_agent_host *host) {
    (void)host;
    int count = 0;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        count++;
    return count;
}

const strata_agent_def *strata_agent_host_find(const strata_agent_host *host,
                                                const char *name) {
    if (!host || !name) return NULL;
    for (int i = 0; i < host->agent_count; i++) {
        if (strcmp(host->agents[i].name, name) == 0)
            return &host->agents[i];
    }
    return NULL;
}

int strata_agent_register_wasm_buf(strata_agent_host *host,
                                    const char *name,
                                    const unsigned char *wasm_buf, size_t wasm_len,
                                    const char *sub_endpoint,
                                    const char *req_endpoint) {
    if (!host || !name || !wasm_buf || host->agent_count >= STRATA_MAX_AGENTS) return -1;

    strata_agent_def *def = &host->agents[host->agent_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_WASM;
    strncpy(def->name, name, sizeof(def->name) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->wasm_buf = malloc(wasm_len);
    if (!def->wasm_buf) return -1;
    memcpy(def->wasm_buf, wasm_buf, wasm_len);
    def->wasm_len = wasm_len;

    host->agent_count++;
    return 0;
}

int strata_agent_register_js_buf(strata_agent_host *host,
                                  const char *name,
                                  const char *js_source,
                                  const char *sub_endpoint,
                                  const char *req_endpoint,
                                  const char *pub_endpoint,
                                  const char *rep_endpoint) {
    if (!host || !name || !js_source || host->agent_count >= STRATA_MAX_AGENTS) return -1;

    strata_agent_def *def = &host->agents[host->agent_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_JS;
    strncpy(def->name, name, sizeof(def->name) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);
    if (pub_endpoint) strncpy(def->pub_endpoint, pub_endpoint, sizeof(def->pub_endpoint) - 1);
    if (rep_endpoint) strncpy(def->rep_endpoint, rep_endpoint, sizeof(def->rep_endpoint) - 1);

    def->js_source = strdup(js_source);
    if (!def->js_source) return -1;

    host->agent_count++;
    return 0;
}
