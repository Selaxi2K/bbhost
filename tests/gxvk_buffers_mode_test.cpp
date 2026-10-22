// BBHOST_GXVK_BUFFERS (host/gxvk.h, buffers_mode_for): unset or empty, GX's
// buffers are bound by handle except on a device whose draws read them in
// place (an integrated GPU, tight memory), which keeps today's path; a value
// given holds on every device - 0 today's path, 1 by handle, 2 compared,
// "census" (the census alone) today's path, and anything else today's path,
// as before the default changed.
#include "host/gxvk.h"

#include <cstdio>

int main() {
    using gxvk::Buffers;
    struct Case {
        const char* env;
        bool reads_in_place;
        Buffers want;
    };
    const Case cases[] = {
        {nullptr, false, Buffers::kOn},    {nullptr, true, Buffers::kOff},     {"", false, Buffers::kOn},
        {"", true, Buffers::kOff},         {"0", false, Buffers::kOff},        {"0", true, Buffers::kOff},
        {"1", false, Buffers::kOn},        {"1", true, Buffers::kOn},          {"2", false, Buffers::kCompare},
        {"2", true, Buffers::kCompare},    {"census", false, Buffers::kOff},   {"census", true, Buffers::kOff},
        {"off", false, Buffers::kOff},     {"no", true, Buffers::kOff},
    };
    int failed = 0;
    for (const Case& c : cases) {
        const Buffers got = gxvk::buffers_mode_for(c.env, c.reads_in_place);
        if (got != c.want) {
            std::printf("FAIL: BBHOST_GXVK_BUFFERS=%s on a device that %s: mode %d, wanted %d\n", c.env ? c.env : "(unset)",
                        c.reads_in_place ? "reads in place" : "does not read in place", static_cast<int>(got), static_cast<int>(c.want));
            ++failed;
        }
    }
    std::printf(failed ? "gxvk_buffers_mode_test: %d failed\n" : "gxvk_buffers_mode_test: ok (%d cases)\n",
                failed ? failed : static_cast<int>(sizeof(cases) / sizeof(cases[0])));
    return failed ? 1 : 0;
}
