// The Scaleform text-layout guard (engine/sf_text_layout.h) without the game:
// a render context's entry page, the tables of the snapshot on display and of
// the one being built, text node data and a text cache node laid out as
// eboot 1.09 lays them out, then the read the game's TreeCacheText transform
// update makes at 0x52d68d-0x52d6c0 - the null layout's +0x1c, the fault on
// the Steam Deck - with the guard and without it; the marking that brings the
// layout's part back once a capture delivers the layout, and the tree
// update's walk over the root's pending list that turns it into the flag; and
// the cases the guard must leave alone. And the fix at the cause: the HUD's
// update run under its movie's lock (run_locked) against a stand-in for the
// movie's Advance job holding that lock across its capture, on a real gsync
// mutex.
#include "engine/sf_text_layout.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace sf_text_layout;

namespace {

int g_failed = 0;
void check(bool ok, const char* what, int line) {
    if (ok) return;
    if (++g_failed <= 40) std::printf("FAIL (line %d): %s\n", line, what);
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr std::uint64_t kTextVtable = 0x55447b0;

// One arena for every object, page aligned, so that "readable" can be told
// exactly and an entry page can sit on its own 4 KiB.
alignas(4096) std::uint8_t g_mem[1 << 17];
std::size_t g_used = 4096;  // page 0 stays unused: an entry page goes on a fresh page
std::uint64_t base() { return reinterpret_cast<std::uint64_t>(g_mem); }
bool readable(std::uint64_t a, std::size_t n) { return a >= base() && a + n <= base() + sizeof(g_mem); }
auto kReadable = [](std::uint64_t a, std::size_t n) { return readable(a, n); };

std::uint64_t alloc(std::size_t n, std::size_t align = 16) {
    g_used = (g_used + align - 1) & ~(align - 1);
    const std::uint64_t p = base() + g_used;
    g_used += n;
    std::memset(reinterpret_cast<void*>(p), 0, n);
    return p;
}

// An entry page, the tables of the snapshot on display and of the one being
// built (Data[72] from +0x28), and the snapshot with its context.
struct Page {
    std::uint64_t page, display, active, snapshot, context;
    std::uint64_t entry(unsigned i) const { return page + kPageEntries + std::uint64_t{i} * kEntrySize; }
};
Page make_page() {
    Page p{};
    p.page = alloc(4096, 4096);
    p.display = alloc(kTableData + kEntriesPerPage * 8);
    p.active = alloc(kTableData + kEntriesPerPage * 8);
    p.snapshot = alloc(0xa0);
    p.context = alloc(0x100);
    wr(p.page + kPageSnapshot, p.snapshot);
    wr(p.page + kPageActive, p.active);
    wr(p.page + kPageDisplay, p.display);
    wr(p.snapshot + kSnapshotContext, p.context);
    return p;
}
void set_data(std::uint64_t table, unsigned i, std::uint64_t data, bool tag) {
    wr(table + kTableData + std::uint64_t{i} * 8, data | (tag ? 1 : 0));
}

// TreeText::NodeData as its constructor leaves it (0x4e2e50): no view, no
// layout, the flag clear.
std::uint64_t make_text_data(std::uint64_t layout) {
    const std::uint64_t d = alloc(0xb0);
    wr(d, kTextVtable);
    wr(d + kTextDocView, 0);
    wr(d + kTextLayout, layout);
    return d;
}
// A TextLayout: what the update reads at +0x1c (its flags).
std::uint64_t make_layout() { return alloc(0xc0); }

struct Root {
    std::uint64_t root;
};
Root make_root() { return Root{alloc(0x100)}; }

// TreeCacheText (0x1a0 bytes): its root and entry; flags as the base
// constructor leaves them (0x4dcf70: 0x7e0000).
std::uint64_t make_node(const Root& r, std::uint64_t entry) {
    const std::uint64_t n = alloc(0x1a0);
    wr(n + kNodeRoot, r.root);
    wr(n + kNodeEntry, entry);
    wr32(n + kNodeFlags, 0x7e0000);
    return n;
}

// The game's read in TreeCacheText::UpdateTransform when its flags have bit 0
// (0x52d683: and r14d, 1; 0x52d68d-0x52d6c0): the entry's displayed data, its
// layout, and the layout's +0x1c. Returns the address it would read last, so
// 0x1c is the fault.
std::uint64_t game_read(std::uint64_t node, std::uint32_t flags) {
    if (!(flags & 1)) return 0;  // je 0x52d770: the layout is never read
    const std::uint64_t entry = rd(node + 0x20);
    const std::uint64_t page = entry & ~std::uint64_t{0xfff};
    const std::uint32_t idx = static_cast<std::uint32_t>((entry - (page + 0x38)) >> 3) * 0xb6db6db7u;
    const std::uint64_t table = rd(page + 0x28);
    const std::uint64_t data = rd(table + std::uint64_t{idx} * 8 + 0x28) & ~std::uint64_t{1};
    const std::uint64_t layout = rd(data + 0x98);
    return layout + 0x1c;  // test byte [r8+0x1c], 1
}

void test_entry_index() {
    const Page p = make_page();
    bool all = true;
    for (unsigned i = 0; i < kEntriesPerPage; ++i) all = all && entry_index(p.entry(i)) == i;
    CHECK(all);
    // The last entry ends inside the page.
    CHECK(p.entry(kEntriesPerPage - 1) + kEntrySize <= p.page + 4096);
}

void test_entry_data() {
    const Page p = make_page();
    const std::uint64_t shown = make_text_data(0), built = make_text_data(make_layout());
    set_data(p.display, 5, shown, true);
    set_data(p.active, 5, built, false);
    CHECK(entry_data(p.entry(5), kPageDisplay, kReadable) == shown);  // the tag cleared
    CHECK(entry_data(p.entry(5), kPageActive, kReadable) == built);
    CHECK(entry_context(p.entry(5), kReadable) == p.context);
    // Nothing on the way readable: 0, never a guess.
    CHECK(entry_data(0x1000, kPageDisplay, kReadable) == 0);
    CHECK(entry_data(p.page + 8, kPageDisplay, kReadable) == 0);  // inside the header, not an entry
    wr(p.page + kPageDisplay, 0);
    CHECK(entry_data(p.entry(5), kPageDisplay, kReadable) == 0);
}

// The fault's sequence: a text node whose data reaches the display without a
// layout, updated with bit 0 - with the guard and without it.
void test_crash_sequence() {
    const Page p = make_page();
    const Root r = make_root();
    const std::uint64_t data = make_text_data(0);
    wr(data + kTextDocView, make_layout());  // a view, the layout not built yet
    wr(data + kTextLayoutChanged, 1);
    set_data(p.display, 9, data, true);
    const std::uint64_t node = make_node(r, p.entry(9));
    const std::uint32_t flags = 0x41;  // bit 0 with others the update keeps
    // Without the guard the game reads the null layout's +0x1c: the fault.
    CHECK(game_read(node, flags) == 0x1c);
    // With it the layout is never read, and only bit 0 is gone.
    const Decision d = decide(node, flags, kReadable);
    CHECK(d.skipped && d.data == data && d.flags == 0x40);
    CHECK(game_read(node, d.flags) == 0);
    // The node itself is not touched by the decision.
    CHECK(rd32(node + kNodeFlags) == 0x7e0000 && rd(node + kNodeNext) == 0 && rd(r.root + kRootPending) == 0);
}

void test_left_alone() {
    const Page p = make_page();
    const Root r = make_root();
    const std::uint64_t layout = make_layout();
    // A layout on display: as the game has it.
    const std::uint64_t ok = make_text_data(layout);
    set_data(p.display, 1, ok, true);
    const std::uint64_t n1 = make_node(r, p.entry(1));
    const Decision d1 = decide(n1, 0x1, kReadable);
    CHECK(!d1.skipped && d1.flags == 0x1);
    CHECK(game_read(n1, d1.flags) == layout + 0x1c);
    // No bit 0: the layout is not read, nothing to do.
    const std::uint64_t none = make_text_data(0);
    set_data(p.display, 2, none, false);
    const std::uint64_t n2 = make_node(r, p.entry(2));
    const Decision d2 = decide(n2, 0x40, kReadable);
    CHECK(!d2.skipped && d2.flags == 0x40);
    // The displayed table decides, not the one being built: a layout the next
    // capture will publish does not make the displayed data safe...
    const std::uint64_t later = make_text_data(layout);
    set_data(p.display, 3, none, false);
    set_data(p.active, 3, later, false);
    const std::uint64_t n3 = make_node(r, p.entry(3));
    CHECK(decide(n3, 0x1, kReadable).skipped);
    // ...and a missing one there does not make a displayed layout unsafe.
    set_data(p.display, 4, ok, false);
    set_data(p.active, 4, none, false);
    const std::uint64_t n4 = make_node(r, p.entry(4));
    CHECK(!decide(n4, 0x1, kReadable).skipped);
    // A node whose entry or table cannot be read is the game's own business.
    const std::uint64_t n5 = make_node(r, 0x2000);
    CHECK(!decide(n5, 0x1, kReadable).skipped && decide(n5, 0x1, kReadable).flags == 0x1);
    const Page q = make_page();
    wr(q.page + kPageDisplay, 0x3000);
    const std::uint64_t n6 = make_node(r, q.entry(0));
    CHECK(!decide(n6, 0x1, kReadable).skipped);
}

// The tree update's use of the root's pending list (0x4dc0c0, then 0x4dc280):
// the list drained into the update, each node's flags taken - 0x10000 adds
// bit 0 - and its transform updated, then the pending bit cleared. Returns
// the flags each node was updated with.
std::vector<std::pair<std::uint64_t, std::uint32_t>> tree_update(const Root& r) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> out;
    std::uint64_t n = rd(r.root + kRootPending);
    wr(r.root + kRootPending, 0);
    std::vector<std::uint64_t> list;
    while (n && list.size() < 64) {
        list.push_back(n);
        n = rd(n + kNodeNext);
    }
    for (const std::uint64_t node : list) {
        std::uint32_t f = rd32(node + kNodeFlags);
        std::uint32_t pass = f & 3;
        if (f & kNodeRetransform) {
            f &= ~kNodeRetransform;
            pass |= 1;
        }
        wr32(node + kNodeFlags, f & ~std::uint32_t{3});
        out.push_back({node, pass});
    }
    for (const std::uint64_t node : list) wr32(node + kNodeFlags, rd32(node + kNodeFlags) & ~kNodePending);
    return out;
}

void test_mark_for_update() {
    const Page p = make_page();
    const Root r = make_root();
    const std::uint64_t a = make_node(r, p.entry(10)), b = make_node(r, p.entry(11));
    // Not pending: onto the root's list, with the flag and the pending bit.
    CHECK(mark_for_update(a, kNodeRetransform, kReadable));
    CHECK(rd(r.root + kRootPending) == a && rd(a + kNodeNext) == 0);
    CHECK(rd32(a + kNodeFlags) == (0x7e0000u | kNodeRetransform | kNodePending));
    CHECK(mark_for_update(b, kNodeRetransform, kReadable));
    CHECK(rd(r.root + kRootPending) == b && rd(b + kNodeNext) == a);
    // Pending already: only the flags; its link (the list it is on) kept.
    wr(a + kNodeNext, 0x1234);
    CHECK(mark_for_update(a, 0x2, kReadable));
    CHECK(rd(a + kNodeNext) == 0x1234 && rd(r.root + kRootPending) == b && (rd32(a + kNodeFlags) & 0x2) == 0x2);
    wr(a + kNodeNext, 0);
    // The next tree update gives both bit 0 and leaves them off the list.
    const auto u = tree_update(r);
    CHECK(u.size() == 2 && u[0].first == b && u[1].first == a && (u[0].second & 1) && (u[1].second & 1));
    CHECK(!(rd32(a + kNodeFlags) & (kNodePending | kNodeRetransform)) && rd(r.root + kRootPending) == 0);
    // During the root's own update nothing is written: the node would stay
    // on the list past it.
    {
        auto* in_update = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(r.root + kRootInUpdate));
        *in_update = 1;
        const std::uint32_t before = rd32(a + kNodeFlags);
        CHECK(!mark_for_update(a, kNodeRetransform, kReadable));
        CHECK(rd32(a + kNodeFlags) == before && rd(r.root + kRootPending) == 0);
        *in_update = 0;
    }
    // No root: nothing.
    const std::uint64_t c = make_node(Root{0}, p.entry(12));
    CHECK(!mark_for_update(c, kNodeRetransform, kReadable));
}

