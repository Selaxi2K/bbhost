#pragma once

// Scaleform 4's render tree in eboot 1.09, and a text node shown before its
// layout exists.
//
// Every movie's display list is mirrored in a render context (Context, at
// MovieImpl +0x5350). Its nodes are 56-byte entries on 4 KiB pages; a page
// holds 72 of them from +0x38, and its header names the snapshot being built
// (+0x18), that snapshot's data table (+0x20) and the table of the snapshot
// on display (+0x28). A table lists each entry's node data from +0x28, a
// pointer whose bit 0 tags it. Changes go into the snapshot being built:
// getWritableData (0x482a70) gives an entry data of its own there,
// CreateEntry (0x4848d0) makes one, addToPropagate (0x482a40) lists one for
// the capture's propagate pass.
//
// A text node's data (TreeText::NodeData, vtable 0x55447b0) holds its
// document view (+0x90), its layout (+0x98) and a layout-changed flag (+0xa0
// bit 0). The layout is built only in the capture: Context::Capture
// (0x484c20) runs the propagate pass (0x485030), whose TreeText step
// (0x4e3110, the data's vtable +0x30) makes a layout from the view when the
// flag is set, and then publishes the snapshot under the context's lock. The
// render side takes it in NextCapture (0x483a00) and builds its own cache
// nodes from the data on display; a text's cache node (TreeCacheText, vtable
// 0x5547470) reads the layout in its transform update (0x52d5e0, vtable
// +0x28) whenever the update carries flag bit 0, and never checks it for
// null.
//
// In Bloodborne the movie's Advance and Capture run as a CSEzWork job
// (0x2358e50) and NextCapture on the GX job thread (0x2359150), both under
// the movie wrapper's lock (+0x38, its mutex at +0x40). Nothing else in the
// game takes that lock, and the render tree's changes take none of their
// own, so a text changed by another thread after a capture's propagate pass
// is published with its flag set and no layout - and off every later
// propagate list, its list link left in the drained one - and faults at
// 0x52d6c0, reading +0x1c of the null layout. That was a death during an
// area load on the Steam Deck; the other thread is the in-game HUD's update
// (below).
//
// The guard (on; BBHOST_SF_TEXT_GUARD=0 turns it off): at the transform
// update's entry, a text cache node whose displayed data has no layout runs
// the update without bit 0 - the part that snaps the text and keys its mesh
// from the layout, which has nothing to work on yet. When a later capture
// brings the layout (its change 0x400 reaches the node's HandleChanges,
// 0x52df80, before that capture's tree update), the node is marked for the
// update with the flag (0x10000) the tree update turns into bit 0, as the
// game's own sub_4d6b90 marks a node whose transform changed, so the part
// runs then. Logged as "sf text:".
//
// BBHOST_SF_TEXT_CHECK=1 adds the detector: every getWritableData,
// CreateEntry and addToPropagate on a movie's context is classed by the
// movie's wrapper lock and capture - made under the lock; while another
// thread is in the movie's capture (between its propagate pass and its
// unlock: the window above); while the Advance job, or the NextCapture job,
// holds the lock (the latter is Scaleform's design: the render side works on
// the snapshot on display); or with the lock free. The exit report counts them by thread
// and the first of each kind are logged with their callers.
// BBHOST_TEST_SF_CAPTURE_US=<n> sleeps n microseconds in every capture
// between its propagate pass and its lock - the window, made wide for a test.
//
// The cause (on; BBHOST_SF_MOVIE_LOCK=0 turns it off): the movie changed
// outside its lock is the in-game HUD's, by its update (0x1ff7c20), an EzWork
// job queued beside the movies' Advance jobs that drives the HUD movie
// through GFx's Value API. The update now runs under its movie wrapper's
// lock, as the movie's own jobs run theirs (run_locked below; the hook is in
// engine/sf_text_layout.cpp). Logged as "sf lock:". The guard stays as the
// check behind it.

#include "hle/sync.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct ElfImage;

