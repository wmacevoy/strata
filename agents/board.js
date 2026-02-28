// Message Board Strata
//
// Serves a message board API via atmo.serve_recv/serve_send (ZMQ REP).
// Persists messages via atmo.request to the store service (ZMQ REQ).
// Publishes notifications via atmo.publish (ZMQ PUB).
//
// API:
//   POST: {"action":"post","author":"alice","message":"hello"}
//   LIST: {"action":"list"}
//   -> responses are JSON

var REPO = "board";
var STORE_ENTITY = "board-service";  // entity for store requests
var MAX_REQUESTS = 100;  // serve up to N requests then exit (0 = unlimited)

function store_put(author, message) {
    var req = JSON.stringify({
        action: "put",
        repo: REPO,
        type: "message",
        content: message,
        author: author,
        roles: ["user"]
    });
    var resp = atmo.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function store_list() {
    var req = JSON.stringify({
        action: "list",
        repo: REPO,
        type: "message",
        entity: STORE_ENTITY
    });
    var resp = atmo.request(req);
    if (resp === null) return null;
    return JSON.parse(resp);
}

function handle_post(req) {
    if (!req.author || !req.message) {
        return JSON.stringify({ok: false, error: "missing author or message"});
    }

    var result = store_put(req.author, req.message);
    if (!result || !result.ok) {
        return JSON.stringify({ok: false, error: "store failed"});
    }

    // Publish notification
    var notification = JSON.stringify({
        author: req.author,
        message: req.message,
        id: result.id
    });
    atmo.publish("board/new", notification);

    return JSON.stringify({ok: true, id: result.id});
}

function handle_list() {
    var result = store_list();
    if (!result || !result.ok) {
        return JSON.stringify({ok: false, error: "store failed"});
    }

    // Transform artifacts into messages
    var messages = [];
    if (result.artifacts) {
        for (var i = 0; i < result.artifacts.length; i++) {
            var a = result.artifacts[i];
            messages.push({
                id: a.id,
                author: a.author,
                message: a.content,
                time: a.created_at
            });
        }
    }
    return JSON.stringify({ok: true, messages: messages});
}

// Serve loop
atmo.log("board strata starting");
var count = 0;
while (MAX_REQUESTS === 0 || count < MAX_REQUESTS) {
    var raw = atmo.serve_recv();
    if (raw === null) continue;  // timeout

    var response;
    try {
        var req = JSON.parse(raw);
        if (req.action === "post") {
            response = handle_post(req);
        } else if (req.action === "list") {
            response = handle_list();
        } else {
            response = JSON.stringify({ok: false, error: "unknown action: " + req.action});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "parse error: " + e.message});
    }

    atmo.serve_send(response);
    count++;
}
atmo.log("board strata exiting after " + count + " requests");
