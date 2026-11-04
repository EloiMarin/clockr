# Denotational semantics in ten minutes — and how to think them up

Denotational semantics is the discipline the whole library leans on. This
walkthrough is the gentler start: first the idea itself, in ten minutes,
then the process by which such a meaning was (and can be) worked out for
this codebase. The complete specification lives separately in
[`../reference/denotational-semantics.md`](../reference/denotational-semantics.md) —
the semantic domain, the meaning of every construct, and the laws. Come back
to it when the idea is in hand.

## Denotational semantics in ten minutes

A program is text — *syntax*. Denotational semantics assigns every syntactic
construct a mathematical object — its *denotation* — through a meaning
function written with "semantic brackets":

```
⟦ construct ⟧  =  mathematical object
```

Two requirements separate this from an informal description:

- **Compositionality**: the meaning of a composite is built *only* from the
  meanings of its parts. The equation for `⟦a >> b⟧` may use `⟦a⟧` and `⟦b⟧`,
  but never their internal structure. If you find yourself peeking inside a
  part to define the whole, the API is leaking.
- **Implementation-independence**: the domain may not mention nodes, spans,
  vectors, atomics, or evaluation order. Those belong to the *operational*
  story — what the machine does step by step. The denotation says *what is
  computed*; the implementation's job is to *preserve* it.

Two C++ analogies:

1. `std::sort`'s contract — "the result is a permutation of the input,
   ordered according to `comp`" — is a meaning statement. Introsort vs.
   pdqsort are operational choices. Denotational semantics is that contract,
   written in mathematics, for the whole API instead of one function.
2. SQL. A query denotes a relation (a set of rows); a query plan (hash join
   vs. nested loop) is operational. Two plans are interchangeable exactly
   when they preserve the denotation. Keep this analogy: several of our
   operators (`share` today) turn out to be *plan-level*
   annotations whose denotation is the identity — and that is a feature, not
   a gap (the operator section of the reference, §2.5, has the list; the
   worked example in §5.2 shows the method catching a plan-level annotation
   in the act).

Why Conal Elliott insists on this: if the meaning of every operator fits on
one page, users can reason about programs **without reading the
implementation**, and implementers get checkable obligations ("does
`evaluate` preserve `⟦·⟧`?"). FRP was *defined* this way (Fran; Elliott &
Hudak 1997): `Behavior a = Time → a` over continuous time, `Event a` = a list
of `(time, value)` occurrences. Our engine keeps the method and changes the
domain (the reference's §6, relation to classic FRP).

## The mental process — how to arrive at this yourself

The five steps, narrated as they were applied to this codebase:

1. **Forget the code.** For each construct, ask: what does a user *mean* by
   it, in one behavioural sentence? "A signal has one value per tick." "A
   seek either happens this tick or not." Write those sentences first.
2. **Choose the smallest mathematics that expresses the sentences.** Here:
   sequences indexed by ticks, plus tick→time maps to relate threads. Ban
   implementation vocabulary (node, span, buffer, atomic) from the domain —
   if it sneaks in, you are describing the implementation, not the meaning.
   And if no small domain suffices, treat that as design feedback: Elliott's
   "simple and precise" is a *force on the design*, not just a description of
   it.
3. **Write `⟦·⟧` for every public construct, compositionally.** Where you
   cannot, do not force it: either the construct is an operational annotation
   (denote it as identity and record its contract separately — this is how
   `share` and `on` were found), or the design leaks (redesign it).
4. **Check totality and compositionality; every failure is a design fact.**
   Queue overflow → the capacity precondition (L6). Feedback loops → the
   delay-guard rule (L7), which is precisely the topology rule A1 implemented.
   Positional ports → a convention the semantics makes visible, motivating
   named ports. Same-epoch multiplicity → the coalescing rule (L9): an epoch
   contributes a map, not a sequence — and the one-value-per-epoch discipline
   it names for producers.
5. **Harvest laws; spend them.** Laws become tests (L5 already exists),
   optimizations (L3), tooling justifications (L1), and documentation (L4).

Honest difficulties to expect when you repeat this: the continuous-vs-
discrete-time fork is *the* classic decision (we chose discrete clocked
streams, following Lustre/rhine rather than Fran); events-as-`Maybe`-streams
vs occurrence lists is another (we chose `Maybe`-streams: they match per-tick
evaluation and make `queued` batching a one-line equation).

## Where to go next

The reference finishes the story this walkthrough starts:
[`../reference/denotational-semantics.md`](../reference/denotational-semantics.md)
§1 sets the semantic domain (clocked streams), §2 gives the meaning of every
construct that exists — with §2.7 the observer — and §3 then covers the
constructs and capabilities still planned. The laws in §4 are where
walkthrough exercises end and real obligations begin. And when the words are
the problem, not the mathematics, [`../../GLOSSARY.md`](../../GLOSSARY.md)
settles the vocabulary.
