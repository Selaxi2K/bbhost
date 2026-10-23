// The Scaleform text-layout guard (engine/sf_text_layout.h) without the game:
// a render context's entry page, the tables of the snapshot on display and of
// the one being built, text node data and a text cache node laid out as
// eboot 1.09 lays them out, then the read the game's TreeCacheText transform
// update makes at 0x52d68d-0x52d6c0 - the null layout's +0x1c, the fault on
// the Steam Deck - with the guard and without it; the marking that brings the
// layout's part back once a capture delivers the layout, and the tree
// update's walk over the root's pending list that turns it into the flag; and
// the cases the guard must leave alone.
#include "engine/sf_text_layout.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

}  // namespace

int main() {
    test_entry_index();
    test_entry_data();
    test_crash_sequence();
    test_left_alone();
    test_mark_for_update();
    test_layout_arrives();
    if (g_failed) {
        std::printf("sf_text_layout_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("sf_text_layout_test: ok\n");
    return 0;
}
