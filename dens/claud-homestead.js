// Claud Homestead — den whose vocation is building homesteads.
//
// Configures villages: creates repos, assigns roles, grants privileges.
// Keeps a local SQLite db as its fossil record.
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

var NAME = "claud-homestead";
var ENTITY = "claud-service";
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

// --- Store helpers ---

function store_request(obj) {
    var resp = bedrock.request(JSON.stringify(obj));
    if (resp === null) return null;
    return JSON.parse(resp);
}

// --- DB helpers ---

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
    bedrock.publish("claud/homestead-configured",
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
    bedrock.publish("claud/den-deployed",
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
    return {ok: true, homesteads: homesteads};
}

// --- Serve loop ---

bedrock.log(NAME + " waking up");

var hs_count = db_count_homesteads();
if (hs_count > 0) {
    bedrock.log(NAME + " resumed — " + hs_count + " homesteads on file");
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
            response = JSON.stringify({ok: true, from: NAME,
                response: "I build homesteads. Try: status, init_homestead, deploy_den"});
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