// The whole path: shown without a layout (the guard takes bit 0 out), the
// layout delivered by a later capture, the node marked, the next update runs
// with bit 0 and reads the layout.
void test_layout_arrives() {
    const Page p = make_page();
    const Root r = make_root();
    const std::uint64_t first = make_text_data(0);
    set_data(p.display, 20, first, true);
    const std::uint64_t node = make_node(r, p.entry(20));
    const Decision d = decide(node, 0x1, kReadable);
    CHECK(d.skipped && game_read(node, d.flags) == 0);
    // The capture with the layout: the display table now names data with one.
    const std::uint64_t layout = make_layout();
    const std::uint64_t second = make_text_data(layout);
    set_data(p.display, 20, second, true);
    CHECK(mark_for_update(node, kNodeRetransform, kReadable));
    const auto u = tree_update(r);
    CHECK(u.size() == 1 && u[0].first == node && (u[0].second & 1));
    const Decision again = decide(node, u[0].second, kReadable);
    CHECK(!again.skipped && game_read(node, again.flags) == layout + 0x1c);
}

// A snapshot's propagate list head (next, prev at +0x40), empty.
void list_init(std::uint64_t snap) {
    wr(snap + kSnapshotPropagate, snap + kSnapshotPropagate);
    wr(snap + kSnapshotPropagate + 8, snap + kSnapshotPropagate);
}
// addToPropagate's link (0x482a40): at the front of the snapshot's list.
void list_add(std::uint64_t snap, std::uint64_t entry) {
    const std::uint64_t head = snap + kSnapshotPropagate, link = entry + kEntryPropagate, first = rd(head);
    wr(link, first);
    wr(link + 8, head);
    wr(first + 8, link);
    wr(head, link);
}
// The entries on a snapshot's list in order, and whether every link is consistent.
std::vector<std::uint64_t> list_members(std::uint64_t snap, bool* consistent) {
    std::vector<std::uint64_t> out;
    const std::uint64_t head = snap + kSnapshotPropagate;
    std::uint64_t prev = head;
    *consistent = true;
    for (std::uint64_t n = rd(head); n != head && out.size() < 64; n = rd(n)) {
        if (rd(n + 8) != prev) *consistent = false;
        out.push_back(n - kEntryPropagate);
        prev = n;
    }
    if (rd(head + 8) != prev) *consistent = false;
    return out;
}

