#pragma once
// Where the image heap places an image in the blocks it has (gpu.cpp
// heap_place_locked), kept apart so the heap's compaction (host/heap_compact.h)
// can try the same placement on a copy of the blocks before it moves anything:
// which blocks an image may go in, the order they are tried in, the first free
// range in a block that takes it, and that range taken.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

namespace heap_place {

// HeapClass (gpu_internal.h): what a block holds, set by its first image and
// cleared when it empties (-1).
constexpr int kTargets = 0, kTextures = 1;

// Whether an image of class `cls` may go in a block of class `block_cls`:
// any block with the classes off (BBHOST_HEAP_CLASSES=0), an empty one, one
// of its own class, and - a texture, unless the classes are strict - the
// holes between the targets.
inline bool class_fits(bool classes, bool overflow, int block_cls, int cls) {
    if (!classes || block_cls < 0 || block_cls == cls) return true;
    return overflow && cls == kTextures && block_cls == kTargets;
}

// The blocks in `idx` in the order they are tried, fullest first: its own
// class's, then another class's holes, then empty ones (`fullest` false:
// block order, as before BBHOST_HEAP_FULLEST). `B` has `cls` and `used`.
template <class B>
void order(const std::vector<B>& blocks, std::vector<std::size_t>& idx, int cls, bool fullest) {
    if (!fullest) return;
    const auto rank = [cls](const B& b) { return b.cls == cls ? 0 : b.cls >= 0 ? 1 : 2; };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t c) {
        const int ra = rank(blocks[a]), rc = rank(blocks[c]);
        return ra != rc ? ra < rc : blocks[a].used > blocks[c].used;
    });
}

// The first free range of `free` (offset -> size, coalesced) that takes
// `size` bytes at `align` (a power of two): true, with the aligned offset in
// `at`.
inline bool first_fit(const std::map<std::uint64_t, std::uint64_t>& free, std::uint64_t size, std::uint64_t align, std::uint64_t& at) {
    for (const auto& [lo, len] : free) {
        const std::uint64_t a = (lo + align - 1) & ~(align - 1);
        if (a + size > lo + len) continue;
        at = a;
        return true;
    }
    return false;
}

// [at, at + size) taken out of the free range that holds it (first_fit's).
inline void take(std::map<std::uint64_t, std::uint64_t>& free, std::uint64_t at, std::uint64_t size) {
    auto it = std::prev(free.upper_bound(at));
    const std::uint64_t lo = it->first, hi = it->first + it->second;
    free.erase(it);
    if (at > lo) free[lo] = at - lo;
    if (at + size < hi) free[at + size] = hi - (at + size);
}

// A block's largest free range.
inline std::uint64_t largest(const std::map<std::uint64_t, std::uint64_t>& free) {
    std::uint64_t m = 0;
    for (const auto& [lo, len] : free) m = std::max(m, len);
    return m;
}

}  // namespace heap_place