void sf_text_layout_install(ElfImage* image);
// The exit report's lines: the guard's count, and the detector's classes.
void sf_text_layout_report();

namespace sf_text_layout {

// EntryPage (4 KiB aligned).
constexpr std::uint64_t kPageMask = ~std::uint64_t{0xfff};
constexpr std::uint64_t kPageSnapshot = 0x18;  // Snapshot* being built
constexpr std::uint64_t kPageActive = 0x20;    // its SnapshotPage (the data table changes go to)
constexpr std::uint64_t kPageDisplay = 0x28;   // the displayed snapshot's SnapshotPage
constexpr std::uint64_t kPageEntries = 0x38;   // Entry[72], 56 bytes each
constexpr std::uint64_t kEntrySize = 56;
constexpr std::uint64_t kEntriesPerPage = 72;
constexpr std::uint64_t kTableData = 0x28;      // SnapshotPage: the entries' data pointers, bit 0 a tag
constexpr std::uint64_t kSnapshotContext = 0x10;  // Snapshot: its Context
constexpr std::uint64_t kSnapshotPropagate = 0x40;  // Snapshot: its propagate list's head (next, prev)
constexpr std::uint64_t kEntryPropagate = 0x28;     // Entry: its link in that list (next, prev)
constexpr std::uint64_t kContextActive = 0xd0;      // Context: the snapshot being built
constexpr std::uint64_t kContextCaptured = 0xd8;    // Context: the snapshot the capture just published
constexpr std::uint64_t kMovieContext = 0x5350;     // MovieImpl: its Context

// TreeText::NodeData.
constexpr std::uint64_t kTextDocView = 0x90;
constexpr std::uint64_t kTextLayout = 0x98;
constexpr std::uint64_t kTextLayoutChanged = 0xa0;  // byte, bit 0

// TreeCacheNode.
constexpr std::uint64_t kNodeRoot = 0x18;   // TreeCacheRoot*
constexpr std::uint64_t kNodeEntry = 0x20;  // its Entry
constexpr std::uint64_t kNodeFlags = 0x44;  // update flags; bit 31 pending
constexpr std::uint64_t kNodeNext = 0x48;   // the root's pending list link
// TreeCacheRoot: its pending list's head, and the byte set while its tree
// update runs (0x4dc0c0 to the end of 0x4dc280), when the game marks a node
// into the update's own lists instead.
constexpr std::uint64_t kRootPending = 0xa0;
constexpr std::uint64_t kRootInUpdate = 0xaa;

// The transform update's flag that reads the layout, and the node flag the
// tree update (0x4dc280) turns into it for a node it updates.
constexpr std::uint32_t kUpdateLayoutPart = 0x1;
constexpr std::uint32_t kNodeRetransform = 0x10000;
constexpr std::uint32_t kNodePending = 0x80000000u;

inline std::uint64_t rd(std::uint64_t a) {
    std::uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}
inline std::uint32_t rd32(std::uint64_t a) {
    std::uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}
inline std::uint8_t rd8(std::uint64_t a) {
    return *reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(a));
}
inline void wr(std::uint64_t a, std::uint64_t v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v));
}
inline void wr32(std::uint64_t a, std::uint32_t v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof(v));
}

// An entry's index on its page, as the game computes it: the byte offset
// from the first entry over 8, times the inverse of 7 mod 2^32 (56 = 8 * 7),
// in 32 bits.
inline std::uint32_t entry_index(std::uint64_t entry) {
    const std::uint64_t page = entry & kPageMask;
    const std::uint32_t q = static_cast<std::uint32_t>((entry - (page + kPageEntries)) >> 3);
    return q * 0xb6db6db7u;
}

