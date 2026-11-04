# Further reading

This is material I've found while developing `clockr`.
It's not necessary to read or watch any of this in order to use the library,
but it's related literature that can be very interesting if you also want to dive deep into this.

I just wish I knew the existence of many of these resources when I started :)

## 1. FRP: the original idea and its flavours

- **Conal Elliott and Paul Hudak, _Functional Reactive Animation_ (ICFP
  1997).** The original denotational formulation: behaviours are
  continuous-time functions and events are occurrence lists. It is the
  starting point that [`denotational-semantics.md`](denotational-semantics.md)
  deliberately departs from.
- **Conal Elliott, [_The Essence & Origins of Functional Reactive Programming_](https://www.youtube.com/watch?v=rfmkzp76M4M) (YOW! 2015).**
  Must-watch to understand the motivation behind Elliott's emphasis on denotational semantics.
- **Paul Hudak, Antony Courtney, Henrik Nilsson, and John Peterson,
  [_Arrows, Robots, and Functional Reactive Programming_](https://www.cs.yale.edu/homes/hudak/CS429F04/AFPLectureNotes.pdf)
  (AFP 2002).** The Yampa tutorial: arrowized FRP, continuous signals, and
  the space- and time-leak discipline that motivates bounded-state designs.
- **Evan Czaplicki, [_Elm: Concurrent FRP for Functional GUIs_](https://elm-lang.org/assets/papers/concurrent-frp.pdf) (thesis,
  2012).
- **Evan Czaplicki and Stephen Chong, [_Asynchronous Functional Reactive
  Programming for
  GUIs_](https://people.seas.harvard.edu/~chong/pubs/pldi13-elm.pdf) (PLDI
  2013).** Elm's asynchronous FRP: how a practical GUI language trades the
  continuous-time model for event-driven updates.
- **[_Sodium_](https://github.com/SodiumFRP/sodium).** A push-based FRP
  library with an explicit operational model. The companion book by
  Blackheath and Jones, _Functional Reactive Programming_ (Manning, 2016),
  is a very good introduction to the classic FRP formulation.
- **Ivan Perez, Manuel Bärenz, and Henrik Nilsson, _Functional Reactive
  Programming, Refactored_ (Haskell Symposium 2016).** Monadic stream
  functions — the lineage of the recipe layer's stream-function view.
- **Manuel Bärenz and Ivan Perez, [_Rhine: FRP with Type-Level Clocks_](https://www.researchgate.net/publication/329548187_Rhine_FRP_with_type-level_clocks)
  (Haskell Symposium 2018).** The closest published model to `clockr`:
  clocks in the type system, explicit resampling between them, and a
  synchronous core.
- **Conal Elliott, _Denotational Design_ (talks and essays).** The design
  method used throughout this repository: give each construct a meaning
  first, then an implementation that respects it.

## 2. Synchronous languages: clocks as a first-class discipline

- **[_Synchronous programming language_](https://en.wikipedia.org/wiki/Synchronous_programming_language)
  (Wikipedia).** A short orientation to the family — Lustre, Esterel,
  Signal, Lucid Synchrone — and to the vocabulary used across this
  documentation.
- **Nicolas Halbwachs, [_Synchronous Programming of Reactive Systems_](https://www-verimag.imag.fr/~halbwach/newbook.pdf) (1993).**
  The textbook account of the synchronous hypothesis, clocks, and compilation for reactive systems.
- **Paul Caspi, Daniel Pilaud, Nicolas Halbwachs, and John Plaice, _LUSTRE:
  A declarative language for programming synchronous systems_ (POPL 1987).**
  Clocked streams and guarded feedback; a direct ancestor of the region-and-clock model.
- **Marc Pouzet, [_Lucid Synchrone_](https://www.di.ens.fr/~pouzet/bib/chap_lucid_synchrone_english_iste08.pdf)
  (ISTE 2008 chapter).** Multi-sampled systems, clock inference, and state
  machines in an ML-style synchronous language.
- **Marc Pouzet, [_Lucid Synchrone manual — Multi-clock systems_](https://www.di.ens.fr/~pouzet/lucid-synchrone/manual_html/manual006.html).**
  The `when` and `merge` operators and their clock constraints, worked
  through in code: the most direct explanation of why resampling is
  explicit.

## 3. Semantics and operational foundations

- **Joseph E. Stoy, [_Denotational Semantics: The Scott–Strachey Approach to Programming Language Theory_](https://www.amazon.com/exec/obidos/ASIN/0262690764/acmorg-20)
  (MIT Press, 1981).** The classic introduction to denotational definitions, the method behind [`denotational-semantics.md`](denotational-semantics.md).
- **Hans Hüttel, [_Transitions and Trees: An Introduction to Structural Operational Semantics_](https://www.cambridge.org/core/books/transitions-and-trees/B1C5947833933470511AB2560F9EA3AB)
  (Cambridge University Press, 2010).** The operational counterpart:
  transition systems and structural rules, useful when reading the engine's tick and state semantics.

## 4. Audio and this project

- **[_YampaSynth_](https://hackage.haskell.org/package/YampaSynth).**
  A software synthesizer built with [Yampa](https://hackage.haskell.org/package/Yampa).
- **[_Functional Reactive Programming for Audio Software_](../archive/adc-2026/index.md) (ADC 2026).**
  The talk page: abstract, slides, and links to the live demo.
