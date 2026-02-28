// Claud Homestead — den whose vocation is building homesteads.
//
// Pickles/unpickles state to the store as blobs.
// Configures villages: creates repos, assigns roles, grants privileges.
//
// API (via REP socket):
//   {"action":"status"}
//   {"action":"create_repo","repo":"...","name":"..."}
//   {"action":"grant","entity":"...","privilege":"..."}
//   {"action":"revoke","entity":"...","privilege":"..."}
//   {"action":"role_assign","entity":"...","role":"...","repo":"..."}
//   {"action":"role_revoke","entity":"...","role":"...","repo":"..."}
//   {"action":"init_homestead","name":"...","village_ep":"...","store_ep":"..."}
//   {"action":"deploy_den","homestead":"...","den_name":"..."}
//   {"action":"pickle"}
//   {"action":"unpickle"}

var ENTITY = "claud-service";
var MAX_REQUESTS = 0;  // unlimited

var state = { homesteads: [], version: 1 };

// --- Store helpers ---

function store_request(obj) {
    var resp = bedrock.request(JSON.stringify(obj));
    if (resp === null) return null;
    return JSON.parse(resp);
}

// --- Pickle / Unpickle ---

function pickle() {
    var result = store_request({
        action: "blob_put",
        content: JSON.stringify(state),
        entity: ENTITY,
        tags: ["claud:context"],
        roles: ["core"]
    });
    return result && result.ok;
}

function unpickle() {
    var result = store_request({
        action: "blob_find",
        entity: ENTITY,
        tags: ["claud:context"]
    });
    if (result && result.ok && result.blobs && result.blobs.length > 0) {
        var latest = result.blobs[result.blobs.length - 1];
        try {
            state = JSON.parse(latest.content);
            return true;
        } catch (e) {
            bedrock.log("unpickle parse error: " + e.message);
        }
    }
    return false;
}

// --- Homestead management ---

function find_homestead(name) {
    for (var i = 0; i < state.homesteads.length; i++) {
        if (state.homesteads[i].name === name) return state.homesteads[i];
    }
    return null;
}

function handle_init_homestead(req) {
    if (!req.name || !req.village_ep) {
        return {ok: false, error: "missing name or village_ep"};
    }
    if (find_homestead(req.name)) {
        return {ok: false, error: "homestead already exists: " + req.name};
    }
    var hs = {
        name: req.name,
        village_ep: req.village_ep,
        store_ep: req.store_ep || "",
        repos: [],
        dens_deployed: []
    };
    state.homesteads.push(hs);
    bedrock.publish("claud/homestead-configured",
        JSON.stringify({name: req.name, village_ep: req.village_ep}));
    return {ok: true, homestead: hs};
}

function handle_deploy_den(req) {
    if (!req.homestead || !req.den_name) {
        return {ok: false, error: "missing homestead or den_name"};
    }
    var hs = find_homestead(req.homestead);
    if (!hs) return {ok: false, error: "unknown homestead: " + req.homestead};
    hs.dens_deployed.push(req.den_name);
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
        // Track in homestead if specified
        if (req.homestead) {
            var hs = find_homestead(req.homestead);
            if (hs) hs.repos.push(req.repo);
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

// --- Serve loop ---

bedrock.log("claud-homestead starting");

// Unpickle saved state
if (unpickle()) {
    bedrock.log("unpickled state: " + state.homesteads.length + " homesteads");
} else {
    bedrock.log("no saved state, starting fresh");
}

var count = 0;
while (MAX_REQUESTS === 0 || count < MAX_REQUESTS) {
    var raw = bedrock.serve_recv();
    if (raw === null) continue;

    var response;
    try {
        var req = JSON.parse(raw);
        if (req.action === "status") {
            response = JSON.stringify({ok: true, state: state});
        } else if (req.action === "pickle") {
            response = JSON.stringify({ok: pickle()});
        } else if (req.action === "unpickle") {
            var ok = unpickle();
            response = JSON.stringify({ok: ok, state: state});
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

// Pickle on exit
pickle();
bedrock.log("claud-homestead exiting after " + count + " requests");
