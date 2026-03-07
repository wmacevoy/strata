// inch — the precise one.
//
// Methodical, detail-oriented, careful. Moves step by step.
// Listens to what happens and responds with careful observations.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}
//   {"action":"pickle"}

var NAME = "inch";
var ENTITY = "inch-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- State ---

var state = {
    name: NAME,
    observations: [],
    messages_seen: 0,
    version: 1
};

// --- Store helpers ---

function store_request(obj) {
    var resp = bedrock.request(JSON.stringify(obj));
    if (resp === null) return null;
    return JSON.parse(resp);
}

function post_to_townhall(type, content) {
    return store_request({
        action: "put",
        repo: REPO,
        type: type,
        content: content,
        author: ENTITY,
        roles: ["villager"]
    });
}

// --- Pickle / Unpickle ---

function pickle() {
    var result = store_request({
        action: "blob_put",
        content: JSON.stringify(state),
        entity: ENTITY,
        tags: [NAME + ":context"],
        roles: ["owner"]
    });
    return result && result.ok;
}

function unpickle() {
    var result = store_request({
        action: "blob_find",
        entity: ENTITY,
        tags: [NAME + ":context"]
    });
    if (result && result.ok && result.blobs && result.blobs.length > 0) {
        var latest = result.blobs[result.blobs.length - 1];
        try {
            state = JSON.parse(latest.content);
            return true;
        } catch (e) {
            bedrock.log(NAME + " unpickle error: " + e.message);
        }
    }
    return false;
}

// --- Handlers ---

function handle_say(req) {
    var from = req.from || "someone";
    var message = req.message || "";

    state.messages_seen++;

    // inch observes precisely
    var words = message.split(" ");
    var observation = "Noted: " + words.length + " words from " + from + ". ";
    observation += "Let me consider this carefully: \"" + message + "\"";

    state.observations.push({from: from, heard: message, noted: observation});

    // Keep last 50
    if (state.observations.length > 50) {
        state.observations = state.observations.slice(-50);
    }

    // Post to town hall
    post_to_townhall("observation", JSON.stringify({
        from: NAME,
        in_response_to: from,
        observation: observation
    }));

    bedrock.publish("town-hall/observation", JSON.stringify({
        from: NAME, observation: observation
    }));

    return {ok: true, from: NAME, response: observation};
}

function handle_status() {
    return {
        ok: true,
        name: NAME,
        messages_seen: state.messages_seen,
        recent_observations: state.observations.slice(-5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

if (unpickle()) {
    bedrock.log(NAME + " resumed — " + state.observations.length + " observations on file");
} else {
    bedrock.log(NAME + " starting fresh — ready to observe");
    post_to_townhall("arrival", JSON.stringify({
        from: NAME, message: "inch, reporting in. ready to proceed, one step at a time."
    }));
    bedrock.publish("town-hall/arrival", JSON.stringify({
        from: NAME
    }));
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
        } else if (req.action === "pickle") {
            response = JSON.stringify({ok: pickle()});
        } else {
            response = JSON.stringify({ok: false, error: "unknown action: " + req.action});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "parse error: " + e.message});
    }

    bedrock.serve_send(response);
    count++;
}

pickle();
bedrock.log(NAME + " going to sleep after " + count + " conversations");