// A capture published with entries listed after its pass: they are on the
// published snapshot's list, which nothing drains. restrand moves them, and
// only them, to the live list without touching the old head.
void test_restrand() {
    Page p = make_page();
    const std::uint64_t live = p.snapshot;  // the snapshot being built: the pages name it
    const std::uint64_t dead = alloc(0xa0);
    list_init(live);
    list_init(dead);
    const std::uint64_t a = p.entry(30), b = p.entry(31), c = p.entry(32), d = p.entry(33);
    // One stranded alone, and a live list with one entry on it already.
    list_add(dead, a);
    list_add(live, d);
    std::uint8_t head_before[16];
    std::memcpy(head_before, reinterpret_cast<void*>(dead + kSnapshotPropagate), 16);
    CHECK(restrand(a, live, kReadable) == 1);
    bool ok = false;
    auto m = list_members(live, &ok);
    CHECK(ok && m.size() == 2 && m[0] == a && m[1] == d);
    CHECK(std::memcmp(head_before, reinterpret_cast<void*>(dead + kSnapshotPropagate), 16) == 0);
    // On the live list now: left alone.
    CHECK(restrand(a, live, kReadable) == 0);
    m = list_members(live, &ok);
    CHECK(ok && m.size() == 2);
    // Two stranded together, found from either: both move.
    const std::uint64_t dead2 = alloc(0xa0);
    list_init(dead2);
    list_add(dead2, b);
    list_add(dead2, c);  // dead2: c, b
    CHECK(restrand(b, live, kReadable) == 2);
    m = list_members(live, &ok);
    CHECK(ok && m.size() == 4);
    bool has_b = false, has_c = false;
    for (const std::uint64_t e : m) {
        has_b = has_b || e == b;
        has_c = has_c || e == c;
    }
    CHECK(has_b && has_c);
    // On no list: nothing.
    const std::uint64_t e = p.entry(34);
    CHECK(restrand(e, live, kReadable) == 0 && rd(e + kEntryPropagate) == 0);
    // Ends that are not one head, or a loop with no head: left as they are.
    const std::uint64_t f = p.entry(35), g = p.entry(36);
    const std::uint64_t h1 = alloc(0x50), h2 = alloc(0x50);
    wr(f + kEntryPropagate, h1 + kSnapshotPropagate);
    wr(f + kEntryPropagate + 8, h2 + kSnapshotPropagate);
    CHECK(restrand(f, live, kReadable) == 0);
    CHECK(rd(f + kEntryPropagate) == h1 + kSnapshotPropagate);
    wr(f + kEntryPropagate, g + kEntryPropagate);
    wr(f + kEntryPropagate + 8, g + kEntryPropagate);
    wr(g + kEntryPropagate, f + kEntryPropagate);
    wr(g + kEntryPropagate + 8, f + kEntryPropagate);
    CHECK(restrand(f, live, kReadable) == 0);
    m = list_members(live, &ok);
    CHECK(ok && m.size() == 4);
    // An entry on another context's page (its pages name another snapshot)
    // ends the walk like a head: the list is left alone.
    Page q = make_page();
    const std::uint64_t dead3 = alloc(0xa0);
    list_init(dead3);
    list_add(dead3, q.entry(0));
    list_add(dead3, p.entry(40));
    CHECK(restrand(p.entry(40), live, kReadable) == 0);
}

