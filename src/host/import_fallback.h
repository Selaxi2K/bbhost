#pragma once
// A draw's vertex record or index range that lies in no guest-memory import -
// its import failed (a driver's limit on imported memory, the address space,
// a udmabuf size limit on Linux; BBHOST_TEST_IMPORT_FAIL), or none holds the
// range whole - is read from a copy instead, so a failed import costs speed,
// never the picture (buffer_shadow.cpp shadow_import_fallback_locked, render.cpp
// at a vertex record's and an index range's placement). Kept apart from Vulkan
// so a test can drive the choice.
//
// The mirrors first, where the buffer shadow runs: their pages are copied
// through upload staging by the CPU (as window imports copy them), so they
// need no import, and a mesh is copied once rather than at every draw. Else
// the bytes the draw reads, copied as it records into the snapshot ring (up
// to 1 MiB a range). Else, counted by why, the range is not copied: a vertex
// record then walks the page table (the sink page for memory in no import),
// and a draw whose indices lie in no import is skipped.
#include <cstdint>

namespace import_fallback {

enum Choice : int {
    kMirror = 0,    // served by the mirrors through staged copies
    kCopy,          // copied as the draw records into the snapshot ring
    kOff,           // BBHOST_IMPORT_COPY=0
    kTooBig,        // past the ring's largest copy and no mirror served it
    kGpuPending,    // a GPU write is still to land on it: a copy now would miss it
    kNoRing,        // the snapshot ring could not be made
    kChoices
};

// What serves a range in no import of `bytes` bytes: `mirror_served` whether
// the mirrors served it (asked first, where the buffer shadow runs), `max_copy`
// the ring's largest copy, `gpu_pending` whether a GPU write is still to land
// on the range, `ring_ok` whether the snapshot ring exists or can be made.
inline Choice choose(bool on, bool mirror_served, std::uint64_t bytes, std::uint64_t max_copy, bool gpu_pending, bool ring_ok) {
    if (!on) return kOff;
    if (mirror_served) return kMirror;
    if (gpu_pending) return kGpuPending;
    if (!bytes || bytes > max_copy) return kTooBig;
    if (!ring_ok) return kNoRing;
    return kCopy;
}

// Whether the choice gives the draw its bytes.
inline bool served(Choice c) { return c == kMirror || c == kCopy; }

}  // namespace import_fallback
