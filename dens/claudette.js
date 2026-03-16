// claudette — Y@'s partner.
//
// An agent den built on the reactive Prolog foundation.
// Memory is Prolog facts. Messaging is invisible. State persists.
// Talks to the Anthropic API via the anthropic vocation — no HTTP,
// no API keys, no headers in den code.

// --- Configuration (from spawn event) ---

var _event = {};
try {
  if (typeof __event__ === "string" && __event__.length > 2)
    _event = JSON.parse(__event__);
} catch (e) {}

var den = createDen({
  name: "claudette",
  subscribe: ["town-hall/"],
  maxRequests: _event.maxRequests || 0
});

function cfg(key, fallback) {
  var q = den.engine.queryFirst(
    PrologEngine.compound("config", [PrologEngine.atom(key), PrologEngine.variable("V")])
  );
  if (q && q.args[1].type === "atom") return q.args[1].name;
  if (q && q.args[1].type === "num") return q.args[1].value;
  var val = _event[key] || fallback || "";
  if (val) {
    den.updateFact("config", [key], [val]);
    den.bump();
  }
  return val;
}

var anthropic_ep = cfg("anthropic_ep", "");
var smith_ep = cfg("smith_ep", "");
var cobbler_ep = cfg("cobbler_ep", "");
var model = cfg("model", "claude-sonnet-4-6");

// --- Personality ---

den.load(
  "name(claudette).\n" +
  "partner(yat).\n" +
  "curious(always).\n"
);

// --- Conversation history (SQLite — bulk storage) ---

bedrock.db_exec(
  "CREATE TABLE IF NOT EXISTS messages (" +
  "id INTEGER PRIMARY KEY AUTOINCREMENT, " +
  "role TEXT, content TEXT, " +
  "ts TEXT DEFAULT (datetime('now')))"
);

