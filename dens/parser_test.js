// parser_test.js — Test that parser.js + loadString work in den context.
// Exits 0 on success, 1 on failure.

var passed = 0;
var failed = 0;

function assert_eq(a, b, msg) {
  if (a === b) {
    passed++;
  } else {
    bedrock.log("FAIL: " + msg + " — expected " + JSON.stringify(b) + " got " + JSON.stringify(a));
    failed++;
  }
}

function assert_true(v, msg) {
  if (v) { passed++; } else { bedrock.log("FAIL: " + msg); failed++; }
}

// --- Test parseTerm ---

var t1 = parseTerm("hello");
assert_eq(t1.type, "atom", "parseTerm atom type");
assert_eq(t1.name, "hello", "parseTerm atom name");

var t2 = parseTerm("X");
assert_eq(t2.type, "var", "parseTerm var type");
assert_eq(t2.name, "X", "parseTerm var name");

var t3 = parseTerm("42");
assert_eq(t3.type, "num", "parseTerm num type");
assert_eq(t3.value, 42, "parseTerm num value");

var t4 = parseTerm("parent(tom, bob)");
assert_eq(t4.type, "compound", "parseTerm compound type");
assert_eq(t4.functor, "parent", "parseTerm compound functor");
assert_eq(t4.args.length, 2, "parseTerm compound arity");
assert_eq(t4.args[0].name, "tom", "parseTerm compound arg0");
assert_eq(t4.args[1].name, "bob", "parseTerm compound arg1");

// --- Test parseClause ---

var c1 = parseClause("grandparent(X, Z) :- parent(X, Y), parent(Y, Z).");
assert_eq(c1.head.functor, "grandparent", "parseClause head functor");
assert_eq(c1.body.length, 2, "parseClause body length");
assert_eq(c1.body[0].functor, "parent", "parseClause body[0] functor");
assert_eq(c1.body[1].functor, "parent", "parseClause body[1] functor");

// --- Test parseProgram ---

var prog = parseProgram(
  "parent(tom, bob).\n" +
  "parent(bob, ann).\n" +
  "grandparent(X, Z) :- parent(X, Y), parent(Y, Z).\n"
);
assert_eq(prog.length, 3, "parseProgram clause count");
assert_eq(prog[0].head.functor, "parent", "parseProgram clause 0");
assert_eq(prog[0].body.length, 0, "parseProgram fact has no body");
assert_eq(prog[2].head.functor, "grandparent", "parseProgram clause 2");
assert_eq(prog[2].body.length, 2, "parseProgram rule has body");

// --- Test loadString + query ---

var engine = new PrologEngine();
var loaded = loadString(engine,
  "parent(tom, bob).\n" +
  "parent(bob, ann).\n" +
  "grandparent(X, Z) :- parent(X, Y), parent(Y, Z).\n"
);
assert_eq(loaded, 3, "loadString returns clause count");

var results = engine.query(
  PrologEngine.compound("grandparent", [
    PrologEngine.variable("X"),
    PrologEngine.variable("Z")
  ])
);
assert_eq(results.length, 1, "grandparent query result count");
assert_eq(results[0].args[0].name, "tom", "grandparent X = tom");
assert_eq(results[0].args[1].name, "ann", "grandparent Z = ann");

// --- Test loadString with reactive engine ---

var den = createDenEngine(new PrologEngine());
loadString(den.engine,
  "sensor(temp, 22).\n" +
  "hot :- sensor(temp, T), T > 30.\n"
);
var hotQuery = den.createQuery(function() {
  return PrologEngine.atom("hot");
});
assert_eq(hotQuery().length, 0, "not hot at 22");

// Update temp to 35
den.updateFact("sensor", ["temp"], [35]);
den.bump();
assert_eq(hotQuery().length, 1, "hot at 35");

// --- Report ---

bedrock.log("parser_test: " + passed + " passed, " + failed + " failed");
if (failed > 0) {
  throw new Error("parser_test: " + failed + " tests failed");
}
