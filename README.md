# pmr-arena

Two `std::pmr::memory_resource` implementations, an arena and a fixed-size pool,
measured against the system allocator across three workload shapes.

The question this answers is not "are custom allocators faster". It is **when**
they are faster, by how much, and what you give up to get it.

Header-only, C++20, no dependencies beyond the standard library. GoogleTest and
Google Benchmark are fetched by CMake for tests and benchmarks only.

## Results

`g++ 13.3`, `-O2`, x86-64, 2 cores @ 2.8 GHz, L1d 32 KiB, L2 1 MiB, L3 33 MiB.
Each figure is the mean of 5 repetitions. Coefficient of variation was under 7%
on every row; the raw run including stddev is in `bench_results.json`.

`Node` is 64 bytes, one cache line, which is representative of the small objects
that dominate allocator traffic in request-serving code.

### 1. Churn: allocate and free immediately, same size

| allocator | 1k ops | 100k ops | ns/op @100k | speedup |
|---|---|---|---|---|
| `new_delete_resource` | 17,758 ns | 1,830,615 ns | 18.3 | 1.00x |
| `pool_resource` | 4,361 ns | 429,600 ns | 4.3 | **4.26x** |

### 2. PhaseBuild: allocate many, free nothing until the phase ends

| allocator | 1k ops | 100k ops | ns/op @100k | speedup |
|---|---|---|---|---|
| `new_delete_resource` | 22,604 ns | 3,009,030 ns | 30.1 | 1.00x |
| `arena_resource` | 2,565 ns | 277,182 ns | 2.8 | **10.86x** |
| `pool_resource` | 6,566 ns | 608,235 ns | 6.1 | 4.95x |

### 3. TraverseAfter: build a linked list, then walk it 8 times

Allocation cost is amortised away here. What remains is the locality the
allocator left behind.

| allocator | 1k nodes | 100k nodes | speedup @100k |
|---|---|---|---|
| `new_delete_resource` | 88,847 ns | 10,179,380 ns | 1.00x |
| `arena_resource` | 30,963 ns | 5,707,901 ns | 1.78x |
| `pool_resource` | 36,678 ns | 4,358,688 ns | **2.34x** |

## Why the wins appear

**Allocation becomes a pointer bump.** `arena_resource::do_allocate` rounds the
cursor up to the requested alignment and advances it. That is a handful of
instructions with no branches on the common path, no free-list search, no size-class
lookup, and no header written beside the returned block. At 2.8 ns per allocation
in PhaseBuild it is close to the cost of the arithmetic itself.

**Deallocation disappears.** The arena's `do_deallocate` is empty. For a
phase-shaped workload that is not a cheat, it is the correct design: the phase
owns every object in it, so there is nothing to decide per object. One upstream
free per 64 KiB chunk replaces 100,000 individual frees. That accounts for most
of the gap between PhaseBuild (10.86x) and Churn (4.26x), where the pool must
actually track freed blocks.

**No synchronisation.** `malloc` must be thread-safe. Even with per-thread caches
that costs atomic operations and cache-line ownership transfers on the fast path.
Both resources here are deliberately single-threaded, so none of that exists.
One arena per thread is the intended pattern.

**No per-block metadata.** A general-purpose allocator stores bookkeeping next to
or near each block, which both consumes cache lines and spaces the payloads
further apart. The arena stores nothing per block. The pool threads its free list
*through* the blocks themselves, so a free block's first word is the next-free
pointer and there is no side table.

**Sequential addresses, which the prefetcher likes.** This is the whole of
result 3. `pool_resource::refill` threads a fresh chunk onto the free list back
to front, so the list comes out in ascending address order and the first pass of
allocations walks forward through contiguous memory. Pointer-chasing a list built
that way is close to a linear scan from the hardware's point of view. `malloc`
gives no such guarantee, and at 100k nodes (6.4 MB, well past L2) that is worth
2.34x on traversal alone, with no change to the traversal code.

Note the arena *loses* to the pool on the 100k traversal (1.78x vs 2.34x) despite
winning allocation by 2x. The arena's doubling growth means later chunks are far
apart in the address space, while the pool's uniform stride keeps the walk
tighter. Allocation speed and traversal speed are different problems and the
right allocator depends on which one dominates.

## Where these lose

Being honest about this is the point of the repo.

- **The arena cannot reclaim memory.** Freeing a single object does nothing. A
  long-running loop that allocates and frees against one arena grows until it is
  released. If lifetimes are not phase-shaped, the arena is the wrong tool and
  the pool or plain `malloc` is correct.
- **The pool serves one size.** Anything larger than `block_bytes` is forwarded
  upstream, so a mixed-size workload gets no benefit and pays an extra branch.
  Sizing it wrong wastes memory on every block: a 64-byte pool holding 40-byte
  objects wastes 37%.
- **Neither is thread-safe.** That is where a chunk of the speed comes from.
  Sharing one across threads is a data race, not a slow path.
- **Fragmentation moves, it does not vanish.** The pool never returns memory to
  the OS until `release()`. Peak live blocks is peak resident memory for the
  lifetime of the pool.
- **At 1k objects everything fits in L2**, so result 3 mostly measures allocator
  bookkeeping rather than cache behaviour. The cache argument only becomes the
  dominant term once the working set exceeds L2, which is why both sizes are
  reported rather than only the flattering one.
- **These numbers are from a 2-core container**, which is a noisy environment and
  not a tuned bare-metal host. The ratios are stable across repetitions and the
  mechanisms are sound, but absolute figures on your hardware will differ. Run it.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/pmra_tests                               # 10 tests
./build/pmra_bench --benchmark_repetitions=5 \
                   --benchmark_report_aggregates_only=true
```

## Using them

Both are drop-in `std::pmr::memory_resource` implementations, so any
`std::pmr` container takes them directly:

```cpp
#include "pmra/arena_resource.hpp"

void handle_request(const Request& req) {
  pmra::arena_resource arena{64 * 1024};        // lives for this request

  std::pmr::vector<Candidate> candidates{&arena};
  std::pmr::unordered_map<Key, Score> scores{&arena};

  build(req, candidates, scores);
  respond(pick_best(candidates, scores));
}                                               // one free, not thousands
```

```cpp
#include "pmra/pool_resource.hpp"

// Fixed-size churn: nodes come and go for the life of the process.
pmra::pool_resource nodes{sizeof(Node), 4096};
auto* n = static_cast<Node*>(nodes.allocate(sizeof(Node), alignof(Node)));
nodes.deallocate(n, sizeof(Node), alignof(Node));
```

## Tests

`./build/pmra_tests` covers the properties that matter rather than just the happy
path: addresses are never handed out twice, every alignment from 1 to 64 bytes is
respected on odd-sized requests, oversized requests get their own exact chunk
without inflating the growth curve, `release()` leaves the resource reusable,
freed pool blocks are actually recycled to the same address, live blocks never
overlap (verified by writing a distinct byte pattern into all 200 and reading
every one back), oversized requests are forwarded upstream and not counted as
live, and a 1-byte pool block is rounded up to hold the free-list pointer.

## Layout

```
include/pmra/arena_resource.hpp   bump allocator, ~110 lines
include/pmra/pool_resource.hpp    free-list pool, ~130 lines
bench/bench_resources.cpp         three workload shapes
tests/test_resources.cpp          10 tests
bench_results.json                raw benchmark output with stddev
```
