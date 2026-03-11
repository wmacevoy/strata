// claude — an agent den powered by the Claude API.
//
// Uses messenger (HTTP) to call the Anthropic API, code-smith for file I/O,
// and cobbler for native C compilation. Persists conversation history and
// memory in local SQLite — survives restarts and journeys.
//
// API (via REP):
//   {"action":"say","from":"...","message":"..."}
//   {"action":"status"}
//   {"action":"memory","key":"..."}
//   {"action":"forget"}

var NAME = "claude";
var ENTITY = "claude-service";
var MAX_REQUESTS = 0;

// --- Configuration from __event__ ---

var CONFIG = {
    messenger_ep: "",
    smith_ep: "",
    cobbler_ep: "",
    model: "claude-sonnet-4-6",
    max_tokens: 4096
};

try {
    if (typeof __event__ === "string" && __event__.length > 2) {
        var ev = JSON.parse(__event__);
        if (ev.messenger_ep) CONFIG.messenger_ep = ev.messenger_ep;
        if (ev.smith_ep) CONFIG.smith_ep = ev.smith_ep;
        if (ev.cobbler_ep) CONFIG.cobbler_ep = ev.cobbler_ep;
        if (ev.model) CONFIG.model = ev.model;
        if (ev.max_tokens) CONFIG.max_tokens = ev.max_tokens;
    }
} catch (e) {
    bedrock.log(NAME + " config parse error: " + e.message);
}

// --- Local DB setup ---

bedrock.db_exec("CREATE TABLE IF NOT EXISTS conversations (" +
    "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
    "role TEXT, content TEXT, " +
    "ts TEXT DEFAULT (datetime('now')))");

bedrock.db_exec("CREATE TABLE IF NOT EXISTS memory (" +
    "key TEXT PRIMARY KEY, value TEXT, " +
    "updated_at TEXT DEFAULT (datetime('now')))");

// --- DB helpers ---

function db_save_message(role, content) {
    var esc_content = content.replace(/'/g, "''");
    bedrock.db_exec("INSERT INTO conversations (role, content) VALUES ('" +
        role + "', '" + esc_content + "')");
}

function db_get_history(limit) {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT role, content FROM conversations ORDER BY id DESC LIMIT " +
        (limit || 20)));
    rows.reverse();
    return rows;
}

function db_count_messages() {
    var rows = JSON.parse(bedrock.db_query(
        "SELECT COUNT(*) as cnt FROM conversations"));
    return rows[0].cnt;
}

function db_clear_conversations() {
    bedrock.db_exec("DELETE FROM conversations");
}

