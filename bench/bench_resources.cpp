// Benchmarks for pmra::arena_resource and pmra::pool_resource against the
// system allocator, via std::pmr::new_delete_resource.
//
// Three workload shapes, because "is a custom allocator faster" has no single
// answer and a benchmark that only measures the flattering case is worthless:
//
//   1. Churn       - allocate and free immediately, fixed size. Pool territory.
//   2. PhaseBuild  - allocate many, free nothing until the phase ends. Arena
//                    territory, and the shape most real request handling takes.
//   3. TraverseAfter - build a linked structure, then walk it. Measures the
//                    locality the allocator left behind, not allocation cost.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory_resource>
#include <vector>

#include "pmra/arena_resource.hpp"
#include "pmra/pool_resource.hpp"

namespace {

// A node sized so that a few fit in a cache line, which is typical of the
// small objects that dominate allocator traffic in real services.
struct Node {
  Node* next;
  std::uint64_t a, b, c, d, e, f, g;
};
static_assert(sizeof(Node) == 64);

// ---------------------------------------------------------------- 1. Churn

void Churn_NewDelete(benchmark::State& state) {
  auto* mr = std::pmr::new_delete_resource();
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) {
      void* p = mr->allocate(sizeof(Node), alignof(Node));
      benchmark::DoNotOptimize(p);
      mr->deallocate(p, sizeof(Node), alignof(Node));
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}

void Churn_Pool(benchmark::State& state) {
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  pmra::pool_resource pool(sizeof(Node), 4096);
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) {
      void* p = pool.allocate(sizeof(Node), alignof(Node));
      benchmark::DoNotOptimize(p);
      pool.deallocate(p, sizeof(Node), alignof(Node));
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}

// ----------------------------------------------------------- 2. PhaseBuild

void PhaseBuild_NewDelete(benchmark::State& state) {
  auto* mr = std::pmr::new_delete_resource();
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  std::vector<void*> live;
  live.reserve(n);
  for (auto _ : state) {
    live.clear();
    for (std::size_t i = 0; i < n; ++i) {
      live.push_back(mr->allocate(sizeof(Node), alignof(Node)));
    }
    benchmark::DoNotOptimize(live.data());
    for (void* p : live) mr->deallocate(p, sizeof(Node), alignof(Node));
  }
  state.SetItemsProcessed(state.iterations() * n);
}

void PhaseBuild_Arena(benchmark::State& state) {
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    // A fresh arena per phase is the intended usage: the phase ends, the
    // arena is released, and that release is one upstream free per chunk.
    pmra::arena_resource arena(64 * 1024);
    for (std::size_t i = 0; i < n; ++i) {
      void* p = arena.allocate(sizeof(Node), alignof(Node));
      benchmark::DoNotOptimize(p);
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}

void PhaseBuild_Pool(benchmark::State& state) {
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    pmra::pool_resource pool(sizeof(Node), 4096);
    for (std::size_t i = 0; i < n; ++i) {
      void* p = pool.allocate(sizeof(Node), alignof(Node));
      benchmark::DoNotOptimize(p);
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}

// -------------------------------------------------------- 3. TraverseAfter

// Build a chain of n nodes from mr, then walk it `walks` times. The allocation
// cost is amortised away; what is left is how far apart the allocator scattered
// the nodes.
template <typename MakeResource>
void Traverse(benchmark::State& state, MakeResource make) {
  const std::size_t n = static_cast<std::size_t>(state.range(0));
  constexpr int walks = 8;
  for (auto _ : state) {
    auto holder = make();
    std::pmr::memory_resource* mr = holder.get();

    Node* head = nullptr;
    for (std::size_t i = 0; i < n; ++i) {
      auto* node = static_cast<Node*>(mr->allocate(sizeof(Node), alignof(Node)));
      node->next = head;
      node->a = i;
      head = node;
    }

    std::uint64_t sum = 0;
    for (int w = 0; w < walks; ++w) {
      for (Node* p = head; p != nullptr; p = p->next) sum += p->a;
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * n * walks);
}

// Small holders so Traverse can own whatever lifetime each resource needs.
struct NewDeleteHolder {
  std::pmr::memory_resource* get() { return std::pmr::new_delete_resource(); }
};
struct ArenaHolder {
  pmra::arena_resource r{64 * 1024};
  std::pmr::memory_resource* get() { return &r; }
};
struct PoolHolder {
  pmra::pool_resource r{sizeof(Node), 4096};
  std::pmr::memory_resource* get() { return &r; }
};

void Traverse_NewDelete(benchmark::State& s) {
  Traverse(s, [] { return NewDeleteHolder{}; });
}
void Traverse_Arena(benchmark::State& s) {
  Traverse(s, [] { return ArenaHolder{}; });
}
void Traverse_Pool(benchmark::State& s) {
  Traverse(s, [] { return PoolHolder{}; });
}

constexpr int kSmall = 1000;
constexpr int kLarge = 100000;

}  // namespace

BENCHMARK(Churn_NewDelete)->Arg(kSmall)->Arg(kLarge);
BENCHMARK(Churn_Pool)->Arg(kSmall)->Arg(kLarge);

BENCHMARK(PhaseBuild_NewDelete)->Arg(kSmall)->Arg(kLarge);
BENCHMARK(PhaseBuild_Arena)->Arg(kSmall)->Arg(kLarge);
BENCHMARK(PhaseBuild_Pool)->Arg(kSmall)->Arg(kLarge);

BENCHMARK(Traverse_NewDelete)->Arg(kSmall)->Arg(kLarge);
BENCHMARK(Traverse_Arena)->Arg(kSmall)->Arg(kLarge);
BENCHMARK(Traverse_Pool)->Arg(kSmall)->Arg(kLarge);

BENCHMARK_MAIN();
