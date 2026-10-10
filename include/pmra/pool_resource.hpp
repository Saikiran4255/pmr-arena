// pool_resource.hpp — fixed-size free-list pool as a std::pmr::memory_resource.
//
// Where the arena never reuses memory, this does. It serves one block size and
// keeps freed blocks on an intrusive singly-linked list threaded through the
// blocks themselves, so there is no side table and no per-block header. Alloc
// is pop-front, free is push-front: a load and a store each.
//
// This is the right shape when you churn many objects of one size (nodes,
// request contexts, fixed-size buffers) and lifetimes are not phase-shaped, so
// an arena would grow without bound.
//
// Not thread-safe, same reasoning as the arena: one pool per thread.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <vector>

namespace pmra {

class pool_resource final : public std::pmr::memory_resource {
public:
  // block_bytes is rounded up to hold at least a pointer, since a free block
  // stores the next-free pointer in its own first word.
  pool_resource(std::size_t block_bytes,
                std::size_t blocks_per_chunk = 1024,
                std::pmr::memory_resource* upstream =
                    std::pmr::get_default_resource())
      : upstream_(upstream),
        block_bytes_(block_bytes < sizeof(void*) ? sizeof(void*) : block_bytes),
        blocks_per_chunk_(blocks_per_chunk),
        chunks_(upstream) {}

  pool_resource(const pool_resource&) = delete;
  pool_resource& operator=(const pool_resource&) = delete;

  ~pool_resource() override { release(); }

  void release() noexcept {
    for (auto& c : chunks_) {
      upstream_->deallocate(c.ptr, c.bytes, alignof(std::max_align_t));
    }
    chunks_.clear();
    free_list_ = nullptr;
    live_ = 0;
  }

  std::size_t block_size() const noexcept { return block_bytes_; }
  std::size_t chunks_allocated() const noexcept { return chunks_.size(); }
  std::size_t live_blocks() const noexcept { return live_; }

private:
  struct chunk {
    void* ptr;
    std::size_t bytes;
  };

  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    // Requests this pool cannot serve go straight upstream. Keeping the
    // fast path honest matters more than serving every size badly.
    if (bytes > block_bytes_ || alignment > alignof(std::max_align_t)) {
      return upstream_->allocate(bytes, alignment);
    }
    if (free_list_ == nullptr) refill();

    void* p = free_list_;
    free_list_ = *static_cast<void**>(free_list_);
    ++live_;
    return p;
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    if (bytes > block_bytes_ || alignment > alignof(std::max_align_t)) {
      upstream_->deallocate(p, bytes, alignment);
      return;
    }
    *static_cast<void**>(p) = free_list_;
    free_list_ = p;
    --live_;
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  void refill() {
    const std::size_t stride = round_up(block_bytes_, alignof(std::max_align_t));
    const std::size_t bytes = stride * blocks_per_chunk_;

    auto* mem = static_cast<std::byte*>(
        upstream_->allocate(bytes, alignof(std::max_align_t)));
    chunks_.push_back(chunk{mem, bytes});

    // Thread the new chunk onto the free list back to front, so the list comes
    // out in ascending address order. Sequential addresses mean the first pass
    // over freshly allocated objects walks forward through memory, which the
    // hardware prefetcher handles well. This is most of the cache win.
    for (std::size_t i = blocks_per_chunk_; i-- > 0;) {
      void* blk = mem + i * stride;
      *static_cast<void**>(blk) = free_list_;
      free_list_ = blk;
    }
  }

  static constexpr std::size_t round_up(std::size_t n, std::size_t a) noexcept {
    return (n + (a - 1)) & ~(a - 1);
  }

  std::pmr::memory_resource* upstream_;
  void* free_list_ = nullptr;
  std::size_t block_bytes_;
  std::size_t blocks_per_chunk_;
  std::size_t live_ = 0;
  std::pmr::vector<chunk> chunks_;
};

}  // namespace pmra
