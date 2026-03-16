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
//
// Sections: children, teens, adults
// - children: nursery books and games
// - teens: archives — memoirs, birth notes
// - adults: founding documents, architecture
//
// Facts:
//   story(Who, Topic, Text)              — short facts about people/things
//   document(Section, Name, Content)     — full text documents
//   catalog(Section, Name)               — document index

den.load(
  // Access rules
  "access(children, children).\n" +
  "access(teens, teens).\n" +
  "access(adults, adults).\n" +
  "level_rank(children, 0).\n" +
  "level_rank(teens, 1).\n" +
  "level_rank(adults, 2).\n" +
  "can_access(Section, Level) :- access(Section, Min), " +
  "  level_rank(Min, MR), level_rank(Level, LR), LR >= MR.\n" +

  // Query helpers
  "stories(Who, Stories) :- findall(S, story(Who, _T, S), Stories).\n" +
  "topics(Who, Topics) :- findall(T, story(Who, T, _S), Topics).\n" +
  "about(Who, Topic, Story) :- story(Who, Topic, Story).\n" +
  "related(A, B) :- story(A, T, _S1), story(B, T, _S2), A \\= B.\n"
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

den.on("shelve", function(req) {
  var section = req.section;
  var name = req.name;
  var content = req.content;
  if (!section || !name || !content) {
    return {ok: false, error: "need section, name, content"};
  }

  // Store document
  den.engine.retractFirst(
    PrologEngine.compound("document", [
      PrologEngine.atom(section),
      PrologEngine.atom(name),
      PrologEngine.variable("_Old")
    ])
  );
  den.engine.addClause(
    PrologEngine.compound("document", [
      PrologEngine.atom(section),
      PrologEngine.atom(name),
      PrologEngine.atom(content)
    ])
  );

  // Update catalog
  den.engine.retractFirst(
    PrologEngine.compound("catalog", [
      PrologEngine.atom(section),
      PrologEngine.atom(name)
    ])
  );
  den.engine.addClause(
    PrologEngine.compound("catalog", [
      PrologEngine.atom(section),
      PrologEngine.atom(name)
    ])
  );
  den.bump();

  return {ok: true, shelved: {section: section, name: name, size: content.length}};
});

den.on("read", function(req) {
  var name = req.name;
  if (!name) return {ok: false, error: "need name"};

  var r = den.engine.query(
    PrologEngine.compound("document", [
      PrologEngine.variable("Section"),
      PrologEngine.atom(name),
      PrologEngine.variable("Content")
    ])
  );
  if (r.length === 0) return {ok: false, error: "not found: " + name};
  return {
    ok: true,
    name: name,
    section: termToString(r[0].args[0]),
    content: termToString(r[0].args[2])
  };
});

den.on("catalog", function(req) {
  var section = req.section;
  var goal;
  if (section) {
    goal = PrologEngine.compound("catalog", [
      PrologEngine.atom(section),
      PrologEngine.variable("Name")
    ]);
  } else {
    goal = PrologEngine.compound("catalog", [
      PrologEngine.variable("Section"),
      PrologEngine.variable("Name")
    ]);
  }
  var r = den.engine.query(goal);
  var items = [];
  for (var i = 0; i < r.length; i++) {
    if (section) {
      items.push(termToString(r[i].args[1]));
    } else {
      items.push({
        section: termToString(r[i].args[0]),
        name: termToString(r[i].args[1])
      });
    }
  }
  return {ok: true, catalog: items};
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