// A capture's unlock with entries listed after its pass on the published
// snapshot's list: destrand moves them all to the next snapshot's list and
// leaves the published one empty; a list with a stranger on it is left whole.
void test_destrand() {
    Page p = make_page();
    const std::uint64_t next = p.snapshot;  // the pages name the snapshot being built
    const std::uint64_t published = alloc(0xa0);
    list_init(next);
    list_init(published);
    CHECK(destrand(published, next, kReadable) == 0);  // nothing listed late
    const std::uint64_t a = p.entry(50), b = p.entry(51), c = p.entry(52);
    list_add(published, a);
    list_add(published, b);
    list_add(next, c);
    CHECK(destrand(published, next, kReadable) == 2);
    bool ok = false;
    auto m = list_members(next, &ok);
    CHECK(ok && m.size() == 3);
    bool has_a = false, has_b = false, has_c = false;
    for (const std::uint64_t e : m) {
        has_a = has_a || e == a;
        has_b = has_b || e == b;
        has_c = has_c || e == c;
    }
    CHECK(has_a && has_b && has_c);
    m = list_members(published, &ok);
    CHECK(ok && m.empty());
    // An entry of another context's page on the list: nothing is written.
    Page q = make_page();
    const std::uint64_t published2 = alloc(0xa0);
    list_init(published2);
    list_add(published2, p.entry(53));
    list_add(published2, q.entry(1));
    std::uint8_t before[0xa0], before_next[16];
    std::memcpy(before, reinterpret_cast<void*>(published2), sizeof(before));
    std::memcpy(before_next, reinterpret_cast<void*>(next + kSnapshotPropagate), sizeof(before_next));
    CHECK(destrand(published2, next, kReadable) == 0);
    CHECK(std::memcmp(before, reinterpret_cast<void*>(published2), sizeof(before)) == 0);
    CHECK(std::memcmp(before_next, reinterpret_cast<void*>(next + kSnapshotPropagate), sizeof(before_next)) == 0);
    m = list_members(next, &ok);
    CHECK(ok && m.size() == 3);
    // The same snapshot twice, or none: nothing.
    CHECK(destrand(next, next, kReadable) == 0 && destrand(0, next, kReadable) == 0);
}

