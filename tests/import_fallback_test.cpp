// What serves a draw's vertex record or index range that lies in no
// guest-memory import (host/import_fallback.h): the mirrors first, then a
// copy into the snapshot ring up to its largest copy, else nothing - and why.
#include "host/import_fallback.h"

#include <cstdio>

namespace {

int g_fail = 0;
#define CHECK(c)                                                      \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                 \
        }                                                             \
    } while (0)

using namespace import_fallback;

constexpr std::uint64_t kMiB = 1ull << 20;

void test_choose() {
    // Off: nothing, whatever else holds.
    CHECK(choose(false, true, 4096, kMiB, false, true) == kOff);
    CHECK(choose(false, false, 4096, kMiB, false, true) == kOff);
    // The mirrors served it: they win over a copy, a pending write (they refuse one themselves) and size.
    CHECK(choose(true, true, 4096, kMiB, false, true) == kMirror);
    CHECK(choose(true, true, 64 * kMiB, kMiB, false, false) == kMirror);
    // A copy: small enough, no GPU write to land, a ring.
    CHECK(choose(true, false, 4096, kMiB, false, true) == kCopy);
    CHECK(choose(true, false, kMiB, kMiB, false, true) == kCopy);  // exactly the ring's largest copy
    CHECK(choose(true, false, 1, kMiB, false, true) == kCopy);
    // Past the largest copy, or nothing to copy.
    CHECK(choose(true, false, kMiB + 1, kMiB, false, true) == kTooBig);
    CHECK(choose(true, false, 0, kMiB, false, true) == kTooBig);
    // A GPU write still to land: a copy now would miss it, whatever the size or the ring.
    CHECK(choose(true, false, 4096, kMiB, true, true) == kGpuPending);
    CHECK(choose(true, false, 64 * kMiB, kMiB, true, true) == kGpuPending);
    // No ring: nothing.
    CHECK(choose(true, false, 4096, kMiB, false, false) == kNoRing);
    // Too big comes before the ring: no ring is asked of for a range it cannot take.
    CHECK(choose(true, false, kMiB + 1, kMiB, false, false) == kTooBig);
}

void test_served() {
    CHECK(served(kMirror));
    CHECK(served(kCopy));
    CHECK(!served(kOff));
    CHECK(!served(kTooBig));
    CHECK(!served(kGpuPending));
    CHECK(!served(kNoRing));
    // Every choice is one of the counted ones.
    for (int c = 0; c < kChoices; ++c) CHECK(c >= kMirror && c < kChoices);
}

}  // namespace

int main() {
    test_choose();
    test_served();
    if (g_fail) {
        std::printf("import_fallback_test: %d failed\n", g_fail);
        return 1;
    }
    std::printf("import_fallback_test: ok\n");
    return 0;
}
