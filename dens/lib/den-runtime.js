// ============================================================
// den-runtime.js — Reactive den runtime
//
// A den is a reactive Prolog state. Messages are invisible
// infrastructure: facts flow in (accept), signals flow out.
// The serve loop, JSON plumbing, and persistence are handled here.
//
// State = the Prolog clause database. Program clauses (from load())
// are code — always fresh from source. Runtime facts (from assert,
// updateFact, or Prolog assert/1) are state — persisted automatically
// to per-den SQLite and restored on next startup.
//
// Usage:
//   var den = createDen({name: "gee"});
//   den.load("is_curious(gee).\n");
//   den.on("say", function(req) { ... });
//   den.run();
//
// Portable: same var-only style as reactive.js / prolog-engine.js
// ============================================================

function createDen(opts) {
  var name = (opts && opts.name) || "den";
  var maxRequests = (opts && opts.maxRequests) || 0;
  var subscriptions = (opts && opts.subscribe) || [];

  var de = createDenEngine(new PrologEngine());
  var _handlers = {};
  var _programSize = 0;  // clauses from load() — not persisted
  var _restored = false;

  // --- SQL escaping for state persistence ---

  function _sqlEscape(s) {
    return s.replace(/'/g, "''");
  }

  // --- State persistence ---

  function _initStateTable() {
    bedrock.db_exec(
      "CREATE TABLE IF NOT EXISTS _prolog_state (" +
      "id INTEGER PRIMARY KEY, clauses TEXT, " +
      "updated_at TEXT DEFAULT (datetime('now')))"
    );
  }

  function _saveState() {
    var runtimeClauses = de.engine.clauses.slice(_programSize);
    if (runtimeClauses.length === 0) {
      bedrock.db_exec("DELETE FROM _prolog_state WHERE id = 1");
      return 0;
    }
    var json = JSON.stringify(runtimeClauses);
    bedrock.db_exec(
      "INSERT OR REPLACE INTO _prolog_state (id, clauses) VALUES (1, '" +
      _sqlEscape(json) + "')"
    );
    return runtimeClauses.length;
  }

  function _restoreState() {
    var raw = bedrock.db_query(
      "SELECT clauses FROM _prolog_state WHERE id = 1"
    );
    var rows = JSON.parse(raw);
    if (rows.length === 0) return 0;

    var clauses = JSON.parse(rows[0].clauses);
    for (var i = 0; i < clauses.length; i++) {
      de.engine.addClause(clauses[i].head, clauses[i].body);
    }
    return clauses.length;
  }

  // --- load: Prolog text → engine clauses (program, not state) ---

  function load(text) {
    var n = loadString(de.engine, text);
    _programSize = de.engine.clauses.length;
    return n;
  }

  // --- on: register JS handler for an action ---

  function on(action, fn) {
    _handlers[action] = fn;
  }

  // --- Prolog-based handler via handle/1 + send/2 ---

  function _prologHandle(action, req) {
    if (req.from !== undefined) {
      de.updateFact("from", [], [req.from]);
    }
    if (req.message !== undefined) {
      de.updateFact("message", [], [req.message]);
    }
    de.updateFact("request", [], [action]);
    de.bump();

    var result = de.engine.queryWithSends(
      PrologEngine.compound("handle", [PrologEngine.atom(action)])
    );

    var response = null;
    for (var i = 0; i < result.sends.length; i++) {
      var s = result.sends[i];
      var targetName = s.target.type === "atom" ? s.target.name : termToString(s.target);

      if (targetName === "respond" && response === null) {
        response = _termToResponse(s.fact);
      } else if (targetName === "publish") {
        _publishTerm(s.fact);
      } else if (targetName === "log") {
        bedrock.log(name + ": " + termToString(s.fact));
      }
    }

    de.engine.retractFirst(PrologEngine.compound("request", [PrologEngine.variable("_")]));
    de.engine.retractFirst(PrologEngine.compound("from", [PrologEngine.variable("_")]));
    de.engine.retractFirst(PrologEngine.compound("message", [PrologEngine.variable("_")]));

    return response;
  }

  function _termToResponse(term) {
    if (term.type === "atom") {
      return {ok: true, from: name, response: term.name};
    }
    if (term.type === "num") {
      return {ok: true, from: name, response: term.value};
    }
    if (term.type === "compound") {
      var obj = {ok: true, from: name};
      if (term.functor === "error") {
        obj.ok = false;
        obj.error = term.args.length > 0 ? termToString(term.args[0]) : "error";
      } else {
        obj.response = termToString(term);
      }
      return obj;
    }
    return {ok: true, from: name, response: termToString(term)};
  }

  function _publishTerm(term) {
    if (term.type === "compound" && term.args.length >= 2) {
      bedrock.publish(termToString(term.args[0]), termToString(term.args[1]));
    } else if (term.type === "compound" && term.args.length === 1) {
      bedrock.publish(termToString(term.args[0]), "");
    } else {
      bedrock.publish(name, termToString(term));
    }
  }

  // --- init: restore state from per-den SQLite ---

  function _init() {
    _initStateTable();
    var n = _restoreState();
    if (n > 0) {
      _restored = true;
      bedrock.log(name + " restored " + n + " facts");
    }
  }

  // Initialize immediately
  _init();

  // --- run: the invisible loop ---

  function run() {
    for (var i = 0; i < subscriptions.length; i++) {
      bedrock.subscribe(subscriptions[i]);
    }

    bedrock.log(name + " running (" + de.engine.clauses.length + " clauses, " +
                (_restored ? "restored" : "fresh") + ")");

    var count = 0;
    var nulls = 0;
    while (maxRequests === 0 || count < maxRequests) {
      de.poll();

      var raw = bedrock.serve_recv();
      if (raw === null) {
        nulls++;
        // In bounded mode (maxRequests > 0), exit if no traffic
        if (maxRequests > 0 && nulls > 3) break;
        continue;
      }
      nulls = 0;

      var response;
      try {
        var req = JSON.parse(raw);
        var action = req.action || "unknown";

        if (_handlers[action]) {
          response = _handlers[action](req);
        } else {
          response = _prologHandle(action, req);
          if (response === null) {
            response = {ok: false, error: "no handler for: " + action};
          }
        }
      } catch (e) {
        response = {ok: false, error: e.message};
      }

      bedrock.serve_send(JSON.stringify(response));
      count++;
    }

    // Persist state before exit
    var saved = _saveState();
    bedrock.log(name + " stopped (" + count + " requests, " + saved + " facts saved)");
  }

  return {
    engine: de.engine,
    reactive: de,
    restored: _restored,
    load: load,
    on: on,
    save: _saveState,
    updateFact: de.updateFact,
    bump: de.bump,
    poll: de.poll,
    createQuery: de.createQuery,
    createQueryFirst: de.createQueryFirst,
    onUpdate: de.onUpdate,
    run: run
  };
}

if (typeof exports !== "undefined") {
  exports.createDen = createDen;
}
