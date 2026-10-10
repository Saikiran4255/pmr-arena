// arena_resource.hpp — monotonic bump allocator as a std::pmr::memory_resource.
//
// Allocation is a pointer bump. Deallocation is a no-op: memory is reclaimed
// only when the resource is destroyed or release() is called. That trade is the
// whole point. If your allocation lifetimes are phase-shaped (build a graph,
// walk it, throw it away) you get allocation for a few instructions and you
// stop paying for allocator metadata, free-list maintenance and the locks that
// a general-purpose allocator needs to be thread-safe.
//
// Not thread-safe by design. One arena per thread is the intended pattern;
// sharing one across threads would reintroduce the synchronisation this is
// meant to avoid.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <utility>
#include <vector>

namespace pmra {

class arena_resource final : public std::pmr::memory_resource {
public:
  // first_block_bytes is the initial chunk size. When a chunk is exhausted the
  // next one is twice as large, capped at max_block_bytes, so a small arena
  // that turns out to be busy stops making syscall-sized requests forever
  // without growing unboundedly.
  explicit arena_resource(std::size_t first_block_bytes = 64 * 1024,
                          std::size_t max_block_bytes = 8 * 1024 * 1024,
                          std::pmr::memory_resource* upstream =
                              std::pmr::get_default_resource())
      : upstream_(upstream),
        next_block_bytes_(first_block_bytes),
        max_block_bytes_(max_block_bytes),
        blocks_(upstream) {}

  arena_resource(const arena_resource&) = delete;
  arena_resource& operator=(const arena_resource&) = delete;

  ~arena_resource() override { release(); }

  // Hand every chunk back upstream and start over. Cheap: one upstream
  // deallocation per chunk, not per object.
  void release() noexcept {
    for (auto& b : blocks_) {
      upstream_->deallocate(b.ptr, b.bytes, b.alignment);
    }
    blocks_.clear();
    cur_ = nullptr;
    end_ = nullptr;
  }

  // Diagnostics, used by the benchmarks and tests.
  std::size_t blocks_allocated() const noexcept { return blocks_.size(); }
  std::size_t bytes_reserved() const noexcept {
    std::size_t n = 0;
    for (auto const& b : blocks_) n += b.bytes;
    return n;
  }

private:
  struct block {
    void* ptr;
    std::size_t bytes;
    std::size_t alignment;
  };

  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    // Round the cursor up to the requested alignment, then bump.
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(cur_);
    std::uintptr_t aligned = (p + (alignment - 1)) & ~(alignment - 1);

    if (aligned + bytes > reinterpret_cast<std::uintptr_t>(end_)) {
      grow(bytes, alignment);
      p = reinterpret_cast<std::uintptr_t>(cur_);
      aligned = (p + (alignment - 1)) & ~(alignment - 1);
    }

    cur_ = reinterpret_cast<std::byte*>(aligned + bytes);
    return reinterpret_cast<void*>(aligned);
  }

  // No-op. This is the trade.
  void do_deallocate(void*, std::size_t, std::size_t) override {}

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  void grow(std::size_t bytes, std::size_t alignment) {
    // A single allocation larger than our next chunk gets its own exact chunk,
    // so one huge request does not inflate the growth curve for everything after.
    std::size_t want = next_block_bytes_;
    if (bytes + alignment > want) want = bytes + alignment;

    void* mem = upstream_->allocate(want, alignof(std::max_align_t));
    blocks_.push_back(block{mem, want, alignof(std::max_align_t)});

    cur_ = static_cast<std::byte*>(mem);
    end_ = cur_ + want;

    if (next_block_bytes_ < max_block_bytes_) {
      next_block_bytes_ = next_block_bytes_ * 2 < max_block_bytes_
                              ? next_block_bytes_ * 2
                              : max_block_bytes_;
    }
  }

  std::pmr::memory_resource* upstream_;
  std::byte* cur_ = nullptr;
  std::byte* end_ = nullptr;
  std::size_t next_block_bytes_;
  std::size_t max_block_bytes_;
  std::pmr::vector<block> blocks_;
};

}  // namespace pmra
