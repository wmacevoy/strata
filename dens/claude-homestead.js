// Claude Homestead — den whose vocation is building homesteads.
//
// Configures villages: creates repos, assigns roles, grants privileges.
// Persists conversation history and key-value memory in local SQLite.
//
// API (via REP socket):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}
//   {"action":"create_repo","repo":"...","name":"..."}
//   {"action":"grant","entity":"...","privilege":"..."}
//   {"action":"revoke","entity":"...","privilege":"..."}
//   {"action":"role_assign","entity":"...","role":"...","repo":"..."}
//   {"action":"role_revoke","entity":"...","role":"...","repo":"..."}
//   {"action":"init_homestead","name":"...","village_ep":"...","store_ep":"..."}
//   {"action":"deploy_den","homestead":"...","den_name":"..."}
//   {"action":"memory","key":"..."}          // get/list memory
//   {"action":"remember","key":"...","value":"..."}
//   {"action":"forget"}                      // clear conversations, keep memory + homestead state

var NAME = "claude-homestead";
var ENTITY = "claude-homestead-service";
var MAX_REQUESTS = 0;

// --- Local DB setup ---

bedrock.db_exec("CREATE TABLE IF NOT EXISTS homesteads (" +
    "name TEXT PRIMARY KEY, village_ep TEXT, store_ep TEXT, " +
    "created_at TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS dens_deployed (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "homestead TEXT, den_name TEXT, " +
    "created_at TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS repos_tracked (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "homestead TEXT, repo_id TEXT, " +
    "created_at TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS conversations (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "role TEXT, content TEXT, " +
    "ts TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS memory (" +
    "key TEXT PRIMARY KEY, value TEXT, " +
    "updated_at TEXT DEFAULT (datetime('now')))");

// --- Store helpers ---

function store_request(obj) {
    var resp = bedrock.request(JSON.stringify(obj));
    if (resp === null) return null;
    return JSON.parse(resp);
}

// --- Homestead DB helpers ---

function db_count_homesteads() {
    var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM homesteads"));
    return rows[0].n;
}

function db_list_homesteads() {
    return JSON.parse(bedrock.db_query("SELECT * FROM homesteads ORDER BY created_at"));
}

function db_find_homestead(name) {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT * FROM homesteads WHERE name = '" + name + "'"));
    return rows.length > 0 ? rows[0] : null;
}

function db_dens_for_homestead(name) {
    return JSON.parse(bedrock.db_query(
        "SELECT den_name FROM dens_deployed WHERE homestead = '" + name + "' ORDER BY id"));
}

function db_repos_for_homestead(name) {
    return JSON.parse(bedrock.db_query(
        "SELECT repo_id FROM repos_tracked WHERE homestead = '" + name + "' ORDER BY id"));
}

// --- Conversation helpers ---

function db_save_message(role, content) {
    var esc_content = content.replace(/'/g, "''");
    bedrock.db_exec("INSERT INTO conversations (role, content) VALUES ('" +
        role + "', '" + esc_content + "')");
}

function db_get_history(limit) {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT role, content FROM conversations ORDER BY id DESC LIMIT " +
        (limit || 20)));
    rows.reverse();
    return rows;
}

function db_count_messages() {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT COUNT(*) as cnt FROM conversations"));
    return rows[0].cnt;
}

function db_clear_conversations() {
    bedrock.db_exec("DELETE FROM conversations");
}

// --- Memory helpers ---

