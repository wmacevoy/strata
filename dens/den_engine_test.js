/* den_engine_test.js — verifies createDenEngine auto-wiring */
var atom = PrologEngine.atom;
var variable = PrologEngine.variable;
var compound = PrologEngine.compound;
var num = PrologEngine.num;

var e = new PrologEngine();

/* Rule: hot(X) :- sensor(temp, X), X > 30. */
e.addClause(
    compound("hot", [variable("X")]),
    [compound("sensor", [atom("temp"), variable("X")]),
     compound(">", [variable("X"), num(30)])]
);

/* Create den engine */
var den = createDenEngine(e);

/* Manually wire an event (simulates what poll() would do) */
den.processEvent({topic: "sensor/temp", payload: "22.5"});
den.bump();

/* Query: sensor(temp, X) should exist */
var tempQ = den.createQueryFirst(function() {
    return compound("sensor", [atom("temp"), variable("X")]);
});

var result = tempQ();
if (!result) throw new Error("sensor fact not asserted");
if (result.args[1].value !== 22.5) throw new Error("expected 22.5, got " + JSON.stringify(result.args[1]));

/* hot(X) should NOT match (22.5 <= 30) */
var hotQ = den.createQueryFirst(function() {
    return compound("hot", [variable("X")]);
});
if (hotQ() !== null) throw new Error("should not be hot at 22.5");

/* Update temp to 35 */
den.processEvent({topic: "sensor/temp", payload: "35"});
den.bump();

/* Now sensor(temp, X) should give 35 (old 22.5 retracted) */
result = tempQ();
if (!result || result.args[1].value !== 35) throw new Error("expected 35 after update");

/* hot(X) should now match */
result = hotQ();
if (!result) throw new Error("should be hot at 35");
if (result.args[0].value !== 35) throw new Error("hot value should be 35");

/* Test updateFact directly */
den.updateFact("battery", ["level"], [87]);
den.bump();

var battQ = den.createQueryFirst(function() {
    return compound("battery", [atom("level"), variable("V")]);
});
result = battQ();
if (!result || result.args[1].value !== 87) throw new Error("battery should be 87");

/* Update battery */
den.updateFact("battery", ["level"], [42]);
den.bump();
result = battQ();
if (!result || result.args[1].value !== 42) throw new Error("battery should be 42 after update");

/* Test topic without slash */
den.processEvent({topic: "heartbeat", payload: "1"});
den.bump();

var hbQ = den.createQueryFirst(function() {
    return compound("heartbeat", [variable("V")]);
});
result = hbQ();
if (!result || result.args[0].value !== 1) throw new Error("heartbeat should be 1");

bedrock.log("DEN_ENGINE_TEST_PASS");
