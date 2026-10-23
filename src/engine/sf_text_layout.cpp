#include "guest_abi.h"
#include "engine/sf_text_layout.h"

#include "core/elf.h"
#include "core/portable.h"
#include "engine/addr.h"
#include "engine/graphics_patch.h"
#include "hle/modules.h"
#include "hle/sync.h"
#include "log.h"

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using namespace sf_text_layout;

// Scaleform 4 in eboot 1.09 (Binary Ninja addresses).
// TreeCacheText::UpdateTransform(node, root context, data, TransformArgs*, flags).
constexpr std::uint64_t kUpdateTransform = 0x52d5e0;
constexpr std::uint8_t kUpdateTransformPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};
// TreeCacheText::HandleChanges(node, root context, change flags): 0x400, the
// text's layout changed.
constexpr std::uint64_t kHandleChanges = 0x52df80;
constexpr std::uint8_t kHandleChangesPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                   0x53, 0x50, 0x89, 0xd3, 0x49, 0x89, 0xf6};
constexpr std::uint32_t kChangeTextLayout = 0x400;
// The render context's changes: getWritableData(entry, change flags),
// CreateEntry(context, data), addToPropagate(entry).
constexpr std::uint64_t kGetWritable = 0x482a70;
constexpr std::uint8_t kGetWritablePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::uint64_t kCreateEntry = 0x4848d0;
constexpr std::uint8_t kCreateEntryPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::uint64_t kAddToPropagate = 0x482a40;
constexpr std::uint8_t kAddToPropagatePrologue[] = {0x48, 0x89, 0xf8, 0x48, 0x25, 0x00, 0xf0, 0xff, 0xff,
                                                    0x48, 0x8b, 0x40, 0x18, 0x48, 0x8d, 0x48, 0x40};
// The movie wrapper's jobs: Advance and Capture (0x2358e50) and NextCapture
// and the tree update (0x2359150), each wrapper in rdi; its movie at +0x18,
// its lock's mutex at +0x40.
constexpr std::uint64_t kAdvanceJob = 0x2358e50;
constexpr std::uint8_t kAdvanceJobPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};
constexpr std::uint64_t kDisplayJob = 0x2359150;
constexpr std::uint8_t kDisplayJobPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::uint64_t kWrapperMovie = 0x18;
constexpr std::uint64_t kWrapperMutex = 0x40;
// Context::Capture (0x484c20): its call of the propagate pass, its call of
// pthread_mutex_lock right after that pass, and its call of
// pthread_mutex_unlock once the snapshot is published.
constexpr std::uint64_t kCapturePropagateCall = 0x484c42;
constexpr std::uint64_t kPropagate = 0x485030;
constexpr std::uint64_t kCaptureLockCall = 0x484c64;
constexpr std::uint64_t kPthreadMutexLockStub = 0x2fbe598;
constexpr std::uint64_t kCaptureUnlockCall = 0x484ffe;
constexpr std::uint64_t kPthreadMutexUnlockStub = 0x2fbe5a8;
constexpr std::uint64_t kTextDataVtable = 0x55447b0;  // TreeText::NodeData
constexpr std::uint64_t kTextLo = 0x400000, kTextHi = 0x54d96dc;

std::uint64_t g_slide = 0;
bool g_guard = false, g_check = false;
std::uint64_t g_capture_us = 0;

std::uint64_t va(std::uint64_t b) { return g_slide + (b - kPreferredGuestSlide); }
std::uint64_t bn(std::uint64_t a) { return a - g_slide + kPreferredGuestSlide; }
bool text(std::uint64_t a) { return a >= va(kTextLo) && a < va(kTextHi); }
bool readable(std::uint64_t a, std::size_t n) { return a >= 0x10000 && hle_kernel_va_readable(a, n); }
// For what the game itself reads next: the guard follows the same pointers
// the update is about to follow, so it checks nothing the game does not.
auto kGameReads = [](std::uint64_t a, std::size_t) { return a >= 0x10000; };

void thread_name(char* out, std::size_t n) {
    out[0] = 0;
#if !defined(_WIN32)
    pthread_getname_np(pthread_self(), out, n);
#else
    (void)n;
#endif
}

bool log_this(std::uint64_t n) { return n <= 16 || (n & (n - 1)) == 0; }

// Up to `n` eboot code addresses: the hooked function's return address
// (saved[6]: the prologue stub pushed six registers over it), then the first
// code addresses above it on the guest stack.
void callers(const std::uint64_t* saved, std::uint64_t* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = 0;
    out[0] = saved[6];
    int k = 1;
    for (int i = 7; i < 7 + 48 && k < n; ++i) {
        if (text(saved[i])) out[k++] = saved[i];
    }
}

