#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <set>
#include <string>
#include <vector>

#include "pmra/arena_resource.hpp"
#include "pmra/pool_resource.hpp"

namespace {

bool is_aligned(void* p, std::size_t a) {
  return (reinterpret_cast<std::uintptr_t>(p) % a) == 0;
}

}  // namespace

TEST(Arena, HandsBackDistinctNonOverlappingBlocks) {
  pmra::arena_resource a(1024);
  std::set<void*> seen;
  for (int i = 0; i < 500; ++i) {
    void* p = a.allocate(24, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(seen.insert(p).second) << "address handed out twice";
    std::memset(p, 0xAB, 24);
  }
}

TEST(Arena, RespectsAlignmentRequests) {
  pmra::arena_resource a(4096);
  for (std::size_t align : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
    for (int i = 0; i < 20; ++i) {
      void* p = a.allocate(7, align);  // odd size, to force misalignment
      EXPECT_TRUE(is_aligned(p, align)) << "align=" << align;
    }
  }
}

TEST(Arena, GrowsPastInitialBlockAndServesOversizedRequests) {
  pmra::arena_resource a(256);
  a.allocate(128, 8);
  a.allocate(128, 8);
  a.allocate(128, 8);              // forces a new block
  EXPECT_GT(a.blocks_allocated(), 1u);

  void* big = a.allocate(1 << 20, 64);  // larger than any growth step so far
  ASSERT_NE(big, nullptr);
  EXPECT_TRUE(is_aligned(big, 64));
  std::memset(big, 1, 1 << 20);
}

TEST(Arena, ReleaseResetsAndIsReusable) {
  pmra::arena_resource a(1024);
  for (int i = 0; i < 100; ++i) a.allocate(64, 8);
  EXPECT_GT(a.bytes_reserved(), 0u);

  a.release();
  EXPECT_EQ(a.blocks_allocated(), 0u);
  EXPECT_EQ(a.bytes_reserved(), 0u);

  void* p = a.allocate(64, 8);  // must work again after release
  ASSERT_NE(p, nullptr);
}

TEST(Arena, WorksAsBackingForStandardContainers) {
  pmra::arena_resource a(4096);
  std::pmr::vector<std::pmr::string> v(&a);
  for (int i = 0; i < 2000; ++i) {
    v.emplace_back("a moderately long string that will not fit in SSO storage");
  }
  ASSERT_EQ(v.size(), 2000u);
  EXPECT_EQ(v.back().front(), 'a');
}

TEST(Pool, ReusesFreedBlocks) {
  pmra::pool_resource p(32, 64);
  void* first = p.allocate(32, 8);
  p.deallocate(first, 32, 8);
  void* again = p.allocate(32, 8);
  EXPECT_EQ(first, again) << "freed block was not recycled";
}

TEST(Pool, TracksLiveCountAndRefillsOnce) {
  pmra::pool_resource p(64, 100);
  std::vector<void*> live;
  for (int i = 0; i < 100; ++i) live.push_back(p.allocate(64, 8));
  EXPECT_EQ(p.live_blocks(), 100u);
  EXPECT_EQ(p.chunks_allocated(), 1u);

  live.push_back(p.allocate(64, 8));  // one past the chunk
  EXPECT_EQ(p.chunks_allocated(), 2u);

  for (void* q : live) p.deallocate(q, 64, 8);
  EXPECT_EQ(p.live_blocks(), 0u);
}

TEST(Pool, BlocksDoNotOverlapWhileLive) {
  pmra::pool_resource p(48, 50);
  std::vector<void*> live;
  for (int i = 0; i < 200; ++i) {
    void* q = p.allocate(48, 8);
    std::memset(q, i & 0xFF, 48);
    live.push_back(q);
  }
  // If any two blocks overlapped, an earlier memset would have been clobbered.
  for (std::size_t i = 0; i < live.size(); ++i) {
    auto* bytes = static_cast<unsigned char*>(live[i]);
    for (int j = 0; j < 48; ++j) {
      ASSERT_EQ(bytes[j], static_cast<unsigned char>(i & 0xFF))
          << "overlap at block " << i;
    }
  }
  for (void* q : live) p.deallocate(q, 48, 8);
}

TEST(Pool, DelegatesOversizedRequestsUpstream) {
  pmra::pool_resource p(32, 16);
  void* big = p.allocate(4096, 8);  // too big for the pool
  ASSERT_NE(big, nullptr);
  std::memset(big, 7, 4096);
  p.deallocate(big, 4096, 8);
  EXPECT_EQ(p.live_blocks(), 0u) << "upstream request must not count as live";
}

TEST(Pool, RoundsTinyBlockSizeUpToPointerSize) {
  pmra::pool_resource p(1, 8);  // 1 byte cannot hold the free-list pointer
  EXPECT_GE(p.block_size(), sizeof(void*));
  void* a = p.allocate(1, 1);
  p.deallocate(a, 1, 1);
  EXPECT_EQ(p.allocate(1, 1), a);
}