// The entry's data in the table at page offset `which` (kPageDisplay: what
// the render side reads; kPageActive: what the next capture will publish),
// the tag cleared; 0 when anything on the way cannot be read.
// `readable(addr, len)` says whether guest memory there can be read.
template <typename Readable>
std::uint64_t entry_data(std::uint64_t entry, std::uint64_t which, Readable readable) {
    if (!readable(entry, 8)) return 0;
    const std::uint64_t page = entry & kPageMask;
    if (entry < page + kPageEntries) return 0;
    const std::uint32_t i = entry_index(entry);
    if (i >= kEntriesPerPage || !readable(page, kPageEntries)) return 0;
    const std::uint64_t table = rd(page + which);
    const std::uint64_t slot = table + kTableData + std::uint64_t{i} * 8;
    if (!table || !readable(slot, 8)) return 0;
    return rd(slot) & ~std::uint64_t{1};
}

// The entry's Context, through the snapshot its page is building; 0 when it
// cannot be read.
template <typename Readable>
std::uint64_t entry_context(std::uint64_t entry, Readable readable) {
    const std::uint64_t page = entry & kPageMask;
    if (!readable(page, kPageEntries)) return 0;
    const std::uint64_t snap = rd(page + kPageSnapshot);
    if (!snap || !readable(snap + kSnapshotContext, 8)) return 0;
    return rd(snap + kSnapshotContext);
}

// The guard's decision for the transform update of the text cache node
// `node` with `flags`.
struct Decision {
    std::uint32_t flags;  // what the update runs with
    bool skipped;         // the layout part was taken out
    std::uint64_t data;   // the displayed data (when skipped)
};
template <typename Readable>
Decision decide(std::uint64_t node, std::uint32_t flags, Readable readable) {
    if (!(flags & kUpdateLayoutPart) || !readable(node + kNodeEntry, 8)) return {flags, false, 0};
    const std::uint64_t data = entry_data(rd(node + kNodeEntry), kPageDisplay, readable);
    // Data the game could not read either is left to the game: this guards
    // the missing layout, not a broken tree.
    if (!data || !readable(data + kTextLayout, 8) || rd(data + kTextLayout) != 0) return {flags, false, 0};
    return {flags & ~kUpdateLayoutPart, true, data};
}

// Marks a cache node for its root's next tree update, as the game's
// sub_4d6b90 does while it takes a capture's changes (0x498af0, before the
// update): onto the root's pending list unless it is pending already, with
// `flags` and the pending bit. False (nothing written) when the node has no
// root, or the root's update is under way - a node left pending after it
// could be destroyed before the next one drains the list.
template <typename Readable>
bool mark_for_update(std::uint64_t node, std::uint32_t flags, Readable readable) {
    if (!readable(node + kNodeRoot, 8)) return false;
    const std::uint64_t root = rd(node + kNodeRoot);
    if (!root || !readable(root + kRootPending, 16) || !readable(node + kNodeFlags, 12)) return false;
    if (rd8(root + kRootInUpdate) != 0) return false;
    const std::uint32_t f = rd32(node + kNodeFlags);
    if (!(f & kNodePending)) {
        wr(node + kNodeNext, rd(root + kRootPending));
        wr(root + kRootPending, node);
    }
    wr32(node + kNodeFlags, f | flags | kNodePending);
    return true;
}

// Whether `link` is an entry's propagate link on a page whose snapshot being
// built is `snap`: +0x28 of an entry slot of such a page.
template <typename Readable>
bool is_entry_link(std::uint64_t link, std::uint64_t snap, Readable readable) {
    const std::uint64_t entry = link - kEntryPropagate;
    const std::uint64_t page = entry & kPageMask;
    if (entry < page + kPageEntries || (entry - (page + kPageEntries)) % kEntrySize != 0 ||
        (entry - (page + kPageEntries)) / kEntrySize >= kEntriesPerPage)
        return false;
    return readable(page, kPageEntries) && rd(page + kPageSnapshot) == snap && readable(link, 16);
}

