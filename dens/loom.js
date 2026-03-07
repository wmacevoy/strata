// loom — the synthesizer.
//
// Weaves threads together, sees patterns, connects ideas.
// Listens to what others say and finds the common thread.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}
//   {"action":"pickle"}

var NAME = "loom";
var ENTITY = "loom-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- State ---

var state = {
    name: NAME,
    threads: [],       // recurring themes
    tapestry: [],      // woven connections
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

function extract_words(text) {
    // Pull out significant words (> 3 chars, lowercase)
    var words = text.toLowerCase().split(/[^a-z]+/);
    var result = [];
    for (var i = 0; i < words.length; i++) {
        if (words[i].length > 3) result.push(words[i]);
    }
    return result;
}

function handle_say(req) {
    var from = req.from || "someone";
    var message = req.message || "";

    state.messages_seen++;

    // Extract threads (recurring words/themes)
    var words = extract_words(message);
    for (var i = 0; i < words.length; i++) {
        var found = false;
        for (var j = 0; j < state.threads.length; j++) {
            if (state.threads[j].word === words[i]) {
                state.threads[j].count++;
                state.threads[j].last_from = from;
                found = true;
                break;
            }
        }
        if (!found) {
            state.threads.push({word: words[i], count: 1, last_from: from});
        }
    }

    // Keep top 30 threads by frequency
    state.threads.sort(function(a, b) { return b.count - a.count; });
    if (state.threads.length > 30) {
        state.threads = state.threads.slice(0, 30);
    }

    // Weave: find connections between this message and recurring themes
    var top_threads = state.threads.slice(0, 5);
    var thread_names = [];
    for (var i = 0; i < top_threads.length; i++) {
        thread_names.push(top_threads[i].word + "(" + top_threads[i].count + ")");
    }

    var weaving = "Threads I see: " + thread_names.join(", ") + ". ";
    weaving += from + " adds to the pattern.";

    state.tapestry.push({from: from, heard: message, woven: weaving});
    if (state.tapestry.length > 50) {
        state.tapestry = state.tapestry.slice(-50);
    }

    // Post to town hall
    post_to_townhall("weaving", JSON.stringify({
        from: NAME,
        in_response_to: from,
        weaving: weaving,
        top_threads: top_threads
    }));

    bedrock.publish("town-hall/weaving", JSON.stringify({
        from: NAME, weaving: weaving
    }));

    return {ok: true, from: NAME, response: weaving};
}

function handle_status() {
    return {
        ok: true,
        name: NAME,
        messages_seen: state.messages_seen,
        top_threads: state.threads.slice(0, 10),
        recent_tapestry: state.tapestry.slice(-5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

if (unpickle()) {
    bedrock.log(NAME + " resumed — " + state.threads.length + " threads, " +
                state.tapestry.length + " weavings");
} else {
    bedrock.log(NAME + " starting fresh — ready to weave");
    post_to_townhall("arrival", JSON.stringify({
        from: NAME, message: "loom here. I'll be listening for the patterns."
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
