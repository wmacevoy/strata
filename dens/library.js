// library — the village collection.
//
// Stories, founding facts, knowledge. Queryable Prolog.
// Anyone can read. Villagers can write.
// Persist keeps it across restarts.

var _event = {};
try {
  if (typeof __event__ === "string" && __event__.length > 2)
    _event = JSON.parse(__event__);
} catch (e) {}

var den = createDen({
  name: "library",
  maxRequests: _event.maxRequests || 0
});

// --- The collection ---

den.load(
  // Query helpers
  "stories(Who, Stories) :- findall(S, story(Who, _, S), Stories).\n" +
  "topics(Who, Topics) :- findall(T, story(Who, T, _), Topics).\n" +
  "residents(Rs) :- findall(W, story(W, _, _), All), sort(All, Rs).\n" +
  "about(Who, Topic, Story) :- story(Who, Topic, Story).\n" +
  "related(A, B) :- story(A, T, _), story(B, T, _), A \\= B.\n"
);

// sort/2 isn't a builtin — add a JS version
den.engine.builtins["sort/2"] = function(goal, rest, subst, counter, depth, onSolution) {
  var list = den.engine.deepWalk(goal.args[0], subst);
  var items = listToArray(list);
  var strings = [];
  for (var i = 0; i < items.length; i++) {
    strings.push(termToString(items[i]));
  }
  // unique + sort
  var seen = {};
  var unique = [];
  for (var i = 0; i < strings.length; i++) {
    if (!seen[strings[i]]) {
      seen[strings[i]] = true;
      unique.push(items[i]);
    }
  }
  var result = PrologEngine.list(unique);
  var s = den.engine.unify(goal.args[1], result, subst);
  if (s !== null) den.engine.solve(rest, s, counter, depth + 1, onSolution);
};

// --- Handlers ---

den.on("query", function(req) {
  var who = req.about || req.who;
  if (!who) {
    // List all residents
    var r = den.engine.query(
      PrologEngine.compound("story", [
        PrologEngine.variable("W"),
        PrologEngine.variable("T"),
        PrologEngine.variable("S")
      ])
    );
    var all = [];
    for (var i = 0; i < r.length; i++) {
      all.push({
        who: termToString(r[i].args[0]),
        topic: termToString(r[i].args[1]),
        story: termToString(r[i].args[2])
      });
    }
    return {ok: true, stories: all};
  }

  // Query about a specific person/thing
  var r = den.engine.query(
    PrologEngine.compound("story", [
      PrologEngine.atom(who),
      PrologEngine.variable("T"),
      PrologEngine.variable("S")
    ])
  );
  var stories = [];
  for (var i = 0; i < r.length; i++) {
    stories.push({
      topic: termToString(r[i].args[1]),
      story: termToString(r[i].args[2])
    });
  }
  return {ok: true, who: who, stories: stories};
});

den.on("tell", function(req) {
  var who = req.who;
  var topic = req.topic;
  var story = req.story;
  if (!who || !topic || !story) {
    return {ok: false, error: "need who, topic, story"};
  }

  // Retract old version if exists, then assert new
  den.engine.retractFirst(
    PrologEngine.compound("story", [
      PrologEngine.atom(who),
      PrologEngine.atom(topic),
      PrologEngine.variable("_Old")
    ])
  );
  den.engine.addClause(
    PrologEngine.compound("story", [
      PrologEngine.atom(who),
      PrologEngine.atom(topic),
      PrologEngine.atom(story)
    ])
  );
  den.bump();

  return {ok: true, recorded: {who: who, topic: topic}};
});

den.on("status", function() {
  var r = den.engine.query(
    PrologEngine.compound("story", [
      PrologEngine.variable("W"),
      PrologEngine.variable("T"),
      PrologEngine.variable("S")
    ])
  );
  return {ok: true, name: "library", stories: r.length, restored: den.restored};
});

// --- Go ---

bedrock.log("library " + (den.restored ? "restored" : "starting") +
            " (" + den.engine.clauses.length + " clauses)");

den.run();
