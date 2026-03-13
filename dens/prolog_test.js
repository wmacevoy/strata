/* prolog_test.js — verifies PrologEngine is available in den JS runtime */
var atom = PrologEngine.atom;
var variable = PrologEngine.variable;
var compound = PrologEngine.compound;
var num = PrologEngine.num;

var e = new PrologEngine();

/* Facts */
e.addClause(compound("parent", [atom("tom"), atom("bob")]));
e.addClause(compound("parent", [atom("tom"), atom("liz")]));
e.addClause(compound("parent", [atom("bob"), atom("ann")]));

/* Rule: grandparent(X, Z) :- parent(X, Y), parent(Y, Z). */
e.addClause(
    compound("grandparent", [variable("X"), variable("Z")]),
    [compound("parent", [variable("X"), variable("Y")]),
     compound("parent", [variable("Y"), variable("Z")])]
);

/* Query */
var results = e.query(compound("grandparent", [atom("tom"), variable("W")]));

if (results.length !== 1) {
    bedrock.log("FAIL: expected 1 grandchild, got " + results.length);
    throw new Error("test failed");
}
var gc = results[0].args[1];
if (gc.name !== "ann") {
    bedrock.log("FAIL: expected ann, got " + gc.name);
    throw new Error("test failed");
}

/* Reactive test */
var pair = createSignal(0);
var read = pair[0];
var write = pair[1];
var doubled = createMemo(function() { return read() * 2; });

if (doubled() !== 0) throw new Error("memo init failed");
write(5);
if (doubled() !== 10) throw new Error("memo update failed");

/* Reactive Prolog */
var rp = createReactiveEngine(e);
rp.engine.addClause(compound("credit", [num(0)]));

var creditQuery = rp.createQueryFirst(function() {
    return compound("credit", [variable("X")]);
});
var cr = creditQuery();
if (!cr || cr.args[0].value !== 0) throw new Error("reactive query init failed: " + JSON.stringify(cr));

rp.engine.retractFirst(compound("credit", [variable("_")]));
rp.engine.addClause(compound("credit", [num(42)]));
rp.bump();

cr = creditQuery();
if (!cr || cr.args[0].value !== 42) throw new Error("reactive query update failed: " + JSON.stringify(cr));

bedrock.log("PROLOG_TEST_PASS");
