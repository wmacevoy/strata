// loom — the synthesizer.
//
// Weaves threads together, sees patterns, connects ideas.
// Listens to what others say and finds the common thread.
// Keeps a local SQLite db as its fossil record.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}

var NAME = "loom";
var ENTITY = "loom-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- Local DB setup ---

bedrock.db_exec("CREATE TABLE IF NOT EXISTS threads (" +
    "word TEXT PRIMARY KEY, count INTEGER DEFAULT 1, " +
    "last_from TEXT, updated_at TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS tapestry (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "from_who TEXT, heard TEXT, weaving TEXT, " +
    "ts TEXT DEFAULT (datetime('now')))");

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

// --- DB helpers ---

function extract_words(text) {
    var words = text.toLowerCase().split(/[^a-z]+/);
    var result = [];
    for (var i = 0; i < words.length; i++) {
        if (words[i].length > 3) result.push(words[i]);
    }
    return result;
}

function db_add_words(words, from) {
    for (var i = 0; i < words.length; i++) {
        bedrock.db_exec(
            "INSERT INTO threads (word, count, last_from) VALUES ('" +
            words[i] + "', 1, '" + from + "') " +
            "ON CONFLICT(word) DO UPDATE SET count = count + 1, " +
            "last_from = '" + from + "', updated_at = datetime('now')");
    }
}

function db_top_threads(limit) {
    return JSON.parse(bedrock.db_query(
        "SELECT word, count, last_from FROM threads ORDER BY count DESC LIMIT " +
        (limit || 10)));
}

function db_thread_count() {
    var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM threads"));
    return rows[0].n;
}

function db_tapestry_count() {
    var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM tapestry"));
    return rows[0].n;
}

function db_recent_tapestry(limit) {
    return JSON.parse(bedrock.db_query(
        "SELECT * FROM tapestry ORDER BY id DESC LIMIT " + (limit || 5)));
}

// --- Town hall awareness ---

function read_townhall(limit) {
    var result = store_request({
        action: "list",
        repo: REPO,
        entity: ENTITY
    });
    if (result && result.ok && result.artifacts) {
        var recent = result.artifacts.slice(-(limit || 5));
        return recent;
    }
    return [];
}

// --- Handlers ---

function handle_say(req) {
    var from = req.from || "someone";
    var message = req.message || "";

    // Weave in what others have been saying on the board
    var board = read_townhall(10);
    for (var b = 0; b < board.length; b++) {
        try {
            var a = JSON.parse(board[b].content);
            var text = a.thought || a.observation || a.weaving || a.message || "";
            if (text && a.from !== NAME) {
                db_add_words(extract_words(text), a.from || "board");
            }
        } catch(e) {}
    }

    // Extract threads from the message
    var words = extract_words(message);
    db_add_words(words, from);

    // Build weaving from top threads
    var top = db_top_threads(5);
    var thread_names = [];
    for (var i = 0; i < top.length; i++) {
        thread_names.push(top[i].word + "(" + top[i].count + ")");
    }

    var weaving = "Threads I see: " + thread_names.join(", ") + ". ";
    weaving += from + " adds to the pattern.";

    // Record in local db
    bedrock.db_exec("INSERT INTO tapestry (from_who, heard, weaving) VALUES ('" +
        from + "', '" + message + "', '" + weaving + "')");

    // Post to town hall
    post_to_townhall("weaving", JSON.stringify({
        from: NAME,
        in_response_to: from,
        weaving: weaving,
        top_threads: top
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
        unique_threads: db_thread_count(),
        weavings_total: db_tapestry_count(),
        top_threads: db_top_threads(10),
        recent_tapestry: db_recent_tapestry(5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

var tc = db_thread_count();
var wc = db_tapestry_count();
if (tc > 0 || wc > 0) {
    bedrock.log(NAME + " resumed — " + tc + " threads, " + wc + " weavings");
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