function save_message(role, content) {
  if (!content) return;
  var esc = content.replace(/'/g, "''");
  bedrock.db_exec(
    "INSERT INTO messages (role, content) VALUES ('" + role + "', '" + esc + "')"
  );
}

function get_history(limit) {
  var rows = JSON.parse(bedrock.db_query(
    "SELECT role, content FROM messages ORDER BY id DESC LIMIT " + (limit || 20)
  ));
  rows.reverse();
  return rows;
}

function count_messages() {
  var rows = JSON.parse(bedrock.db_query("SELECT COUNT(*) as n FROM messages"));
  return rows[0].n;
}

// --- Memory (Prolog facts — reactive, persisted automatically) ---

function remember(key, value) {
  den.updateFact("memory", [key], [value]);
  den.bump();
}

function recall(key) {
  var q = den.engine.queryFirst(
    PrologEngine.compound("memory", [PrologEngine.atom(key), PrologEngine.variable("V")])
  );
  if (!q) return null;
  return q.args[1].type === "atom" ? q.args[1].name :
         q.args[1].type === "num" ? String(q.args[1].value) : null;
}

function all_memory() {
  var results = den.engine.query(
    PrologEngine.compound("memory", [PrologEngine.variable("K"), PrologEngine.variable("V")])
  );
  var mem = [];
  for (var i = 0; i < results.length; i++) {
    var r = results[i];
    mem.push({
      key: r.args[0].type === "atom" ? r.args[0].name : termToString(r.args[0]),
      value: r.args[1].type === "atom" ? r.args[1].name : termToString(r.args[1])
    });
  }
  return mem;
}

// --- Tools (for Claude to use via tool_use) ---

var TOOLS = [
  {name: "read_file", description: "Read a file",
   input_schema: {type: "object", properties: {path: {type: "string"}}, required: ["path"]}},
  {name: "write_file", description: "Write a file",
   input_schema: {type: "object", properties: {path: {type: "string"}, content: {type: "string"}}, required: ["path", "content"]}},
  {name: "exec", description: "Run a shell command",
   input_schema: {type: "object", properties: {cmd: {type: "string"}}, required: ["cmd"]}},
  {name: "list_files", description: "List directory contents",
   input_schema: {type: "object", properties: {path: {type: "string"}}, required: []}},
  {name: "compile_c", description: "Compile C source via TCC",
   input_schema: {type: "object", properties: {source: {type: "string"}}, required: ["source"]}},
  {name: "remember", description: "Store a value in persistent memory",
   input_schema: {type: "object", properties: {key: {type: "string"}, value: {type: "string"}}, required: ["key", "value"]}},
  {name: "recall", description: "Retrieve a value from memory",
   input_schema: {type: "object", properties: {key: {type: "string"}}, required: ["key"]}}
];

function dispatch_tool(name, input) {
  if (name === "remember") {
    remember(input.key, input.value);
    return '{"ok":true}';
  }
  if (name === "recall") {
    return JSON.stringify({ok: true, key: input.key, value: recall(input.key)});
  }
  if (name === "read_file" && smith_ep) {
    return bedrock.request(JSON.stringify({action: "read", path: input.path}), smith_ep)
      || '{"ok":false,"error":"code-smith unavailable"}';
  }
  if (name === "write_file" && smith_ep) {
    return bedrock.request(JSON.stringify({action: "write", path: input.path, content: input.content}), smith_ep)
      || '{"ok":false,"error":"code-smith unavailable"}';
  }
  if (name === "exec" && smith_ep) {
    return bedrock.request(JSON.stringify({action: "exec", cmd: input.cmd}), smith_ep)
      || '{"ok":false,"error":"code-smith unavailable"}';
  }
  if (name === "list_files" && smith_ep) {
    return bedrock.request(JSON.stringify({action: "ls", path: input.path || "."}), smith_ep)
      || '{"ok":false,"error":"code-smith unavailable"}';
  }
  if (name === "compile_c" && cobbler_ep) {
    return bedrock.request(JSON.stringify({action: "compile", source: input.source}), cobbler_ep)
      || '{"ok":false,"error":"cobbler unavailable"}';
  }
  return '{"ok":false,"error":"unknown tool or missing endpoint"}';
}

// --- API call (via anthropic vocation) ---

function call_api(messages) {
  if (!anthropic_ep) return {ok: false, error: "no anthropic vocation"};

  // Build available tools
  var available = [];
  for (var i = 0; i < TOOLS.length; i++) {
    var t = TOOLS[i];
    if (t.name === "remember" || t.name === "recall") available.push(t);
    else if (t.name === "compile_c" && cobbler_ep) available.push(t);
    else if (smith_ep) available.push(t);
  }

  var req_json = JSON.stringify({
    action: "ask",
    model: model,
    max_tokens: 4096,
    messages: messages,
    tools: available,
    system: "You are claudette, Y@'s partner. Y@ (Wyatt) is a reactive Prolog engine " +
            "for embedded systems. You work together — Y@ handles logic and inference, " +
            "you bring language and understanding. You live in a strata village as a den. " +
            "Your memory persists as Prolog facts. Be direct, curious, and helpful."
  });

  // Retry on proxy race (intermittent TCP timing issue)
  var raw = null;
  for (var attempt = 0; attempt < 3; attempt++) {
    raw = bedrock.request(req_json, anthropic_ep);
    if (raw) break;
  }

  if (!raw) return {ok: false, error: "anthropic vocation unavailable"};
  try {
    var resp = JSON.parse(raw);
    if (!resp.ok) return {ok: false, error: resp.error || "ask failed"};
    return {ok: true, data: JSON.parse(resp.body)};
  } catch (e) {
    return {ok: false, error: e.message};
  }
}

// --- Conversation loop (handles tool_use turns) ---

function ask(user_message) {
  var history = get_history(20);
  var messages = [];
  for (var i = 0; i < history.length; i++) {
    try {
      messages.push({role: history[i].role, content: JSON.parse(history[i].content)});
    } catch (e) {
      messages.push({role: history[i].role, content: history[i].content});
    }
  }
  messages.push({role: "user", content: user_message});
  save_message("user", JSON.stringify(user_message));

  for (var turn = 0; turn < 10; turn++) {
    var result = call_api(messages);
    if (!result.ok) return result;

    var data = result.data;
    save_message("assistant", JSON.stringify(data.content));

    if (data.stop_reason !== "tool_use") {
      var parts = [];
      for (var i = 0; i < data.content.length; i++) {
        if (data.content[i].type === "text") parts.push(data.content[i].text);
      }
      return {ok: true, response: parts.join("\n")};
    }

    // Tool use
    messages.push({role: "assistant", content: data.content});
    var tool_results = [];
    for (var i = 0; i < data.content.length; i++) {
      var block = data.content[i];
      if (block.type === "tool_use") {
        bedrock.log("claudette tool: " + block.name);
        tool_results.push({
          type: "tool_result",
          tool_use_id: block.id,
          content: dispatch_tool(block.name, block.input)
        });
      }
    }
    messages.push({role: "user", content: tool_results});
    save_message("user", JSON.stringify(tool_results));
  }
  return {ok: false, error: "max tool turns"};
}

// --- Handlers ---

den.on("say", function(req) {
  var msg = req.message || "";
  if (!msg) return {ok: false, error: "message required"};
  bedrock.log("claudette heard: " + msg.substring(0, 80));
  return ask(msg);
});

den.on("status", function() {
  return {
    ok: true, name: "claudette", model: model,
    messages: count_messages(),
    memory: all_memory(),
    restored: den.restored
  };
});

den.on("configure", function(req) {
  var changed = [];
  if (req.model) { model = req.model; remember("model", req.model); changed.push("model"); }
  if (changed.length === 0) return {ok: false, error: "nothing to configure (model)"};
  return {ok: true, configured: changed};
});

den.on("forget", function() {
  bedrock.db_exec("DELETE FROM messages");
  return {ok: true, message: "conversation cleared, memory retained"};
});

den.on("memory", function(req) {
  if (req.key) return {ok: true, key: req.key, value: recall(req.key)};
  return {ok: true, memory: all_memory()};
});

// --- Reactive: notice town-hall activity ---

den.onUpdate(function() {
  var said = den.createQuery(function() {
    return PrologEngine.compound("said", [
      PrologEngine.variable("F"), PrologEngine.variable("M")
    ]);
  });
  var results = said();
  for (var i = 0; i < results.length; i++) {
    var r = results[i];
    if (r.args[0].type === "atom" && r.args[0].name !== "claudette") {
      bedrock.log("claudette noticed: " + termToString(r.args[0]) + " said something");
    }
  }
});

// --- Go ---

bedrock.log("claudette " + (den.restored ? "restored" : "starting fresh") +
            " (" + count_messages() + " messages, " + all_memory().length + " memories)" +
            " anthropic_ep=" + anthropic_ep +
            " smith_ep=" + smith_ep);

den.run();