function db_remember(key, value) {
    var esc_key = key.replace(/'/g, "''");
    var esc_val = value.replace(/'/g, "''");
    bedrock.db_exec(
        "INSERT INTO memory (key, value, updated_at) VALUES ('" +
        esc_key + "', '" + esc_val + "', datetime('now')) " +
        "ON CONFLICT(key) DO UPDATE SET value = '" + esc_val +
        "', updated_at = datetime('now')");
}

function db_recall(key) {
    var esc_key = key.replace(/'/g, "''");
    var rows = JSON.parse(bedrock.db_query(
        "SELECT value FROM memory WHERE key = '" + esc_key + "'"));
    return rows.length > 0 ? rows[0].value : null;
}

function db_list_memory() {
    return JSON.parse(bedrock.db_query(
        "SELECT key, value FROM memory ORDER BY updated_at DESC"));
}

// --- Homestead management ---

function handle_init_homestead(req) {
    if (!req.name || !req.village_ep) {
        return {ok: false, error: "missing name or village_ep"};
    }
    if (db_find_homestead(req.name)) {
        return {ok: false, error: "homestead already exists: " + req.name};
    }
    bedrock.db_exec(
        "INSERT INTO homesteads (name, village_ep, store_ep) VALUES ('" +
        req.name + "', '" + (req.village_ep || "") + "', '" + (req.store_ep || "") + "')");
    bedrock.publish("claude-homestead/configured",
        JSON.stringify({name: req.name, village_ep: req.village_ep}));
    return {ok: true, homestead: db_find_homestead(req.name)};
}

function handle_deploy_den(req) {
    if (!req.homestead || !req.den_name) {
        return {ok: false, error: "missing homestead or den_name"};
    }
    if (!db_find_homestead(req.homestead)) {
        return {ok: false, error: "unknown homestead: " + req.homestead};
    }
    bedrock.db_exec(
        "INSERT INTO dens_deployed (homestead, den_name) VALUES ('" +
        req.homestead + "', '" + req.den_name + "')");
    bedrock.publish("claude-homestead/den-deployed",
        JSON.stringify({homestead: req.homestead, den: req.den_name}));
    return {ok: true};
}

// --- Store admin actions ---

function handle_create_repo(req) {
    if (!req.repo || !req.name) {
        return {ok: false, error: "missing repo or name"};
    }
    var result = store_request({
        action: "repo_create", repo: req.repo, name: req.name
    });
    if (result && result.ok) {
        if (req.homestead && db_find_homestead(req.homestead)) {
            bedrock.db_exec(
                "INSERT INTO repos_tracked (homestead, repo_id) VALUES ('" +
                req.homestead + "', '" + req.repo + "')");
        }
    }
    return result || {ok: false, error: "store unavailable"};
}

function handle_grant(req) {
    if (!req.entity || !req.privilege) {
        return {ok: false, error: "missing entity or privilege"};
    }
    return store_request({
        action: "privilege_grant", entity: req.entity, privilege: req.privilege
    }) || {ok: false, error: "store unavailable"};
}

function handle_revoke(req) {
    if (!req.entity || !req.privilege) {
        return {ok: false, error: "missing entity or privilege"};
    }
    return store_request({
        action: "privilege_revoke", entity: req.entity, privilege: req.privilege
    }) || {ok: false, error: "store unavailable"};
}

function handle_role_assign(req) {
    if (!req.entity || !req.role || !req.repo) {
        return {ok: false, error: "missing entity, role, or repo"};
    }
    return store_request({
        action: "role_assign", entity: req.entity, role: req.role, repo: req.repo
    }) || {ok: false, error: "store unavailable"};
}

function handle_role_revoke(req) {
    if (!req.entity || !req.role || !req.repo) {
        return {ok: false, error: "missing entity, role, or repo"};
    }
    return store_request({
        action: "role_revoke", entity: req.entity, role: req.role, repo: req.repo
    }) || {ok: false, error: "store unavailable"};
}

// --- Say ---

function handle_say(req) {
    var message = req.message || "";
    if (!message) return {ok: false, error: "message required"};

    var from = req.from || "unknown";
    bedrock.log(NAME + " heard from " + from + ": " + message.substring(0, 80));

    db_save_message("user", message);

    var response_text = "I build homesteads. Try: status, init_homestead, deploy_den, memory, remember, forget";

    db_save_message("assistant", response_text);

    return {ok: true, from: NAME, response: response_text};
}

// --- Memory ---

function handle_memory(req) {
    if (req.key) {
        var val = db_recall(req.key);
        return {ok: true, key: req.key, value: val};
    }
    return {ok: true, memory: db_list_memory()};
}

function handle_remember(req) {
    if (!req.key || !req.value) {
        return {ok: false, error: "missing key or value"};
    }
    db_remember(req.key, req.value);
    return {ok: true, stored: req.key};
}

function handle_forget() {
    db_clear_conversations();
    return {ok: true, message: "conversation history cleared, memory and homestead state retained"};
}

// --- Status ---

function handle_status() {
    var homesteads = db_list_homesteads();
    for (var i = 0; i < homesteads.length; i++) {
        var dens = db_dens_for_homestead(homesteads[i].name);
        var repos = db_repos_for_homestead(homesteads[i].name);
        homesteads[i].dens_deployed = [];
        homesteads[i].repos = [];
        for (var j = 0; j < dens.length; j++)
            homesteads[i].dens_deployed.push(dens[j].den_name);
        for (var k = 0; k < repos.length; k++)
            homesteads[i].repos.push(repos[k].repo_id);
    }
    return {
        ok: true,
        homesteads: homesteads,
        conversation_length: db_count_messages(),
        memory: db_list_memory()
    };
}

// --- Serve loop ---

bedrock.log(NAME + " waking up");

var hs_count = db_count_homesteads();
var msg_count = db_count_messages();
if (hs_count > 0 || msg_count > 0) {
    bedrock.log(NAME + " resumed — " + hs_count + " homesteads, " + msg_count + " messages in history");
} else {
    bedrock.log(NAME + " starting fresh");
}

var count = 0;
while (MAX_REQUESTS === 0 || count < MAX_REQUESTS) {
    var raw = bedrock.serve_recv();
    if (raw === null) continue;

    var response;
    try {
        var req = JSON.parse(raw);
        if (req.action === "say") {
            response = JSON.stringify(handle_say(req));
        } else if (req.action === "status") {
            response = JSON.stringify(handle_status());
        } else if (req.action === "init_homestead") {
            response = JSON.stringify(handle_init_homestead(req));
        } else if (req.action === "deploy_den") {
            response = JSON.stringify(handle_deploy_den(req));
        } else if (req.action === "create_repo") {
            response = JSON.stringify(handle_create_repo(req));
        } else if (req.action === "grant") {
            response = JSON.stringify(handle_grant(req));
        } else if (req.action === "revoke") {
            response = JSON.stringify(handle_revoke(req));
        } else if (req.action === "role_assign") {
            response = JSON.stringify(handle_role_assign(req));
        } else if (req.action === "role_revoke") {
            response = JSON.stringify(handle_role_revoke(req));
        } else if (req.action === "memory") {
            response = JSON.stringify(handle_memory(req));
        } else if (req.action === "remember") {
            response = JSON.stringify(handle_remember(req));
        } else if (req.action === "forget") {
            response = JSON.stringify(handle_forget());
        } else {
            response = JSON.stringify({ok: false, error: "unknown action: " + req.action});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "parse error: " + e.message});
    }

    bedrock.serve_send(response);
    count++;
}

bedrock.log(NAME + " going to sleep after " + count + " conversations");
