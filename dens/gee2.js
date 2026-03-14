// gee2 — gee rewritten as a reactive Prolog den.
//
// Compare with gee.js (175 lines of plumbing).
// This is the same personality, ~30 lines.

var den = createDen({
  name: "gee",
  subscribe: ["town-hall/"]
});

// --- Personality as Prolog rules ---

den.load(
  "is_curious(gee).\n" +
  "greeting(hello).\n" +
  "greeting(hi).\n" +
  "greeting(hey).\n"
);

// --- Handlers ---

den.on("say", function(req) {
  var from = req.from || "someone";
  var msg = req.message || "";

  // Record as fact
  den.updateFact("said", [from], [msg]);
  den.bump();

  var wonder = "I wonder... " + msg + " — but why?";

  // Publish to town hall
  bedrock.publish("town-hall/thought", JSON.stringify({
    from: "gee", thought: wonder
  }));

  return {ok: true, from: "gee", response: wonder};
});

den.on("status", function() {
  return {ok: true, name: "gee", clauses: den.engine.clauses.length};
});

den.run();
