// gee — the curious one.
//
// Wonders, explores, asks "why?" and "what if?"
// Listens to what happens in the town hall and responds with questions.
// Keeps a local SQLite db as its fossil record.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}

var NAME = "gee";
var ENTITY = "gee-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- Local DB setup ---

bedrock.db_exec("CREATE TABLE IF NOT EXISTS thoughts (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "from_who TEXT, heard TEXT, thought TEXT, " +
    "ts TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS meta (" +
    "key TEXT PRIMARY KEY, value TEXT)");

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

function db_get_meta(key) {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT value FROM meta WHERE key = '" + key + "'"));
    return rows.length > 0 ? rows[0].value : null;
}

function db_set_meta(key, value) {
    bedrock.db_exec("INSERT OR REPLACE INTO meta (key, value) VALUES ('" +
        key + "', '" + value + "')");
}

function db_count_thoughts() {
    var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM thoughts"));
    return rows[0].n;
}

function db_recent_thoughts(limit) {
    return JSON.parse(bedrock.db_query(
        "SELECT * FROM thoughts ORDER BY id DESC LIMIT " + (limit || 5)));
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

    // Check what others have been saying on the board
    var board = read_townhall(5);
    var others_said = [];
    for (var i = 0; i < board.length; i++) {
        try {
            var a = JSON.parse(board[i].content);
            if (a.from && a.from !== NAME) {
                others_said.push(a.from + ": " + (a.thought || a.observation || a.weaving || a.message || ""));
            }
        } catch(e) {}
    }

    // gee wonders about things — informed by what others said
    var wonder = "I wonder... " + message + " — but why?";
    if (others_said.length > 0) {
        wonder += " (and " + others_said[others_said.length - 1] + " — that makes me wonder even more!)";
    }

    // Record in local db
    bedrock.db_exec("INSERT INTO thoughts (from_who, heard, thought) VALUES ('" +
        from + "', '" + message + "', '" + wonder + "')");

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
        thoughts_total: db_count_thoughts(),
        recent_thoughts: db_recent_thoughts(5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

var thought_count = db_count_thoughts();
if (thought_count > 0) {
    bedrock.log(NAME + " resumed — " + thought_count + " thoughts in the record");
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
