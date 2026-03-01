// Gatekeeper Den — per-project access control
//
// Handles join requests for a project. Enforces "destination decides":
// incoming journeymen request roles, authorized gatekeepers approve/deny.
//
// State is stored as artifacts in the gatekeeper's repo:
//   type "join_request" — pending request
//   type "join_approved" — approved request (role assigned)
//   type "join_denied"   — denied request
//
// API (serve via ZMQ REP):
//   REQUEST:  {"action":"request_join","entity":"alice","role":"developer"}
//   APPROVE:  {"action":"approve_join","request_id":"...","approver":"admin"}
//   DENY:     {"action":"deny_join","request_id":"...","approver":"admin","reason":"..."}
//   LIST:     {"action":"list_requests"}
//   LIST:     {"action":"list_requests","status":"pending"}

var REPO = "gatekeeper";
var STORE_ENTITY = "gatekeeper-service";
var MAX_REQUESTS = 0;  // unlimited

function store_put(type, content, roles) {
    var req = JSON.stringify({
        action: "put",
        repo: REPO,
        type: type,
        content: content,
        author: STORE_ENTITY,
        roles: roles || ["gatekeeper"]
    });
    var resp = bedrock.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function store_list(type) {
    var req = JSON.stringify({
        action: "list",
        repo: REPO,
        type: type,
        entity: STORE_ENTITY
    });
    var resp = bedrock.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function store_get(id) {
    var req = JSON.stringify({
        action: "get",
        id: id,
        entity: STORE_ENTITY
    });
    var resp = bedrock.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function store_role_assign(entity, role, repo) {
    var req = JSON.stringify({
        action: "role_assign",
        entity: entity,
        role: role,
        repo: repo
    });
    var resp = bedrock.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function handle_request_join(req) {
    if (!req.entity || !req.role) {
        return JSON.stringify({ok: false, error: "entity and role required"});
    }

    var project = req.project || REPO;

    var request_data = JSON.stringify({
        entity: req.entity,
        role: req.role,
        project: project,
        status: "pending",
        requested_at: new Date().toISOString()
    });

    var result = store_put("join_request", request_data, ["gatekeeper"]);
    if (!result || !result.ok) {
        return JSON.stringify({ok: false, error: "failed to store request"});
    }

    // Publish event
    var notification = JSON.stringify({
        entity: req.entity,
        role: req.role,
        project: project,
        request_id: result.id
    });
    bedrock.publish("gatekeeper/request", notification);

    return JSON.stringify({ok: true, request_id: result.id});
}

function handle_approve_join(req) {
    if (!req.request_id || !req.approver) {
        return JSON.stringify({ok: false, error: "request_id and approver required"});
    }

    // Get the original request
    var original = store_get(req.request_id);
    if (!original || !original.ok) {
        return JSON.stringify({ok: false, error: "request not found"});
    }

    var request_data = JSON.parse(original.content);
    if (request_data.status !== "pending") {
        return JSON.stringify({ok: false, error: "request already " + request_data.status});
    }

    // Assign the role
    var assign = store_role_assign(request_data.entity, request_data.role, request_data.project);
    if (!assign || !assign.ok) {
        return JSON.stringify({ok: false, error: "role assignment failed"});
    }

    // Record approval
    var approval_data = JSON.stringify({
        original_request_id: req.request_id,
        entity: request_data.entity,
        role: request_data.role,
        project: request_data.project,
        approver: req.approver,
        approved_at: new Date().toISOString()
    });
    store_put("join_approved", approval_data, ["gatekeeper"]);

    // Publish event
    var notification = JSON.stringify({
        entity: request_data.entity,
        role: request_data.role,
        project: request_data.project,
        approver: req.approver
    });
    bedrock.publish("gatekeeper/approved", notification);

    return JSON.stringify({ok: true, entity: request_data.entity, role: request_data.role, project: request_data.project});
}

function handle_deny_join(req) {
    if (!req.request_id || !req.approver) {
        return JSON.stringify({ok: false, error: "request_id and approver required"});
    }

    var original = store_get(req.request_id);
    if (!original || !original.ok) {
        return JSON.stringify({ok: false, error: "request not found"});
    }

    var request_data = JSON.parse(original.content);
    if (request_data.status !== "pending") {
        return JSON.stringify({ok: false, error: "request already " + request_data.status});
    }

    // Record denial
    var denial_data = JSON.stringify({
        original_request_id: req.request_id,
        entity: request_data.entity,
        role: request_data.role,
        project: request_data.project,
        approver: req.approver,
        reason: req.reason || "",
        denied_at: new Date().toISOString()
    });
    store_put("join_denied", denial_data, ["gatekeeper"]);

    // Publish event
    var notification = JSON.stringify({
        entity: request_data.entity,
        role: request_data.role,
        project: request_data.project,
        approver: req.approver
    });
    bedrock.publish("gatekeeper/denied", notification);

    return JSON.stringify({ok: true, denied: true});
}

function handle_list_requests(req) {
    var status = req.status;  // optional filter: "pending", "approved", "denied"

    var requests = store_list("join_request");
    var approved = store_list("join_approved");
    var denied = store_list("join_denied");

    // Build sets of handled request IDs
    var approved_ids = {};
    var denied_ids = {};
    if (approved && approved.ok && approved.artifacts) {
        for (var i = 0; i < approved.artifacts.length; i++) {
            try {
                var data = JSON.parse(approved.artifacts[i].content);
                approved_ids[data.original_request_id] = true;
            } catch(e) {}
        }
    }
    if (denied && denied.ok && denied.artifacts) {
        for (var i = 0; i < denied.artifacts.length; i++) {
            try {
                var data = JSON.parse(denied.artifacts[i].content);
                denied_ids[data.original_request_id] = true;
            } catch(e) {}
        }
    }

    var result = [];
    if (requests && requests.ok && requests.artifacts) {
        for (var i = 0; i < requests.artifacts.length; i++) {
            var a = requests.artifacts[i];
            try {
                var data = JSON.parse(a.content);
                var req_status;
                if (approved_ids[a.id]) req_status = "approved";
                else if (denied_ids[a.id]) req_status = "denied";
                else req_status = "pending";

                if (!status || status === req_status) {
                    result.push({
                        request_id: a.id,
                        entity: data.entity,
                        role: data.role,
                        project: data.project,
                        status: req_status,
                        requested_at: data.requested_at
                    });
                }
            } catch(e) {}
        }
    }

    return JSON.stringify({ok: true, requests: result});
}

// Serve loop
bedrock.log("gatekeeper den starting");
var count = 0;
while (MAX_REQUESTS === 0 || count < MAX_REQUESTS) {
    var raw = bedrock.serve_recv();
    if (raw === null) continue;

    var response;
    try {
        var req = JSON.parse(raw);
        if (req.action === "request_join") {
            response = handle_request_join(req);
        } else if (req.action === "approve_join") {
            response = handle_approve_join(req);
        } else if (req.action === "deny_join") {
            response = handle_deny_join(req);
        } else if (req.action === "list_requests") {
            response = handle_list_requests(req);
        } else {
            response = JSON.stringify({ok: false, error: "unknown action: " + req.action});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "parse error: " + e.message});
    }

    bedrock.serve_send(response);
    count++;
}
bedrock.log("gatekeeper den exiting after " + count + " requests");
