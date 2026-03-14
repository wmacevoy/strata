// anthropic — vocation den for Claude API access.
//
// Thin credential proxy: owns the API key, forwards to messenger.
// Dens send "ask", anthropic adds auth headers, messenger does HTTP.
// The key never leaves this den.
//
// Privileged: needs peer access to messenger vocation.
//
// Actions: ask, models, discover, say

var _event = {};
try {
  if (typeof __event__ === "string" && __event__.length > 2)
    _event = JSON.parse(__event__);
} catch (e) {}

var den = createDen({
  name: "anthropic",
  maxRequests: _event.maxRequests || 0
});

// --- Configuration ---

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

var api_key = cfg("api_key", "");
var default_model = cfg("model", "claude-sonnet-4-6");

// --- API call via bedrock.fetch (direct curl, privileged) ---

function call_api(req) {
  if (!api_key) return {ok: false, error: "no API key"};
  if (typeof bedrock.fetch !== "function") return {ok: false, error: "not privileged — no fetch"};

  var model = req.model || default_model;
  var max_tokens = req.max_tokens || 4096;

  var payload = {
    model: model,
    max_tokens: max_tokens,
    messages: req.messages
  };
  if (req.system) payload.system = req.system;
  if (req.tools) payload.tools = req.tools;

  var raw = bedrock.fetch(JSON.stringify({
    url: "https://api.anthropic.com/v1/messages",
    method: "POST",
    headers: [
      "content-type: application/json",
      "x-api-key: " + api_key,
      "anthropic-version: 2023-06-01"
    ],
    body: JSON.stringify(payload)
  }));

  if (!raw) return {ok: false, error: "fetch failed"};
  try {
    var resp = JSON.parse(raw);
    if (!resp.ok) return {ok: false, error: resp.error || "fetch failed"};
    return {ok: true, status: resp.status, body: resp.body};
  } catch (e) {
    return {ok: false, error: e.message};
  }
}

// --- Handlers ---

den.on("ask", function(req) {
  return call_api(req);
});

den.on("models", function() {
  return {
    ok: true,
    models: ["claude-opus-4-6", "claude-sonnet-4-6", "claude-haiku-4-5-20251001"],
    default: default_model
  };
});

den.on("discover", function() {
  return {
    ok: true, name: "anthropic",
    has_key: api_key ? true : false,
    default_model: default_model,
    actions: {
      ask: {params: {messages: "array", model: "string (optional)", system: "string (optional)", max_tokens: "number (optional)", tools: "array (optional)"}},
      models: {params: {}},
      discover: {params: {}}
    }
  };
});

den.on("status", function() {
  return {ok: true, name: "anthropic", has_key: api_key ? true : false, model: default_model};
});

// --- Go ---

bedrock.log("anthropic vocation " + (den.restored ? "restored" : "starting") +
            " key=" + (api_key ? "yes" : "NO") +
            " model=" + default_model +
            " fetch=" + (typeof bedrock.fetch === "function" ? "yes" : "no"));

den.run();