std::string callers_text(const std::uint64_t* saved) {
    std::uint64_t r[8];
    callers(saved, r, 8);
    std::string s;
    char b[24];
    for (const std::uint64_t a : r) {
        if (!a) break;
        std::snprintf(b, sizeof(b), "%s%llx", s.empty() ? "" : " ", static_cast<unsigned long long>(text(a) ? bn(a) : a));
        s += b;
    }
    return s;
}

// --- the guard ------------------------------------------------------------------
std::atomic<std::uint64_t> g_skipped{0}, g_marked{0}, g_unmarked{0};
// Text cache nodes whose layout part was taken out, until their layout
// arrives. Only compared with the node HandleChanges is given (a live one),
// never followed, so a node destroyed meanwhile costs nothing; one made at the
// same address later is marked once for nothing.
constexpr int kWaiting = 64;
std::mutex g_waiting_mu;
std::uint64_t g_waiting[kWaiting];  // under g_waiting_mu
std::atomic<int> g_waiting_n{0};

void wait_for_layout(std::uint64_t node) {
    std::lock_guard<std::mutex> lk(g_waiting_mu);
    int free_slot = -1;
    for (int i = 0; i < kWaiting; ++i) {
        if (g_waiting[i] == node) return;
        if (!g_waiting[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        free_slot = static_cast<int>(g_skipped.load(std::memory_order_relaxed) % kWaiting);  // full: one goes
    } else {
        g_waiting_n.fetch_add(1, std::memory_order_relaxed);
    }
    g_waiting[free_slot] = node;
}

bool take_waiting(std::uint64_t node) {
    std::lock_guard<std::mutex> lk(g_waiting_mu);
    for (int i = 0; i < kWaiting; ++i) {
        if (g_waiting[i] == node) {
            g_waiting[i] = 0;
            g_waiting_n.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

GUEST_ABI std::int64_t update_transform_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t node = saved[5];  // rdi
    const std::uint32_t flags = static_cast<std::uint32_t>(saved[1]);  // r8d
    const Decision d = decide(node, flags, kGameReads);
    if (!d.skipped) return 0;
    saved[1] = (saved[1] & ~std::uint64_t{0xffffffff}) | d.flags;
    wait_for_layout(node);
    const std::uint64_t n = g_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (log_this(n)) {
        const std::uint64_t entry = rd(node + kNodeEntry);
        const std::uint64_t next = entry_data(entry, kPageActive, readable);
        const bool next_has = next && readable(next + kTextLayout, 8) && rd(next + kTextLayout) != 0;
        char tn[32];
        thread_name(tn, sizeof(tn));
        host_log("sf text: a text is shown before its layout exists (cache node %llx, entry %llx, data %llx%s: document "
                 "view %llx, layout-changed %u; the snapshot being built %s) - its transform update runs without the "
                 "layout's part, done again once the layout arrives (on %s, flags %x, callers %s; %llu so far)",
                 static_cast<unsigned long long>(node), static_cast<unsigned long long>(entry),
                 static_cast<unsigned long long>(d.data), rd(d.data) == va(kTextDataVtable) ? "" : " - not a TreeText's",
                 static_cast<unsigned long long>(rd(d.data + kTextDocView)), rd8(d.data + kTextLayoutChanged) & 1u,
                 next == d.data ? "shares that data" : next_has ? "has a layout" : "has none either", tn, flags,
                 callers_text(saved).c_str(), static_cast<unsigned long long>(n));
    }
    return 0;
}

GUEST_ABI std::int64_t handle_changes_hook(std::uint64_t, std::uint64_t* saved) {
    if (!(saved[3] & kChangeTextLayout) || g_waiting_n.load(std::memory_order_relaxed) == 0) return 0;  // edx
    const std::uint64_t node = saved[5];
    const std::uint64_t data = entry_data(rd(node + kNodeEntry), kPageDisplay, kGameReads);
    if (!data || rd(data + kTextLayout) == 0 || !take_waiting(node)) return 0;
    // Its layout is on display now: the part the guard took out runs in this
    // capture's tree update, as for a node whose transform changed.
    const bool marked = mark_for_update(node, kNodeRetransform, kGameReads);
    const std::uint64_t n = (marked ? g_marked : g_unmarked).fetch_add(1, std::memory_order_relaxed) + 1;
    if (log_this(n)) {
        host_log("sf text: cache node %llx has its layout now - %s (%llu so far)", static_cast<unsigned long long>(node),
                 marked ? "marked for its transform update" : "not marked (no root, or its update under way)",
                 static_cast<unsigned long long>(n));
    }
    return 0;
}

// --- the detector ---------------------------------------------------------------
// The movie wrappers seen, by movie: their jobs name them, and say which
// thread last ran each job and which thread is inside the movie's capture.
constexpr int kWrappers = 64;
struct Wrapper {
    std::atomic<std::uint64_t> movie, wrapper;
    std::atomic<gsync::Thread*> advance, display, capturing;
};
Wrapper g_wrap[kWrappers];

Wrapper* find_movie(std::uint64_t movie) {
    for (Wrapper& w : g_wrap) {
        if (w.movie.load(std::memory_order_acquire) == movie) return &w;
    }
    return nullptr;
}

Wrapper* note_wrapper(std::uint64_t w) {
    if (!readable(w + kWrapperMovie, 8)) return nullptr;
    const std::uint64_t movie = rd(w + kWrapperMovie);
    if (!movie) return nullptr;
    if (Wrapper* e = find_movie(movie)) {
        if (e->wrapper.load(std::memory_order_relaxed) != w) e->wrapper.store(w, std::memory_order_release);
        return e;
    }
    for (Wrapper& e : g_wrap) {
        std::uint64_t expected = 0;
        if (e.movie.load(std::memory_order_relaxed) == 0 && e.movie.compare_exchange_strong(expected, movie)) {
            e.wrapper.store(w, std::memory_order_release);
            return &e;
        }
    }
    return nullptr;
}

// The wrapper of a context, checked live: a wrapper's destructor clears its
// movie first.
Wrapper* wrapper_of(std::uint64_t ctx, std::uint64_t* w_out) {
    Wrapper* e = find_movie(ctx - kMovieContext);
    if (!e) return nullptr;
    const std::uint64_t w = e->wrapper.load(std::memory_order_acquire);
    if (!w || !readable(w + kWrapperMovie, 8) || rd(w + kWrapperMovie) != ctx - kMovieContext) return nullptr;
    *w_out = w;
    return e;
}

GUEST_ABI std::int64_t advance_job_hook(std::uint64_t, std::uint64_t* saved) {
    if (Wrapper* e = note_wrapper(saved[5])) e->advance.store(gsync::current_thread(), std::memory_order_release);
    return 0;
}

GUEST_ABI std::int64_t display_job_hook(std::uint64_t, std::uint64_t* saved) {
    if (Wrapper* e = note_wrapper(saved[5])) e->display.store(gsync::current_thread(), std::memory_order_release);
    return 0;
}

// The capture's window, from its propagate pass to its unlock.
thread_local Wrapper* t_capturing = nullptr;
GUEST_ABI std::int64_t capture_begin_hook(std::uint64_t, std::uint64_t* saved) {
    Wrapper* e = find_movie(saved[5] - kMovieContext);  // rdi: the context
    if (e) e->capturing.store(gsync::current_thread(), std::memory_order_release);
    t_capturing = e;
    return 0;  // on to the propagate pass
}

GUEST_ABI std::int64_t capture_end_hook(std::uint64_t, std::uint64_t*) {
    if (t_capturing) t_capturing->capturing.store(nullptr, std::memory_order_release);
    t_capturing = nullptr;
    return 0;  // on to pthread_mutex_unlock
}

enum Class { kInside, kInCapture, kInAdvance, kBesideDisplay, kHeldOther, kOutside, kUnknown, kClasses };
const char* const kClassText[kClasses] = {
    "under the movie's lock",
    "while another thread was in the movie's capture",
    "while the movie's Advance job held its lock",
    "while its NextCapture job held its lock",
    "while another thread held the movie's lock",
    "with the movie's lock free",
    "on a context no movie wrapper was seen for",
};
std::atomic<std::uint64_t> g_class[kClasses];
// Per thread name (as the thread had it at its first change), the changes in
// each class. A row is never moved, so each thread keeps a pointer to its own.
constexpr int kThreads = 32;
std::mutex g_threads_mu;
struct ThreadCount {
    char name[16];
    std::atomic<std::uint64_t> n[kClasses];
};
ThreadCount g_threads[kThreads];
std::atomic<int> g_threads_n{0};
thread_local ThreadCount* t_count = nullptr;

void count_thread(Class c) {
    if (!t_count) {
        char tn[16];
        thread_name(tn, sizeof(tn));
        tn[sizeof(tn) - 1] = 0;
        std::lock_guard<std::mutex> lk(g_threads_mu);
        const int n = g_threads_n.load(std::memory_order_relaxed);
        for (int i = 0; i < n && !t_count; ++i) {
            if (std::strncmp(g_threads[i].name, tn, sizeof(tn)) == 0) t_count = &g_threads[i];
        }
        if (!t_count && n < kThreads) {
            std::memcpy(g_threads[n].name, tn, sizeof(tn));
            t_count = &g_threads[n];
            g_threads_n.store(n + 1, std::memory_order_release);
        }
        if (!t_count) return;
    }
    t_count->n[c].fetch_add(1, std::memory_order_relaxed);
}

void classify(const char* what, std::uint64_t ctx, std::uint64_t entry, std::uint64_t arg, const std::uint64_t* saved) {
    Class c = kUnknown;
    std::uint64_t w = 0;
    Wrapper* e = ctx ? wrapper_of(ctx, &w) : nullptr;
    if (e && readable(w + kWrapperMutex, 8)) {
        gsync::Thread* const self = gsync::current_thread();
        gsync::Thread* const owner =
            gsync::mutex_owner(reinterpret_cast<void**>(static_cast<std::uintptr_t>(w + kWrapperMutex)));
        gsync::Thread* const cap = e->capturing.load(std::memory_order_acquire);
        if (owner == self) c = kInside;
        else if (cap && cap != self) c = kInCapture;
        else if (!owner) c = kOutside;
        else if (owner == e->advance.load(std::memory_order_acquire)) c = kInAdvance;
        else if (owner == e->display.load(std::memory_order_acquire)) c = kBesideDisplay;
        else c = kHeldOther;
    }
    const std::uint64_t n = g_class[c].fetch_add(1, std::memory_order_relaxed) + 1;
    count_thread(c);
    if (c == kInside || c == kUnknown || !log_this(n)) return;
    char tn[32];
    thread_name(tn, sizeof(tn));
    const std::uint64_t data = entry ? entry_data(entry, kPageActive, readable) : arg;
    const bool is_text = data && readable(data, 8) && rd(data) == va(kTextDataVtable);
    host_log("sf text: %s %s (entry %llx%s, %llx) on %s, movie %llx, wrapper %llx, callers %s (%llu so far)", what,
             kClassText[c], static_cast<unsigned long long>(entry), is_text ? ", a text's" : "",
             static_cast<unsigned long long>(arg), tn, static_cast<unsigned long long>(ctx - kMovieContext),
             static_cast<unsigned long long>(w), callers_text(saved).c_str(), static_cast<unsigned long long>(n));
}

GUEST_ABI std::int64_t get_writable_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t entry = saved[5];
    classify("a change", entry_context(entry, readable), entry, saved[4] & 0xffffffff, saved);
    return 0;
}

GUEST_ABI std::int64_t create_entry_hook(std::uint64_t, std::uint64_t* saved) {
    classify("a new entry", saved[5], 0, saved[4], saved);
    return 0;
}

GUEST_ABI std::int64_t add_to_propagate_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t entry = saved[5];
    classify("a propagate request", entry_context(entry, readable), entry, 0, saved);
    return 0;
}

// BBHOST_TEST_SF_CAPTURE_US: the capture's propagate pass is over and its lock
// not yet taken.
std::atomic<std::uint64_t> g_capture_sleeps{0};
GUEST_ABI std::int64_t capture_lock_hook(std::uint64_t, std::uint64_t*) {
    g_capture_sleeps.fetch_add(1, std::memory_order_relaxed);
    host_sleep_us(g_capture_us);
    return 0;  // on to pthread_mutex_lock
}

bool hook(ElfImage* image, std::uint64_t at, const std::uint8_t* pro, std::size_t n, void* fn, const char* what) {
    const bool ok = engine_prologue_hook(image, va(at), pro, n, fn);
    if (!ok) host_log("sf text: %s (0x%llx) NOT hooked (unexpected bytes)", what, static_cast<unsigned long long>(at));
    return ok;
}

bool call_hook(ElfImage* image, std::uint64_t at, std::uint64_t target, void* fn, const char* what) {
    const bool ok = engine_call_site_hook(image, va(at), va(target), fn);
    if (!ok) host_log("sf text: %s (0x%llx) NOT hooked (unexpected bytes)", what, static_cast<unsigned long long>(at));
    return ok;
}

}  // namespace

void sf_text_layout_install(ElfImage* image) {
    const char* g = std::getenv("BBHOST_SF_TEXT_GUARD");
    const char* c = std::getenv("BBHOST_SF_TEXT_CHECK");
    const char* t = std::getenv("BBHOST_TEST_SF_CAPTURE_US");
    g_guard = !(g && *g == '0');
    g_check = c && *c == '1';
    g_capture_us = t ? std::strtoull(t, nullptr, 10) : 0;
    g_slide = image->mem.slide;
    bool ok = true;
    if (g_guard) {
        ok = hook(image, kUpdateTransform, kUpdateTransformPrologue, sizeof(kUpdateTransformPrologue),
                  reinterpret_cast<void*>(&update_transform_hook), "TreeCacheText::UpdateTransform") && ok;
        ok = hook(image, kHandleChanges, kHandleChangesPrologue, sizeof(kHandleChangesPrologue),
                  reinterpret_cast<void*>(&handle_changes_hook), "TreeCacheText::HandleChanges") && ok;
    }
    if (g_check) {
        ok = hook(image, kAdvanceJob, kAdvanceJobPrologue, sizeof(kAdvanceJobPrologue),
                  reinterpret_cast<void*>(&advance_job_hook), "the movie's Advance job") && ok;
        ok = hook(image, kDisplayJob, kDisplayJobPrologue, sizeof(kDisplayJobPrologue),
                  reinterpret_cast<void*>(&display_job_hook), "the movie's NextCapture job") && ok;
        ok = hook(image, kGetWritable, kGetWritablePrologue, sizeof(kGetWritablePrologue),
                  reinterpret_cast<void*>(&get_writable_hook), "Context getWritableData") && ok;
        ok = hook(image, kCreateEntry, kCreateEntryPrologue, sizeof(kCreateEntryPrologue),
                  reinterpret_cast<void*>(&create_entry_hook), "Context::CreateEntry") && ok;
        ok = hook(image, kAddToPropagate, kAddToPropagatePrologue, sizeof(kAddToPropagatePrologue),
                  reinterpret_cast<void*>(&add_to_propagate_hook), "Entry addToPropagate") && ok;
        ok = call_hook(image, kCapturePropagateCall, kPropagate, reinterpret_cast<void*>(&capture_begin_hook),
                       "Context::Capture's propagate pass") && ok;
        ok = call_hook(image, kCaptureUnlockCall, kPthreadMutexUnlockStub, reinterpret_cast<void*>(&capture_end_hook),
                       "Context::Capture's unlock") && ok;
    }
    if (g_capture_us) {
        ok = call_hook(image, kCaptureLockCall, kPthreadMutexLockStub, reinterpret_cast<void*>(&capture_lock_hook),
                       "Context::Capture's lock") && ok;
    }
    if (!g_guard && !g_check && !g_capture_us) {
        host_log("sf text: the text-layout guard is off (BBHOST_SF_TEXT_GUARD=0)");
        return;
    }
    const std::string wait = g_capture_us ? ", every capture waits " + std::to_string(g_capture_us) +
                                                " us after its propagate pass (BBHOST_TEST_SF_CAPTURE_US)"
                                          : std::string();
    host_log("sf text: text-layout guard %s%s%s%s", g_guard ? "on" : "off (BBHOST_SF_TEXT_GUARD=0)",
             g_check ? ", detector on (BBHOST_SF_TEXT_CHECK=1: render-tree changes by the movie's lock)" : "",
             wait.c_str(), ok ? "" : " - a hook was not placed");
}

void sf_text_layout_report() {
    if (g_guard) {
        host_log("sf text: %llu text update(s) ran without their layout's part; %llu node(s) marked again when it arrived, "
                 "%llu not marked, %d still waiting",
                 static_cast<unsigned long long>(g_skipped.load()), static_cast<unsigned long long>(g_marked.load()),
                 static_cast<unsigned long long>(g_unmarked.load()), g_waiting_n.load());
    }
    if (g_capture_us) {
        host_log("sf text: %llu capture(s) waited %llu us after their propagate pass",
                 static_cast<unsigned long long>(g_capture_sleeps.load()), static_cast<unsigned long long>(g_capture_us));
    }
    if (!g_check) return;
    for (int c = 0; c < kClasses; ++c) {
        host_log("sf text: render-tree changes %s: %llu", kClassText[c], static_cast<unsigned long long>(g_class[c].load()));
    }
    // No allocation: the report also runs from the crash handler.
    const int n = g_threads_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const ThreadCount& t = g_threads[i];
        char row[256];
        int len = 0;
        for (int c = 0; c < kClasses && len >= 0 && len < static_cast<int>(sizeof(row)); ++c) {
            len += std::snprintf(row + len, sizeof(row) - static_cast<std::size_t>(len), "%s%llu", c ? " / " : "",
                                 static_cast<unsigned long long>(t.n[c].load()));
        }
        host_log("sf text:   %-15s %s (the classes above, in order)", t.name, row);
    }
}
