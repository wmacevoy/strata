// runtime_test.js — Test createDen() runtime.
// Since we don't have REP/SUB sockets in test mode,
// we test the engine, load, handler dispatch, Prolog handle/1,
// and state persistence directly.

var passed = 0;
var failed = 0;

function assert_eq(a, b, msg) {
  if (a === b) { passed++; }
  else { bedrock.log("FAIL: " + msg + " — expected " + JSON.stringify(b) + " got " + JSON.stringify(a)); failed++; }
}

function assert_true(v, msg) {
  if (v) { passed++; } else { bedrock.log("FAIL: " + msg); failed++; }
}

// --- Test createDen exists ---

assert_eq(typeof createDen, "function", "createDen is a function");

// --- Test basic creation and load ---

var den = createDen({name: "test", maxRequests: 1});
assert_true(den.engine !== undefined, "den has engine");
assert_true(den.load !== undefined, "den has load");
assert_true(den.on !== undefined, "den has on");
assert_true(den.run !== undefined, "den has run");
assert_true(den.save !== undefined, "den has save");

var loaded = den.load(
  "parent(tom, bob).\n" +
  "parent(bob, ann).\n" +
  "grandparent(X, Z) :- parent(X, Y), parent(Y, Z).\n"
);
assert_eq(loaded, 3, "load returns clause count");

// Verify engine has the clauses
var results = den.engine.query(
  PrologEngine.compound("grandparent", [
    PrologEngine.variable("X"),
    PrologEngine.variable("Z")
  ])
);
assert_eq(results.length, 1, "grandparent query works");
assert_eq(results[0].args[0].name, "tom", "grandparent X = tom");
assert_eq(results[0].args[1].name, "ann", "grandparent Z = ann");

// --- Test JS handler registration ---

var den2 = createDen({name: "test2", maxRequests: 1});
var handlerCalled = false;
den2.on("ping", function(req) {
  handlerCalled = true;
  return {ok: true, pong: req.value || "default"};
});
assert_true(!handlerCalled, "handler not called until run");

// --- Test Prolog handle/1 with send/2 ---

var den3 = createDen({name: "test3"});
den3.load(
  "handle(greet) :- from(F), message(M), send(respond, hello(F, M)).\n" +
  "handle(status) :- send(respond, ok).\n"
);

// Simulate what _prologHandle does
den3.updateFact("from", [], ["alice"]);
den3.updateFact("message", [], ["hi"]);
den3.updateFact("request", [], ["greet"]);
den3.bump();

var result = den3.engine.queryWithSends(
  PrologEngine.compound("handle", [PrologEngine.atom("greet")])
);
assert_true(result.result !== null, "handle(greet) succeeds");
assert_eq(result.sends.length, 1, "one send from handle(greet)");
assert_eq(result.sends[0].target.name, "respond", "send target is respond");
assert_eq(result.sends[0].fact.functor, "hello", "send fact functor is hello");
assert_eq(result.sends[0].fact.args[0].name, "alice", "send fact arg0 is alice");
assert_eq(result.sends[0].fact.args[1].name, "hi", "send fact arg1 is hi");

// Clean up ephemeral facts
den3.engine.retractFirst(PrologEngine.compound("from", [PrologEngine.variable("_")]));
den3.engine.retractFirst(PrologEngine.compound("message", [PrologEngine.variable("_")]));
den3.engine.retractFirst(PrologEngine.compound("request", [PrologEngine.variable("_")]));

// Test handle(status)
var result2 = den3.engine.queryWithSends(
  PrologEngine.compound("handle", [PrologEngine.atom("status")])
);
assert_true(result2.result !== null, "handle(status) succeeds");
assert_eq(result2.sends.length, 1, "one send from handle(status)");
assert_eq(result2.sends[0].fact.name, "ok", "status responds ok");

// --- Test reactive hooks with createDen ---

var den4 = createDen({name: "test4"});
den4.load("sensor(temp, 20).\nhot :- sensor(temp, T), T > 30.\n");

var hotQuery = den4.createQuery(function() {
  return PrologEngine.atom("hot");
});
assert_eq(hotQuery().length, 0, "not hot at 20");

