// ============================================================
// den-engine.js — Automatic SUB event → Prolog fact wiring
//
// Wraps createReactiveEngine with bedrock.receive() integration.
// Convention: topic "type/name" → Prolog fact type(name, value)
// where value is either a number or an atom parsed from the payload.
//
// Portable: same var-only style as reactive.js / prolog-engine.js
// ============================================================

function createDenEngine(engineOrFactory) {
  var rp = createReactiveEngine(engineOrFactory);
  var atom = PrologEngine.atom;
  var num = PrologEngine.num;
  var variable = PrologEngine.variable;
  var compound = PrologEngine.compound;

  // Track asserted event facts for retract-before-assert pattern
  // Key: "functor/keyArg" → last asserted clause head
  var _facts = {};

  function _parseValue(s) {
    var n = Number(s);
    if (s !== "" && !isNaN(n) && isFinite(n)) return num(n);
    return atom(s);
  }

  function _parsePayload(payload) {
    try { return JSON.parse(payload); } catch (e) { return payload; }
  }

  // updateFact("sensor", ["temp"], [22.5])
  // → retract old sensor(temp, _), assert sensor(temp, 22.5), bump
  function updateFact(functor, keyArgs, valueArgs) {
    var allArgs = [];
    var retractArgs = [];
    var i;
    for (i = 0; i < keyArgs.length; i++) {
      var ka = typeof keyArgs[i] === "string" ? atom(keyArgs[i]) : keyArgs[i];
      allArgs.push(ka);
      retractArgs.push(ka);
    }
    for (i = 0; i < valueArgs.length; i++) {
      var va = valueArgs[i];
      if (typeof va === "number") va = num(va);
      else if (typeof va === "string") va = _parseValue(va);
      allArgs.push(va);
      retractArgs.push(variable("_V" + i));
    }

    var retractHead = compound(functor, retractArgs);
    rp.engine.retractFirst(retractHead);

    var assertHead = compound(functor, allArgs);
    rp.engine.addClause(assertHead);
  }

  // processEvent({topic: "sensor/temp", payload: "22.5"})
  // Convention: topic "type/name" → updateFact("type", ["name"], [payload])
  // Topic without "/" → updateFact(topic, [], [payload])
  function processEvent(evt) {
    var topic = evt.topic;
    var payload = evt.payload;
    var slash = topic.indexOf("/");
    var functor, keyName;

    if (slash >= 0) {
      functor = topic.substring(0, slash);
      keyName = topic.substring(slash + 1);
    } else {
      functor = topic;
      keyName = null;
    }

    var parsed = _parsePayload(payload);

    if (typeof parsed === "object" && parsed !== null && !Array.isArray(parsed)) {
      // JSON object: each key becomes a value arg
      // e.g. topic "sensor/temp", payload '{"value":22.5,"unit":"C"}'
      // → sensor(temp, 22.5, "C")
      var keys = keyName ? [keyName] : [];
      var vals = [];
      for (var k in parsed) {
        if (parsed.hasOwnProperty(k)) {
          vals.push(parsed[k]);
        }
      }
      if (vals.length === 0) vals.push(payload);
      updateFact(functor, keys, vals);
    } else {
      // Scalar or array
      var keyArgs = keyName ? [keyName] : [];
      var valueArgs = Array.isArray(parsed) ? parsed : [parsed];
      updateFact(functor, keyArgs, valueArgs);
    }
  }

  // poll() — drain SUB queue, map each event to Prolog facts, bump once
  function poll() {
    var count = 0;
    while (true) {
      var evt = bedrock.receive();
      if (evt === null || evt === undefined) break;
      processEvent(evt);
      count++;
    }
    if (count > 0) rp.bump();
    return count;
  }

  return {
    engine: rp.engine,
    reactive: rp,
    generation: rp.generation,
    bump: rp.bump,
    act: rp.act,
    createQuery: rp.createQuery,
    createQueryFirst: rp.createQueryFirst,
    onUpdate: rp.onUpdate,
    updateFact: updateFact,
    processEvent: processEvent,
    poll: poll
  };
}

if (typeof exports !== "undefined") {
  exports.createDenEngine = createDenEngine;
}