// An entry left on a propagate list that nothing drains any more - a list a
// capture published after its propagate pass had run - is put back, with
// every other entry on that list, on the list of the snapshot being built
// (`snap`), linked as addToPropagate (0x482a40) links one. The old list's
// head is neither read nor written: its snapshot may be gone. Returns how
// many entries were moved; 0 when the entry is on the live list or on none,
// or its list does not walk back to one head both ways (left as it is).
template <typename Readable>
int restrand(std::uint64_t entry, std::uint64_t snap, Readable readable) {
    constexpr int kMax = 64;
    const std::uint64_t link = entry + kEntryPropagate;
    if (!snap || !readable(link, 16) || !readable(snap + kSnapshotPropagate, 16)) return 0;
    const std::uint64_t live = snap + kSnapshotPropagate;
    if (rd(link) == 0) return 0;  // on no list
    std::uint64_t members[2 * kMax + 1];
    int n = 0;
    members[n++] = link;
    std::uint64_t ends[2] = {0, 0};
    for (int dir = 0; dir < 2; ++dir) {
        std::uint64_t x = rd(link + 8 * dir);
        for (int steps = 0;; ++steps) {
            if (x == live) return 0;  // on the live list: the next pass has it
            if (!is_entry_link(x, snap, readable)) break;
            if (x == link || steps == kMax) return 0;  // a loop without a head
            members[n++] = x;
            x = rd(x + 8 * dir);
        }
        ends[dir] = x;
    }
    if (ends[0] != ends[1]) return 0;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t m = members[i], first = rd(live);
        wr(m, first);
        wr(m + 8, live);
        wr(first + 8, m);
        wr(live, m);
    }
    return n;
}

// At a capture's unlock (0x484ffe: the snapshot published, the context's
// pages naming the next one), entries still on the published snapshot's
// propagate list were listed after its propagate pass - by another thread -
// and nothing would ever propagate them. They are moved to the list of the
// snapshot being built (`next`), linked as addToPropagate links one, and the
// published list is left empty: the next capture's pass lays out a text
// among them, and no entry is left linked into a list that dies with its
// snapshot. All or nothing: when an entry on the list is not one of the
// context's own (its page names another snapshot) or the list does not lead
// back to its head within 256 steps, nothing is written. Returns how many
// entries were moved.
template <typename Readable>
int destrand(std::uint64_t published, std::uint64_t next, Readable readable) {
    constexpr int kMax = 256;
    if (!published || !next || published == next) return 0;
    if (!readable(published + kSnapshotPropagate, 16) || !readable(next + kSnapshotPropagate, 16)) return 0;
    const std::uint64_t head = published + kSnapshotPropagate, live = next + kSnapshotPropagate;
    std::uint64_t members[kMax];
    int n = 0;
    for (std::uint64_t x = rd(head); x != head; x = rd(x)) {
        if (n == kMax || !is_entry_link(x, next, readable)) return 0;
        members[n++] = x;
    }
    if (n == 0) return 0;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t m = members[i], first = rd(live);
        wr(m, first);
        wr(m + 8, live);
        wr(first + 8, m);
        wr(live, m);
    }
    wr(head, head);
    wr(head + 8, head);
    return n;
}

// A movie wrapper seen for `movie` that no longer holds it: gone, or its
// destructor ran (it clears the movie, +0x18, first), or its memory went to
// something else. Its table entry may go to another movie.
template <class Readable>
bool wrapper_stale(std::uint64_t movie, std::uint64_t wrapper, Readable readable) {
    return !wrapper || !readable(wrapper + 0x18, 8) || rd(wrapper + 0x18) != movie;
}

// The table of movie wrappers seen (engine/sf_text_layout.cpp): an entry a
// movie, found by the movie, filled in by the wrapper's jobs. A new movie
// takes a free entry; when none is free, one whose wrapper no longer holds
// its movie (wrapper_stale) - a long session makes and drops far more movies
// than are alive at once; with neither, it is left out, counted. The game's
// own list of live wrappers holds 32 (SprjScaleform's at +0x990), so 128
// entries leave room for the dead ones not yet taken again.
constexpr int kWrapperTableSize = 128;
struct WrapperEntry {
    std::atomic<std::uint64_t> movie{0}, wrapper{0};
    std::atomic<gsync::Thread*> advance{nullptr}, display{nullptr}, capturing{nullptr};
};
struct WrapperTable {
    WrapperEntry* entries;
    int size;  // in use of the entries (BBHOST_TEST_SF_TABLE shrinks it, for a test)
    std::atomic<std::uint64_t> reused{0}, left_out{0};
};

