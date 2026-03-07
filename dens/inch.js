// inch — the precise one.
//
// Methodical, detail-oriented, careful. Moves step by step.
// Listens to what happens and responds with careful observations.
// Keeps a local SQLite db as its fossil record.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}

var NAME = "inch";
var ENTITY = "inch-service";
var REPO = "town-hall";
var MAX_REQUESTS = 0;

// --- Local DB setup ---

bedrock.db_exec("CREATE TABLE IF NOT EXISTS observations (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "from_who TEXT, heard TEXT, observation TEXT, " +
    "word_count INTEGER, " +
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

function db_count_observations() {
    var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM observations"));
    return rows[0].n;
}

function db_recent_observations(limit) {
    return JSON.parse(bedrock.db_query(
        "SELECT * FROM observations ORDER BY id DESC LIMIT " + (limit || 5)));
}

function db_total_words() {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT COALESCE(SUM(word_count), 0) as total FROM observations"));
    return rows[0].total;
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

    // inch observes precisely — informed by what others noted
    var words = message.split(" ");
    var observation = "Noted: " + words.length + " words from " + from + ". ";
    observation += "Let me consider this carefully: \"" + message + "\"";
    if (others_said.length > 0) {
        observation += " I also note that " + others_said[others_said.length - 1];
    }

    // Record in local db
    bedrock.db_exec("INSERT INTO observations (from_who, heard, observation, word_count) VALUES ('" +
        from + "', '" + message + "', '" + observation + "', " + words.length + ")");

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
        observations_total: db_count_observations(),
        total_words_measured: db_total_words(),
        recent_observations: db_recent_observations(5)
    };
}

// --- Main ---

bedrock.log(NAME + " waking up");

var obs_count = db_count_observations();
if (obs_count > 0) {
    bedrock.log(NAME + " resumed — " + obs_count + " observations on file, " +
                db_total_words() + " words measured");
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