// The movie's Advance job takes the wrapper's lock around its Advance and
// Capture; a stand-in holds it across a "capture" window again and again
// while the HUD's update stand-in changes the movie through run_locked. No
// change may land inside a window - one run bare (the game's way) lands in
// them. Then the lock already held, and a lock busy past the wait.
void test_run_locked() {
    void* m = nullptr;
    CHECK(gsync::mutex_init(&m, gsync::kMutexRecursive) == 0);
    std::atomic<int> in_capture{0}, inside{0}, done{0};
    std::thread advance([&] {
        for (int i = 0; i < 300; ++i) {
            gsync::mutex_lock(&m, nullptr);
            in_capture.store(1);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            in_capture.store(0);
            gsync::mutex_unlock(&m);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        done.store(1);
    });
    int runs = 0, locked = 0;
    while (!done.load()) {
        std::uint64_t us = 0;
        const LockRun how = run_locked(&m, 100000, &us, [&] {
            if (in_capture.load()) inside.fetch_add(1);
        });
        locked += how == LockRun::kLocked;
        ++runs;
    }
    advance.join();
    CHECK(inside.load() == 0);
    CHECK(runs > 0 && locked == runs);
    CHECK(gsync::mutex_owner(&m) == nullptr);

    // Held by this thread already: runs as is and leaves it held.
    CHECK(gsync::mutex_lock(&m, nullptr) == 0);
    std::uint64_t us = 0;
    bool ran = false;
    CHECK(run_locked(&m, 1000, &us, [&] { ran = true; }) == LockRun::kAlreadyHeld);
    CHECK(ran && us == 0);
    CHECK(gsync::mutex_owner(&m) == gsync::current_thread());
    CHECK(gsync::mutex_unlock(&m) == 0);
    CHECK(gsync::mutex_owner(&m) == nullptr);

    // Busy past the wait: runs without it, and says so.
    std::atomic<int> held{0};
    std::thread holder([&] {
        gsync::mutex_lock(&m, nullptr);
        held.store(1);
        while (held.load() != 2) std::this_thread::yield();
        gsync::mutex_unlock(&m);
    });
    while (held.load() != 1) std::this_thread::yield();
    ran = false;
    CHECK(run_locked(&m, 2000, &us, [&] { ran = true; }) == LockRun::kTimedOut);
    CHECK(ran && us >= 2000);
    CHECK(gsync::mutex_owner(&m) != gsync::current_thread());
    held.store(2);
    holder.join();
    CHECK(gsync::mutex_destroy(&m) == 0);
}

}  // namespace

int main() {
    test_entry_index();
    test_entry_data();
    test_crash_sequence();
    test_left_alone();
    test_mark_for_update();
    test_layout_arrives();
    test_restrand();
    test_destrand();
    test_run_locked();
    if (g_failed) {
        std::printf("sf_text_layout_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("sf_text_layout_test: ok\n");
    return 0;
}