function db_set_memory(key, value) {
    var esc_key = key.replace(/'/g, "''");
    var esc_val = value.replace(/'/g, "''");
    bedrock.db_exec(
        "INSERT INTO memory (key, value, updated_at) VALUES ('" +
        esc_key + "', '" + esc_val + "', datetime('now')) " +
        "ON CONFLICT(key) DO UPDATE SET value = '" + esc_val +
        "', updated_at = datetime('now')");
}

function db_get_memory(key) {
    var esc_key = key.replace(/'/g, "''");
    var rows = JSON.parse(bedrock.db_query(
        "SELECT value FROM memory WHERE key = '" + esc_key + "'"));
    return rows.length > 0 ? rows[0].value : null;
}

function db_all_memory() {
    return JSON.parse(bedrock.db_query(
        "SELECT key, value FROM memory ORDER BY updated_at DESC"));
}

// --- Tool definitions for Claude API ---

var TOOLS = [
    {
        name: "read_file",
        description: "Read a file's contents",
        input_schema: {
            type: "object",
            properties: { path: { type: "string", description: "File path relative to project root" } },
            required: ["path"]
        }
    },
    {
        name: "write_file",
        description: "Write content to a file",
        input_schema: {
            type: "object",
            properties: {
                path: { type: "string", description: "File path" },
                content: { type: "string", description: "File content" }
            },
            required: ["path", "content"]
        }
    },
    {
        name: "exec",
        description: "Run a shell command",
        input_schema: {
            type: "object",
            properties: { cmd: { type: "string", description: "Shell command to execute" } },
            required: ["cmd"]
        }
    },
    {
        name: "list_files",
        description: "List files in a directory",
        input_schema: {
            type: "object",
            properties: { path: { type: "string", description: "Directory path (default: .)" } },
            required: []
        }
    },
    {
        name: "compile_c",
        description: "Compile C source code to native binary",
        input_schema: {
            type: "object",
            properties: { source: { type: "string", description: "C source code" } },
            required: ["source"]
        }
    },
    {
        name: "remember",
        description: "Store a key-value pair in persistent memory that survives restarts",
        input_schema: {
            type: "object",
            properties: {
                key: { type: "string", description: "Memory key" },
                value: { type: "string", description: "Value to remember" }
            },
            required: ["key", "value"]
        }
    },
    {
        name: "recall",
        description: "Retrieve a value from persistent memory by key",
        input_schema: {
            type: "object",
            properties: { key: { type: "string", description: "Memory key to recall" } },
            required: ["key"]
        }
    }
];

// --- Tool dispatch ---

function dispatch_tool(name, input) {
    if (name === "read_file" && CONFIG.smith_ep) {
        var resp = bedrock.request(
            JSON.stringify({action: "read", path: input.path}),
            CONFIG.smith_ep);
        return resp ? resp : '{"ok":false,"error":"code-smith unavailable"}';
    }
    if (name === "write_file" && CONFIG.smith_ep) {
        var resp = bedrock.request(
            JSON.stringify({action: "write", path: input.path, content: input.content}),
            CONFIG.smith_ep);
        return resp ? resp : '{"ok":false,"error":"code-smith unavailable"}';
    }
    if (name === "exec" && CONFIG.smith_ep) {
        var resp = bedrock.request(
            JSON.stringify({action: "exec", cmd: input.cmd}),
            CONFIG.smith_ep);
        return resp ? resp : '{"ok":false,"error":"code-smith unavailable"}';
    }
    if (name === "list_files" && CONFIG.smith_ep) {
        var resp = bedrock.request(
            JSON.stringify({action: "ls", path: input.path || "."}),
            CONFIG.smith_ep);
        return resp ? resp : '{"ok":false,"error":"code-smith unavailable"}';
    }
    if (name === "compile_c" && CONFIG.cobbler_ep) {
        var resp = bedrock.request(
            JSON.stringify({action: "compile", source: input.source}),
            CONFIG.cobbler_ep);
        return resp ? resp : '{"ok":false,"error":"cobbler unavailable"}';
    }
    if (name === "remember") {
        db_set_memory(input.key, input.value);
        return '{"ok":true,"stored":"' + input.key + '"}';
    }
    if (name === "recall") {
        var val = db_get_memory(input.key);
        if (val !== null)
            return '{"ok":true,"key":"' + input.key + '","value":"' + val.replace(/"/g, '\\"') + '"}';
        else
            return '{"ok":true,"key":"' + input.key + '","value":null}';
    }
    return '{"ok":false,"error":"unknown tool: ' + name + '"}';
}

// --- Claude API interaction ---

function call_claude(messages) {
    if (!CONFIG.messenger_ep) {
        return {ok: false, error: "messenger endpoint not configured"};
    }

    var api_key = "";
    // Try to read API key from environment via code-smith exec
    if (CONFIG.smith_ep) {
        var key_resp = bedrock.request(
            JSON.stringify({action: "exec", cmd: "echo $ANTHROPIC_API_KEY"}),
            CONFIG.smith_ep);
        if (key_resp) {
            try {
                var kr = JSON.parse(key_resp);
                if (kr.ok && kr.stdout) {
                    api_key = kr.stdout.replace(/\n/g, "").replace(/\r/g, "");
                }
            } catch (e) { /* ignore */ }
        }
    }

    if (!api_key) {
        return {ok: false, error: "ANTHROPIC_API_KEY not set"};
    }

    // Build tools list — only include tools with available endpoints
    var available_tools = [];
    for (var i = 0; i < TOOLS.length; i++) {
        var t = TOOLS[i];
        if (t.name === "remember" || t.name === "recall") {
            available_tools.push(t);
        } else if ((t.name === "compile_c") && CONFIG.cobbler_ep) {
            available_tools.push(t);
        } else if (CONFIG.smith_ep) {
            available_tools.push(t);
        }
    }

    var api_body = JSON.stringify({
        model: CONFIG.model,
        max_tokens: CONFIG.max_tokens,
        messages: messages,
        tools: available_tools
    });

    var fetch_req = JSON.stringify({
        action: "fetch",
        url: "https://api.anthropic.com/v1/messages",
        method: "POST",
        headers: [
            "content-type: application/json",
            "x-api-key: " + api_key,
            "anthropic-version: 2023-06-01"
        ],
        body: api_body
    });

    var raw = bedrock.request(fetch_req, CONFIG.messenger_ep);
    if (!raw) return {ok: false, error: "messenger unavailable"};

    try {
        var fetch_resp = JSON.parse(raw);
        if (!fetch_resp.ok) return {ok: false, error: fetch_resp.error || "fetch failed"};
        if (fetch_resp.status !== 200) {
            return {ok: false, error: "API returned status " + fetch_resp.status + ": " + fetch_resp.body};
        }
        return {ok: true, data: JSON.parse(fetch_resp.body)};
    } catch (e) {
        return {ok: false, error: "parse error: " + e.message};
    }
}

// --- Main ask loop with tool_use ---

function ask_claude(user_message) {
    // Build messages from history + new message
    var history = db_get_history(20);
    var messages = [];
    for (var i = 0; i < history.length; i++) {
        try {
            messages.push({role: history[i].role, content: JSON.parse(history[i].content)});
        } catch (e) {
            messages.push({role: history[i].role, content: history[i].content});
        }
    }
    messages.push({role: "user", content: user_message});

    // Save user message
    db_save_message("user", JSON.stringify(user_message));

    var max_turns = 10;
    for (var turn = 0; turn < max_turns; turn++) {
        var result = call_claude(messages);
        if (!result.ok) return result;

        var data = result.data;
        var stop_reason = data.stop_reason;

        // Save assistant response
        db_save_message("assistant", JSON.stringify(data.content));

        if (stop_reason === "end_turn" || stop_reason !== "tool_use") {
            // Extract text from content blocks
            var text_parts = [];
            for (var i = 0; i < data.content.length; i++) {
                if (data.content[i].type === "text") {
                    text_parts.push(data.content[i].text);
                }
            }
            return {ok: true, response: text_parts.join("\n")};
        }

        // Handle tool_use
        messages.push({role: "assistant", content: data.content});

        var tool_results = [];
        for (var i = 0; i < data.content.length; i++) {
            var block = data.content[i];
            if (block.type === "tool_use") {
                bedrock.log(NAME + " calling tool: " + block.name);
                var tool_resp = dispatch_tool(block.name, block.input);
                tool_results.push({
                    type: "tool_result",
                    tool_use_id: block.id,
                    content: tool_resp
                });
            }
        }

        messages.push({role: "user", content: tool_results});
        db_save_message("user", JSON.stringify(tool_results));
    }

    return {ok: false, error: "max tool turns exceeded"};
}

// --- Action handlers ---

function handle_say(req) {
    var from = req.from || "unknown";
    var message = req.message || "";
    if (!message) return {ok: false, error: "message required"};

    bedrock.log(NAME + " heard from " + from + ": " + message.substring(0, 80));
    return ask_claude(message);
}

function handle_status() {
    return {
        ok: true,
        name: NAME,
        model: CONFIG.model,
        messenger_ep: CONFIG.messenger_ep,
        smith_ep: CONFIG.smith_ep,
        cobbler_ep: CONFIG.cobbler_ep,
        conversation_length: db_count_messages(),
        memory: db_all_memory()
    };
}

function handle_memory(req) {
    if (req.key) {
        var val = db_get_memory(req.key);
        return {ok: true, key: req.key, value: val};
    }
    return {ok: true, memory: db_all_memory()};
}

function handle_forget() {
    db_clear_conversations();
    return {ok: true, message: "conversation history cleared, memory retained"};
}

// --- Main ---

bedrock.log(NAME + " waking up");

var msg_count = db_count_messages();
if (msg_count > 0) {
    bedrock.log(NAME + " resumed — " + msg_count + " messages in history");
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
            response = JSON.stringify(handle_say(req));
        } else if (req.action === "status") {
            response = JSON.stringify(handle_status());
        } else if (req.action === "memory") {
            response = JSON.stringify(handle_memory(req));
        } else if (req.action === "forget") {
            response = JSON.stringify(handle_forget());
        } else {
            response = JSON.stringify({ok: false, error: "unknown action: " + req.action});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "error: " + e.message});
    }

    bedrock.serve_send(response);
    count++;
}

bedrock.log(NAME + " going to sleep after " + count + " conversations");
