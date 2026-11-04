# clockr::conc

The `conc` module (target `clockr::conc`) provides cross-thread primitives.

The design constraint across the module: these primitives are meant for the
real-time audio path, so they must never block the caller and must not touch
the heap. Every operation is non-blocking and allocation-free, and the
primitives are deliberately restricted to single-producer / single-consumer.

## Contents

### `double_buffer.hpp`

```cpp
template<typename T>
concept inlineable = std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(std::uint64_t);
```

`double_buffer<T>` is the *latest-value* primitive: one writer stores a
snapshot, one reader loads it, synchronized with release/acquire semantics
via a plain `std::atomic<T>`. Despite the name it is a single atomic slot,
not a ping-pong pair. It is bounded-time by construction (one atomic
store/load) and requires `T` to be `inlineable` (fit inside a machine word)
so the atomic can be lock-free.

### `spsc.hpp`

`spsc<T>` is the *FIFO* primitive: a bounded, non-blocking
single-producer/single-consumer queue.
It wraps [moodycamel::ReaderWriterQueue](https://github.com/cameron314/readerwriterqueue) and adds an explicit capacity bound:

This is the backpressure mechanism of the module: a full queue rejects
instead of blocking, leaving policy (drop, overwrite, retry later) to the
caller.

### `link.hpp`

`link` is the project's named primitive from the design proposal: the
mechanism for bounded-time snapshot exchange at explicit synchronization points.
It is a small, `mode`-parameterized wrapper:

- `policy::latest`: Backed by a small ring of `slot<T>` cells holding
  `{epoch, value}` stamps; `send(value, epoch)` stages one epoch (same-
  epoch repeats overwrite, last wins), `receive(published)` returns the
  newest stamp at or before the consumer's snapshotted cut (silence holds
  the last defined value; a lapped snapshot reports stale so the caller can
  re-snapshot and retry). Requires trivially-copyable `T`.
- `policy::queued`: Backed by `spsc<T>` of `{epoch, value}` stamps;
  `send(value, epoch)` enqueues one epoch, `receive(value&, published)`
  dequeues the head only when its epoch is published (future heads wait.
  Epoch-stamped acceptance, so one producer epoch becomes visible on all
  channels together). Bool-returning FIFO, up to the bounded capacity.
  Works with any `T`.

The choice of mode expresses the semantics of the connection: `latest` for
state that is safe to overwrite (e.g. UI slider positions), `queued` for
event streams that must stay ordered and must not be collapsed (e.g. trigger
events).
A producer that cannot afford a loss must size the capacity and watch
`program::dropped_sends()`.

## How it fits together

The module is a small, layered stack:

```
link  (project-level API: latest / queued epoch-stamped semantics)
  ├─ slot       (tear-free stamped cells; latest keeps a small ring)
  └─ spsc       (bounded FIFO of stamps over moodycamel, any T)
```

Tests live in `tests/link.test.cpp` (executable `conc_tests`).