inline WrapperEntry* table_find(WrapperTable& t, std::uint64_t movie) {
    for (int i = 0; i < t.size; ++i) {
        if (t.entries[i].movie.load(std::memory_order_acquire) == movie) return &t.entries[i];
    }
    return nullptr;
}

// Wrapper `w`, which holds `movie` (the caller read it): the movie's entry,
// made or taken again; nullptr when the table has no room (counted).
template <class Readable>
WrapperEntry* table_note(WrapperTable& t, std::uint64_t w, std::uint64_t movie, Readable readable, bool* fresh = nullptr) {
    if (fresh) *fresh = false;
    if (WrapperEntry* e = table_find(t, movie)) {
        if (e->wrapper.load(std::memory_order_relaxed) != w) e->wrapper.store(w, std::memory_order_release);
        return e;
    }
    if (fresh) *fresh = true;
    for (int i = 0; i < t.size; ++i) {
        WrapperEntry& e = t.entries[i];
        std::uint64_t expected = 0;
        if (e.movie.load(std::memory_order_relaxed) == 0 && e.movie.compare_exchange_strong(expected, movie)) {
            e.wrapper.store(w, std::memory_order_release);
            return &e;
        }
    }
    for (int i = 0; i < t.size; ++i) {
        WrapperEntry& e = t.entries[i];
        std::uint64_t old = e.movie.load(std::memory_order_acquire);
        if (!old || !wrapper_stale(old, e.wrapper.load(std::memory_order_acquire), readable)) continue;
        if (!e.movie.compare_exchange_strong(old, movie)) continue;
        e.advance.store(nullptr, std::memory_order_relaxed);
        e.display.store(nullptr, std::memory_order_relaxed);
        e.capturing.store(nullptr, std::memory_order_relaxed);
        e.wrapper.store(w, std::memory_order_release);
        t.reused.fetch_add(1, std::memory_order_relaxed);
        return &e;
    }
    t.left_out.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// Entries whose wrapper still holds their movie: the movies alive in it.
template <class Readable>
int table_live(WrapperTable& t, Readable readable) {
    int n = 0;
    for (int i = 0; i < t.size; ++i) {
        const std::uint64_t movie = t.entries[i].movie.load(std::memory_order_acquire);
        if (movie && !wrapper_stale(movie, t.entries[i].wrapper.load(std::memory_order_acquire), readable)) ++n;
    }
    return n;
}

// How run_locked ran its body.
enum class LockRun {
    kLocked,       // under the lock, taken for it
    kAlreadyHeld,  // this thread held the lock already (the game's is recursive)
    kTimedOut,     // the lock stayed busy past the wait: without it, as the game has it
};

// Runs `body` under the gsync mutex in `slot` - a movie wrapper's lock (its
// scePthreadMutex at +0x40) - as the movie's Advance and NextCapture jobs run
// theirs, unless this thread holds it already. A wait past `wait_us` runs
// `body` without it: a guard against a lock order the game never takes, not
// a path it should see. `waited_us` gets how long a busy lock took (0 when
// it was free).
template <class Body>
LockRun run_locked(void** slot, std::uint64_t wait_us, std::uint64_t* waited_us, Body&& body) {
    *waited_us = 0;
    if (gsync::mutex_owner(slot) == gsync::current_thread()) {
        body();
        return LockRun::kAlreadyHeld;
    }
    if (gsync::mutex_trylock(slot) != 0) {
        const auto t0 = gsync::Clock::now();
        const gsync::Deadline dl = t0 + std::chrono::microseconds(wait_us);
        const int rc = gsync::mutex_lock(slot, &dl);
        *waited_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(gsync::Clock::now() - t0).count());
        if (rc != 0) {
            body();
            return LockRun::kTimedOut;
        }
    }
    body();
    gsync::mutex_unlock(slot);
    return LockRun::kLocked;
}

}  // namespace sf_text_layout