den4.updateFact("sensor", ["temp"], [35]);
den4.bump();
assert_eq(hotQuery().length, 1, "hot at 35");

// --- Test onUpdate fires ---

var den5 = createDen({name: "test5"});
den5.load("sensor(light, 100).\n");
var updateCount = 0;
den5.onUpdate(function() {
  updateCount++;
});
assert_true(updateCount >= 1, "onUpdate fired on registration");

var before = updateCount;
den5.updateFact("sensor", ["light"], [200]);
den5.bump();
assert_true(updateCount > before, "onUpdate fired after bump");

// --- Test state persistence ---

// den6: load program, add runtime facts, save, verify save happened
var den6 = createDen({name: "persist_test"});
den6.load(
  "color(red).\n" +
  "color(blue).\n"
);
assert_eq(den6.restored, false, "fresh den is not restored");

// Program has 2 clauses. Add runtime facts.
den6.updateFact("mood", ["gee"], ["curious"]);
den6.engine.addClause(
  PrologEngine.compound("seen", [PrologEngine.atom("alice")]),
  []
);
// Now: 2 program + 2 runtime = 4 total clauses

// Save state
var savedCount = den6.save();
assert_eq(savedCount, 2, "save persists 2 runtime facts (not program)");

// Verify saved data exists in local db
var rows = JSON.parse(bedrock.db_query(
  "SELECT clauses FROM _prolog_state WHERE id = 1"
));
assert_eq(rows.length, 1, "state row exists");

var savedClauses = JSON.parse(rows[0].clauses);
assert_eq(savedClauses.length, 2, "2 clauses in saved state");

// Verify the saved clauses are the runtime ones, not program
// First runtime fact: mood(gee, curious) — from updateFact
// Second runtime fact: seen(alice) — from addClause
var found_mood = false;
var found_seen = false;
for (var i = 0; i < savedClauses.length; i++) {
  var h = savedClauses[i].head;
  if (h.functor === "mood") found_mood = true;
  if (h.functor === "seen") found_seen = true;
}
assert_true(found_mood, "mood fact was saved");
assert_true(found_seen, "seen fact was saved");

// --- Test state restore ---

// den7: same program, should restore the saved runtime facts
var den7 = createDen({name: "restore_test"});
den7.load(
  "color(red).\n" +
  "color(blue).\n"
);
assert_eq(den7.restored, true, "den7 restored from saved state");

// Should have program (2) + restored runtime (2) = 4 clauses
assert_eq(den7.engine.clauses.length, 4, "4 total clauses after restore");

// Verify runtime facts are queryable
var moods = den7.engine.query(
  PrologEngine.compound("mood", [
    PrologEngine.variable("Who"),
    PrologEngine.variable("What")
  ])
);
assert_eq(moods.length, 1, "mood fact restored");
assert_eq(moods[0].args[0].name, "gee", "mood who = gee");
assert_eq(moods[0].args[1].name, "curious", "mood what = curious");

var seens = den7.engine.query(
  PrologEngine.compound("seen", [PrologEngine.variable("X")])
);
assert_eq(seens.length, 1, "seen fact restored");
assert_eq(seens[0].args[0].name, "alice", "seen = alice");

// Verify program clauses also work
var colors = den7.engine.query(
  PrologEngine.compound("color", [PrologEngine.variable("C")])
);
assert_eq(colors.length, 2, "program color facts intact");

// --- Test save with no runtime facts ---

// Clear the state table for den8
bedrock.db_exec("DELETE FROM _prolog_state");

var den8 = createDen({name: "empty_test"});
den8.load("fact(a).\n");
assert_eq(den8.restored, false, "empty den is not restored");

var saved8 = den8.save();
assert_eq(saved8, 0, "no runtime facts to save");

// State row should be deleted
var rows8 = JSON.parse(bedrock.db_query(
  "SELECT clauses FROM _prolog_state WHERE id = 1"
));
assert_eq(rows8.length, 0, "no state row when no runtime facts");

// --- Report ---

bedrock.log("runtime_test: " + passed + " passed, " + failed + " failed");
if (failed > 0) {
  throw new Error("runtime_test: " + failed + " tests failed");
}
