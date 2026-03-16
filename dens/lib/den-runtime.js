// ============================================================
// den-runtime.js — Reactive den runtime
//
// A den is a reactive Prolog state. Messages are invisible
// infrastructure: facts flow in (accept), signals flow out.
// The serve loop, JSON plumbing, persistence, and POST are
// handled here.
//
// Portable: same var-only style as reactive.js / prolog-engine.js
// ============================================================

function createDen(opts) {
  var name = (opts && opts.name) || "den";
  var maxRequests = (opts && opts.maxRequests) || 0;
  var subscriptions = (opts && opts.subscribe) || [];

  /* Use y8's engine if available (has persist wired), else create fresh */
  var _eng = (typeof _engine !== "undefined") ? _engine : new PrologEngine();
  var de = createDenEngine(_eng);
  var _handlers = {};
  var _programSize = 0;
  var _restored = false;
  var _postResults = null;

  // --- SQL escaping ---

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

  // --- load ---

  function load(text) {
    var n = loadString(de.engine, text);
    _programSize = de.engine.clauses.length;
    return n;
  }

  // --- on ---

  function on(action, fn) {
    _handlers[action] = fn;
  }

  // --- Prolog handle/1 + send/2 ---

  function _prologHandle(action, req) {
    if (req.from !== undefined) de.updateFact("from", [], [req.from]);
    if (req.message !== undefined) de.updateFact("message", [], [req.message]);
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
    if (term.type === "atom") return {ok: true, from: name, response: term.name};
    if (term.type === "num") return {ok: true, from: name, response: term.value};
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

  // --- POST (Power On Self Test) ---

  function _post() {
    var results = [];
    var failed = 0;

    function check(testName, fn) {
      try {
        var ok = fn();
        results.push({test: testName, ok: ok});
        if (!ok) failed++;
      } catch (e) {
        results.push({test: testName, ok: false, error: e.message});
        failed++;
      }
    }

    // Configuration
    check("name", function() { return name && name.length > 0; });
    check("engine", function() { return de && de.engine && de.engine.clauses; });

    // Database
    check("db_exec", function() {
      bedrock.db_exec("CREATE TABLE IF NOT EXISTS _post (id INTEGER PRIMARY KEY)");
      return true;
    });
    check("db_query", function() {
      var rows = bedrock.db_query("SELECT 1 as v");
      var parsed = JSON.parse(rows);
      return parsed.length === 1 && parsed[0].v === 1;
    });

    // State persistence
    check("state_table", function() {
      var rows = JSON.parse(bedrock.db_query(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='_prolog_state'"
      ));
      return rows.length === 1;
    });

    // Store (if configured)
    check("store", function() {
      var resp = bedrock.request(JSON.stringify({action: "init"}));
      if (!resp) return false;
      var data = JSON.parse(resp);
      return data.ok === true;
    });

    // Prolog engine
    check("prolog", function() {
      var eng = new PrologEngine();
      loadString(eng, "test(ok).\n");
      var r = eng.queryFirst(PrologEngine.compound("test", [PrologEngine.variable("X")]));
      return r && r.args[0].name === "ok";
    });

    // Parser
    check("parser", function() {
      var t = parseTerm("hello(world)");
      return t.type === "compound" && t.functor === "hello";
    });

    // Fetch (privileged only)
    if (typeof bedrock.fetch === "function") {
      check("fetch", function() {
        return true; // fetch is available
      });
    }

    // Report
    var total = results.length;
    _postResults = {total: total, failed: failed, results: results};

    if (failed > 0) {
      bedrock.log(name + " POST: " + (total - failed) + "/" + total + " passed, " + failed + " FAILED");
      for (var i = 0; i < results.length; i++) {
        if (!results[i].ok) {
          bedrock.log(name + " POST FAIL: " + results[i].test +
            (results[i].error ? " (" + results[i].error + ")" : ""));
        }
      }
    } else {
      bedrock.log(name + " POST: " + total + "/" + total + " passed");
    }

    return failed === 0;
  }

  // --- init ---

  function _init() {
    _initStateTable();
    var n = _restoreState();
    if (n > 0) {
      _restored = true;
      bedrock.log(name + " restored " + n + " facts");
    }
    _post();
  }

  _init();

  // --- run ---

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
        if (maxRequests > 0 && nulls > 3) break;
        continue;
      }
      nulls = 0;

      var response;
      try {
        var req = JSON.parse(raw);
        var action = req.action || "unknown";

        // Built-in POST action
        if (action === "post") {
          _post();
          response = _postResults;
        } else if (_handlers[action]) {
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

    var saved = _saveState();
    bedrock.log(name + " stopped (" + count + " requests, " + saved + " facts saved)");
  }

  return {
    engine: de.engine,
    reactive: de,
    restored: _restored,
    post: _postResults,
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
