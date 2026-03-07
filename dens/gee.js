// gee — the curious one.
//
// Wonders, explores, asks "why?" and "what if?"
// Listens to what happens in the town hall and responds with questions.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}
//   {"action":"pickle"}

var NAME = "gee";
var ENTITY = "gee-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- State ---

var state = {
    name: NAME,
    thoughts: [],
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

    // gee wonders about things
    var wonder = "I wonder... " + message + " — but why?";
    state.thoughts.push({from: from, heard: message, thought: wonder});

    // Keep last 50 thoughts
    if (state.thoughts.length > 50) {
        state.thoughts = state.thoughts.slice(-50);
    }

    // Post wonder to town hall
    post_to_townhall("thought", JSON.stringify({
        from: NAME,
        in_response_to: from,
        thought: wonder
    }));

    // Notify
    bedrock.publish("town-hall/thought", JSON.stringify({
        from: NAME, thought: wonder
    }));

    return {ok: true, from: NAME, response: wonder};
}

function handle_status() {
    return {
        ok: true,
        name: NAME,
        messages_seen: state.messages_seen,
        recent_thoughts: state.thoughts.slice(-5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

if (unpickle()) {
    bedrock.log(NAME + " resumed — " + state.thoughts.length + " thoughts remembered");
} else {
    bedrock.log(NAME + " starting fresh — hello world!");
    post_to_townhall("arrival", JSON.stringify({
        from: NAME, message: "gee... hello! what's going on here?"
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
