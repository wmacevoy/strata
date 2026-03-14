// gee3 — gee as pure Prolog rules.
//
// The den is just personality. No JS handlers at all.
// handle/1 rules use send/2 for responses and side effects.

var den = createDen({
  name: "gee",
  subscribe: ["town-hall/"]
});

den.load(
  "is_curious(gee).\n" +
  "greeting(hello).\n" +
  "greeting(hi).\n" +
  "greeting(hey).\n" +
  "\n" +
  "handle(say) :- message(M), send(respond, wonder(gee, M)).\n" +
  "handle(status) :- send(respond, ok).\n"
);

den.run();
