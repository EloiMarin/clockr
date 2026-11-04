# clockr::mem

`mem` module (target `clockr::mem`) provides memory primitives and
allocation suitable for real-time paths.

## Contents

### `arena.hpp` / `arena.cpp`

`arena` is the bottom of the stack: a move-only owner of one contiguous,
aligned allocation (`std::aligned_alloc` / `_aligned_malloc` on Windows),
64-byte aligned, with the requested size rounded up to the alignment.

### `pool.hpp` / `pool.cpp`

`pool` turns an `arena` slab into a general-purpose,
bounded allocator built on [TLSF](https://github.com/mattconte/tlsf).

- `allocate(bytes)` / `allocate_aligned(bytes, align)`: O(1).
- `deallocate(ptr)`: O(1).
- `owns(ptr)`: Cheap range check against the arena slab, useful for
  debugging or for validating pointers crossing API boundaries.
- `check()`: Runs TLSF's internal integrity checks (findable block list, pool free-list).
- `snapshot()`: Walks the pool to report `used_bytes` / `free_bytes` / `overhead_bytes`.
- `minimal_size()`: The smallest arena that can host a pool (TLSF control
  structures + overhead + one minimal block). The constructor throws
  `std::bad_alloc` for smaller arenas.

Like `arena`, a `pool` is non-copyable and (to keep pointer provenance
simple) non-movable after construction.

### `pool_allocator.hpp`

`pool_allocator<T>` is the adapter for the standard library:
a `std::allocator`-conforming front end over a `pool`.

```cpp
mem::arena arena { 1 << 20 };
mem::pool pool { std::move(arena) };

std::vector<int, mem::pool_allocator<int>> values { mem::pool_allocator<int> { pool } };
```

So `std` containers (and anything else that speaks the allocator protocol)
can draw their memory from the real-time-safe pool instead of the heap.

## How it fits together

A three-layer stack:

```
pool_allocator<T>  (STL adapter — containers can use the pool)
pool               (O(1) TLSF allocation and deallocation within the slab)
arena              (owns the aligned slab itself)
```

`arena` exists so the raw memory's lifecycle is a single, movable owner;
`pool` gives that slab a real-time-safe allocator; `pool_allocator` bridges
the gap to idiomatic C++ so library and application code can allocate without
leaving the real-time-safe path.

Tests live in `tests/pool.test.cpp` (executable `mem_tests`).
