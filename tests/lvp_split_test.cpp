// Which draws start a command buffer of their own on lavapipe (host/lvp_split.h):
// a bound colour target with an empty write mask, and blending into a target the
// draw writes. Every combination of bound, write mask (empty, partial, full) and
// blend over three slots against the rule written out, the game's own cases by
// name, then four mutants, each of which a check here must catch.
#include "host/lvp_split.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using Rule = bool (*)(const bool[8], const std::uint32_t[8], const bool[8]);

// The rule written out: some bound target has no channel written, and some
// bound target with a channel written blends.
bool expected(const bool bound[8], const std::uint32_t mask[8], const bool blend[8]) {
    bool masked = false, blending = false;
    for (int t = 0; t < 8; ++t) {
        masked = masked || (bound[t] && (mask[t] & 0xf) == 0);
        blending = blending || (bound[t] && (mask[t] & 0xf) != 0 && blend[t]);
    }
    return masked && blending;
}

const std::uint32_t kMasks[3] = {0x0, 0x7, 0xf};

// 3 slots, each unbound or bound with one of three masks, blending or not:
// 7^3 combinations; slots 3-7 unbound (their masks and blends set to what
// would count, so a rule that reads unbound slots is caught).
int run_checks(Rule rule, bool report) {
    int wrong = 0, n = 0;
    for (int a = 0; a < 7; ++a) {
        for (int b = 0; b < 7; ++b) {
            for (int c = 0; c < 7; ++c) {
                const int code[3] = {a, b, c};
                bool bound[8] = {}, blend[8] = {};
                std::uint32_t mask[8] = {};
                for (int t = 0; t < 3; ++t) {
                    bound[t] = code[t] != 6;
                    mask[t] = code[t] == 6 ? 0u : kMasks[code[t] % 3];
                    blend[t] = code[t] != 6 && code[t] >= 3;
                }
                for (int t = 3; t < 8; ++t) {
                    mask[t] = 0;      // unbound: an empty mask that must not count as a masked target
                    blend[t] = true;  // nor as blending
                }
                ++n;
                const bool got = rule(bound, mask, blend), want = expected(bound, mask, blend);
                if (got != want) {
                    ++wrong;
                    if (report && wrong <= 6) std::fprintf(stderr, "  wrong: slots %d %d %d -> %d, want %d\n", a, b, c, got, want);
                }
            }
        }
    }
    if (report) std::printf("lvp_split_test: %d combinations, %d wrong\n", n, wrong);
    return wrong;
}

// Mutants: blending counted on the masked target too, unbound slots counted,
// a masked target alone enough, blending alone enough.
bool mutant_masked_blend_counts(const bool bound[8], const std::uint32_t mask[8], const bool blend[8]) {
    bool masked = false, blending = false;
    for (int t = 0; t < 8; ++t) {
        if (!bound[t]) continue;
        if (!(mask[t] & 0xf)) masked = true;
        if (blend[t]) blending = true;
    }
    return masked && blending;
}
bool mutant_unbound_counts(const bool[8], const std::uint32_t mask[8], const bool blend[8]) {
    const bool all[8] = {true, true, true, true, true, true, true, true};
    return gpu::lvp_split_wanted(all, mask, blend);
}
bool mutant_masked_alone(const bool bound[8], const std::uint32_t mask[8], const bool[8]) {
    for (int t = 0; t < 8; ++t) {
        if (bound[t] && !(mask[t] & 0xf)) return true;
    }
    return false;
}
bool mutant_blending_alone(const bool bound[8], const std::uint32_t mask[8], const bool blend[8]) {
    for (int t = 0; t < 8; ++t) {
        if (bound[t] && (mask[t] & 0xf) && blend[t]) return true;
    }
    return false;
}

}  // namespace

int main() {
    CHECK(run_checks(&gpu::lvp_split_wanted, true) == 0);

    // The game's draws by name. The G-buffer's puddle decals: seven targets
    // bound, six written (RGB) and blended, the seventh not exported.
    {
        bool bound[8] = {true, true, true, true, true, true, true, false}, blend[8] = {true, true, true, true, true, true, false, false};
        std::uint32_t mask[8] = {7, 7, 7, 7, 7, 7, 0, 0};
        CHECK(gpu::lvp_split_wanted(bound, mask, blend));
        // The same decals' opaque variant and the G-buffer's geometry: six written, none blended.
        bool opaque[8] = {};
        std::uint32_t full[8] = {0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0, 0};
        CHECK(!gpu::lvp_split_wanted(bound, full, opaque));
    }
    // The effects' soft sprites: two targets, one written and blended.
    {
        bool bound[8] = {true, true}, blend[8] = {true, true};
        std::uint32_t mask[8] = {0xf, 0};
        CHECK(gpu::lvp_split_wanted(bound, mask, blend));
    }
    // A gap below the last bound slot (no target) is not a masked target.
    {
        bool bound[8] = {true, false, true}, blend[8] = {true, false, true};
        std::uint32_t mask[8] = {0xf, 0, 0xf};
        CHECK(!gpu::lvp_split_wanted(bound, mask, blend));
    }

    struct Mutant {
        const char* name;
        Rule rule;
    } mutants[] = {{"blending counted on the masked target", &mutant_masked_blend_counts},
                   {"unbound slots counted", &mutant_unbound_counts},
                   {"a masked target alone", &mutant_masked_alone},
                   {"blending alone", &mutant_blending_alone}};
    for (const Mutant& m : mutants) {
        const int caught = run_checks(m.rule, false);
        std::printf("lvp_split_test: mutant '%s': %d combinations wrong%s\n", m.name, caught, caught ? " (caught)" : " - NOT CAUGHT");
        CHECK(caught > 0);
    }
    if (g_failures) {
        std::fprintf(stderr, "lvp_split_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lvp_split_test: ok\n");
    return 0;
}
