#include "guest_abi.h"
#include "engine/sf_text_layout.h"

#include "core/elf.h"
#include "core/memory.h"
#include "core/portable.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "engine/graphics_patch.h"
#include "hle/hle.h"
#include "hle/modules.h"
#include "hle/sync.h"
#include "log.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
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

// The guest's own frames above a change, by its frame-pointer chain. The
// hooked function has not run its push of rbp yet, so rbp is still its
// caller's frame; the prologue stub leaves rbp alone and hle_thunk_common
// pushes it first, under the stub's six pushes, 136 bytes of vector saves and
// the return address of its call: 19 words below `saved`. The chain stays on
// the thread's own stack (the host part runs on another), checked against its
// bounds; a frameless function on the way only drops out of the list.
constexpr int kGuestRbpWord = -19;

bool guest_stack(std::uint64_t* lo, std::uint64_t* hi) {
    thread_local std::uint64_t t_lo = 0, t_hi = 0;
    thread_local bool t_known = false;
    if (!t_known) {
        t_known = true;
#if !defined(_WIN32)
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void* base = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&a, &base, &size) == 0) {
                t_lo = reinterpret_cast<std::uint64_t>(base);
                t_hi = t_lo + size;
            }
            pthread_attr_destroy(&a);
        }
#endif
    }
    *lo = t_lo;
    *hi = t_hi;
    return t_hi != 0;
}

// Up to `cap` return addresses into the eboot, innermost first. The boot
// thread runs the guest on a stack of its own (hle_set_guest_stack).
int guest_frames(const std::uint64_t* saved, std::uint64_t* out, int cap) {
    std::uint64_t lo = 0, hi = 0;
    std::uint64_t fp = saved[kGuestRbpWord];
    std::uint64_t blo = 0, bhi = 0;
    if (hle_guest_boot_stack(&blo, &bhi) && fp >= blo && fp < bhi) {
        lo = blo;
        hi = bhi;
    } else if (!guest_stack(&lo, &hi)) {
        return 0;
    }
    std::uint64_t floor = reinterpret_cast<std::uint64_t>(saved + 7);
    int k = 0;
    for (int i = 0; i < 96 && k < cap; ++i) {
        if (fp < floor || fp < lo || fp > hi - 16 || (fp & 7)) break;
        const std::uint64_t next = rd(fp), ret = rd(fp + 8);
        if (text(ret)) out[k++] = bn(ret);
        floor = fp + 16;
        fp = next;
    }
    return k;
}

std::string frames_text(const std::uint64_t* f, int n) {
    std::string s;
    char b[24];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%s%llx", i ? " " : "", static_cast<unsigned long long>(f[i]));
        s += b;
    }
    return s.empty() ? "none" : s;
}

// The game's code that made changes outside the movie's lock: the first frames
// up the chain in the game's own code (0x1000000-0x2700000; Scaleform and the
// libraries are on either side), counted by class. No allocation, for the
// crash report.
constexpr std::uint64_t kGameLo = 0x1000000, kGameHi = 0x2700000;
constexpr int kSites = 48, kSiteFrames = 4;
struct Site {
    std::atomic<std::uint64_t> key;
    std::uint64_t f[kSiteFrames];
    char name[16];
    char movie[24];
    std::atomic<std::uint64_t> n[8];
};
Site g_sites[kSites];
std::mutex g_sites_mu;
std::atomic<std::uint64_t> g_sites_full{0};

void count_site(const std::uint64_t* f, int n, int c, const char* thread, const char* movie) {
    std::uint64_t g[kSiteFrames] = {};
    int k = 0;
    for (int i = 0; i < n && k < kSiteFrames; ++i) {
        if (f[i] >= kGameLo && f[i] < kGameHi) g[k++] = f[i];
    }
    std::uint64_t key = 1469598103934665603ull;
    for (const std::uint64_t a : g) key = (key ^ a) * 1099511628211ull;
    for (const char* t = thread; *t; ++t) key = (key ^ static_cast<unsigned char>(*t)) * 1099511628211ull;
    for (const char* t = movie; *t; ++t) key = (key ^ static_cast<unsigned char>(*t)) * 1099511628211ull;
    if (!key) key = 1;
    for (Site& s : g_sites) {
        const std::uint64_t at = s.key.load(std::memory_order_acquire);
        if (at == key) {
            s.n[c].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!at) break;
    }
    std::lock_guard<std::mutex> lk(g_sites_mu);
    for (Site& s : g_sites) {
        const std::uint64_t at = s.key.load(std::memory_order_relaxed);
        if (at == key) {
            s.n[c].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!at) {
            std::memcpy(s.f, g, sizeof(g));
            std::snprintf(s.name, sizeof(s.name), "%s", thread);
            std::snprintf(s.movie, sizeof(s.movie), "%s", movie);
            s.n[c].fetch_add(1, std::memory_order_relaxed);
            s.key.store(key, std::memory_order_release);
            return;
        }
    }
    g_sites_full.fetch_add(1, std::memory_order_relaxed);
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

// The entries of texts the guard met without a layout, by context, for the
// next capture of that context to check: one a capture published after its
// propagate pass is on a list nothing drains, and stays without a layout for
// good (an addToPropagate skips an entry already linked), unless it is put
// back on the live list (restrand). Checked and repaired on the movie's
// Advance thread, before its propagate pass.
constexpr int kSuspects = 32;
std::mutex g_suspects_mu;
std::uint64_t g_suspect_entry[kSuspects], g_suspect_ctx[kSuspects];  // under g_suspects_mu
std::atomic<int> g_suspects_n{0};
std::atomic<std::uint64_t> g_restranded{0}, g_restrand_entries{0};

void suspect(std::uint64_t entry, std::uint64_t ctx) {
    if (!entry || !ctx) return;
    std::lock_guard<std::mutex> lk(g_suspects_mu);
    int free_slot = -1;
    for (int i = 0; i < kSuspects; ++i) {
        if (g_suspect_entry[i] == entry) return;
        if (!g_suspect_entry[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;  // full: the guard still keeps the game up
    g_suspect_entry[free_slot] = entry;
    g_suspect_ctx[free_slot] = ctx;
    g_suspects_n.fetch_add(1, std::memory_order_relaxed);
}

// At the start of a capture of `ctx`, on its Advance thread.
void repair_suspects(std::uint64_t ctx) {
    if (g_suspects_n.load(std::memory_order_relaxed) == 0) return;
    std::uint64_t mine[kSuspects];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_suspects_mu);
        for (int i = 0; i < kSuspects; ++i) {
            if (g_suspect_entry[i] && g_suspect_ctx[i] == ctx) {
                mine[n++] = g_suspect_entry[i];
                g_suspect_entry[i] = g_suspect_ctx[i] = 0;
                g_suspects_n.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
    if (!n || !readable(ctx + kContextActive, 8)) return;
    const std::uint64_t snap = rd(ctx + kContextActive);
    for (int i = 0; i < n; ++i) {
        const int moved = restrand(mine[i], snap, readable);
        if (moved <= 0) continue;
        const std::uint64_t k = g_restranded.fetch_add(1, std::memory_order_relaxed) + 1;
        g_restrand_entries.fetch_add(static_cast<std::uint64_t>(moved), std::memory_order_relaxed);
        if (log_this(k)) {
            host_log("sf text: entry %llx was on a propagate list nothing drains (published after its capture's pass) - "
                     "it and %d other entr%s put back on the live list, so this capture lays the text out (%llu so far)",
                     static_cast<unsigned long long>(mine[i]), moved - 1, moved == 2 ? "y" : "ies",
                     static_cast<unsigned long long>(k));
        }
    }
}

GUEST_ABI std::int64_t update_transform_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t node = saved[5];  // rdi
    const std::uint32_t flags = static_cast<std::uint32_t>(saved[1]);  // r8d
    const Decision d = decide(node, flags, kGameReads);
    if (!d.skipped) return 0;
    saved[1] = (saved[1] & ~std::uint64_t{0xffffffff}) | d.flags;
    wait_for_layout(node);
    {
        const std::uint64_t entry = rd(node + kNodeEntry);
        suspect(entry, entry_context(entry, readable));
    }
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

// The movie's name as its wrapper keeps it (+0x28: the name its creator,
// sub_235fae0, was given, a UTF-16 string), in ASCII; "?" when unreadable.
constexpr std::uint64_t kWrapperName = 0x28;
void movie_name(std::uint64_t w, char* out, std::size_t n) {
    std::snprintf(out, n, "?");
    if (!w || !readable(w + kWrapperName, 8)) return;
    const std::uint64_t p = rd(w + kWrapperName);
    if (!p || !readable(p, 2)) return;
    const bool all = readable(p, 2 * n);  // the usual case: one lookup, not one a character
    std::size_t k = 0;
    for (; k + 1 < n && (all || readable(p + 2 * k, 2)); ++k) {
        std::uint16_t c = 0;
        std::memcpy(&c, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p + 2 * k)), 2);
        if (!c) break;
        out[k] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    out[k ? k : 1] = 0;
}

GUEST_ABI std::int64_t advance_job_hook(std::uint64_t, std::uint64_t* saved) {
    if (Wrapper* e = note_wrapper(saved[5])) e->advance.store(gsync::current_thread(), std::memory_order_release);
    return 0;
}

GUEST_ABI std::int64_t display_job_hook(std::uint64_t, std::uint64_t* saved) {
    if (Wrapper* e = note_wrapper(saved[5])) e->display.store(gsync::current_thread(), std::memory_order_release);
    return 0;
}

// --- the cause: the HUD's update runs under its movie's lock -----------------------
// sub_1ff7c20(item, state, dt) is the in-game HUD's update: the player's bars,
// enemy tags, boss list, talk window, map name and event text. The game runs
// it as an EzWork job (vtable 0x5756bd0, sub_2363d00, a tail call) that
// sub_2362e30 queues into the work pool beside the movies' Advance jobs, and
// it drives the HUD movie through GFx's Value API (SetText at +0x138,
// GotoAndPlay at +0x150, member lookups) with no lock. The movie's own jobs -
// Advance and Capture (sub_2358e50), NextCapture (sub_2359150) - each hold
// the movie wrapper's lock (+0x38, a recursive DL mutex over the
// scePthreadMutex at +0x40) around theirs, so nothing keeps the update apart
// from them: a text set while a capture publishes goes out flagged for a
// layout none built (the crash at 0x52d6c0), and two threads in one movie's
// tree break its lists (the crashes in Display's recursive draw and the next
// capture's merge).
//
// So the update takes that lock too. The hook finds the item's movie the way
// the update itself does first (sub_1edb8a0: the movie handle at +0x20, whose
// method 0 returns the movie), the movie's wrapper among the Advance jobs
// seen, and runs the update through a trampoline under the wrapper's mutex.
// Lock order: when the game runs the update itself, on the main thread
// (sub_2362e30 without the pool), it holds data_5994200, which NextCapture
// takes before +0x38 as well; Advance never takes data_5994200 (only the
// wrapper's destructor, NextCapture and the HUD list's add, remove and update
// do). A thread already holding the lock runs as is; a wait past 100 ms runs
// as the game does, counted.
constexpr std::uint64_t kHudUpdate = 0x1ff7c20;
constexpr std::uint8_t kHudUpdatePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                               0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x28, 0x07, 0x00, 0x00};
constexpr std::uint64_t kHudItemMovie = 0x20;  // the item's movie handle
constexpr std::uint64_t kMovieRelease = 0x41cfb0;
constexpr std::uint64_t kLockWaitUs = 100000;

bool g_lock = false;
std::uint8_t* g_hud_trampoline = nullptr;
std::atomic<std::uint64_t> g_lock_runs{0}, g_lock_waits{0}, g_lock_wait_us{0}, g_lock_wait_max{0}, g_lock_timeouts{0},
    g_lock_no_wrapper{0}, g_lock_already{0};

void* guest_code(std::uint64_t a) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)); }

// The item's movie, as its handle's method 0 gives it (a Ptr<Movie>, returned
// through rdi), with the reference that call took let go again: the handle
// keeps its own.
std::uint64_t hud_movie(std::uint64_t item) {
    if (item < 0x10000) return 0;
    const std::uint64_t handle = rd(item + kHudItemMovie);
    if (handle < 0x10000) return 0;
    const std::uint64_t get = rd(rd(handle));
    if (!text(get)) return 0;
    std::uint64_t movie = 0;
    hle_call_guest<std::int64_t>(guest_code(get), &movie, handle);
    if (movie) hle_call_guest<std::int64_t>(guest_code(va(kMovieRelease)), movie);
    return movie;
}

void note_max(std::atomic<std::uint64_t>& m, std::uint64_t v) {
    std::uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

GUEST_ABI std::int64_t hud_update_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t movie = hud_movie(saved[5]);
    Wrapper* e = movie ? find_movie(movie) : nullptr;
    const std::uint64_t w = e ? e->wrapper.load(std::memory_order_acquire) : 0;
    if (!w || rd(w + kWrapperMovie) != movie) {
        const std::uint64_t n = g_lock_no_wrapper.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            host_log("sf lock: the HUD's update before its movie's Advance job was seen (movie %llx): it runs as the "
                     "game has it until then",
                     static_cast<unsigned long long>(movie));
        }
        return 0;
    }
    GuestCallRegs r{};
    r.fn = g_hud_trampoline;
    for (int k = 0; k < 6; ++k) r.ints[k] = saved[5 - k];  // rdi rsi rdx rcx r8 r9
    const auto* xmm = reinterpret_cast<const std::uint8_t*>(saved) - 136;
    for (int k = 0; k < 8; ++k) std::memcpy(&r.xmm[k], xmm + 16 * k, 8);
    std::uint64_t us = 0;
    const LockRun how = run_locked(reinterpret_cast<void**>(static_cast<std::uintptr_t>(w + kWrapperMutex)), kLockWaitUs,
                                   &us, [&] { hle_call_guest_regs(&r); });
    if (us) {
        g_lock_waits.fetch_add(1, std::memory_order_relaxed);
        g_lock_wait_us.fetch_add(us, std::memory_order_relaxed);
        note_max(g_lock_wait_max, us);
    }
    if (how == LockRun::kLocked) {
        if (g_lock_runs.fetch_add(1, std::memory_order_relaxed) == 0) {
            char mn[24];
            movie_name(w, mn, sizeof(mn));
            host_log("sf lock: the HUD's update runs under the lock of movie %llx '%s' (wrapper %llx)",
                     static_cast<unsigned long long>(movie), mn, static_cast<unsigned long long>(w));
        }
    } else if (how == LockRun::kAlreadyHeld) {
        g_lock_already.fetch_add(1, std::memory_order_relaxed);
    } else {
        const std::uint64_t n = g_lock_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (log_this(n)) {
            host_log("sf lock: the HUD's update waited %llu us for its movie's lock (wrapper %llx) and ran without it, as "
                     "the game has it (%llu such waits so far)",
                     static_cast<unsigned long long>(us), static_cast<unsigned long long>(w),
                     static_cast<unsigned long long>(n));
        }
    }
    saved[-1] = r.ret;
    return 1;  // the update ran
}

// The capture's window, from its propagate pass to its unlock.
thread_local Wrapper* t_capturing = nullptr;
thread_local std::uint64_t t_capture_ctx = 0;
GUEST_ABI std::int64_t capture_begin_hook(std::uint64_t, std::uint64_t* saved) {
    const std::uint64_t ctx = saved[5];  // rdi: the context
    t_capture_ctx = ctx;
    if (g_guard) repair_suspects(ctx);
    if (g_check) {
        Wrapper* e = find_movie(ctx - kMovieContext);
        if (e) e->capturing.store(gsync::current_thread(), std::memory_order_release);
        t_capturing = e;
    }
    return 0;  // on to the propagate pass
}

// A snapshot published with entries on its propagate list - listed after its
// propagate pass ran, so never propagated - and those entries' first data.
std::atomic<std::uint64_t> g_stranded_captures{0}, g_stranded_entries{0}, g_stranded_texts{0};
void check_stranded(std::uint64_t ctx) {
    if (!readable(ctx + kContextCaptured, 8)) return;
    const std::uint64_t snap = rd(ctx + kContextCaptured);
    if (!snap || !readable(snap + kSnapshotPropagate, 16)) return;
    const std::uint64_t head = snap + kSnapshotPropagate;
    std::uint64_t node = rd(head);
    if (node == head || node == 0) return;
    std::uint64_t entries = 0, texts = 0, first = 0;
    for (int i = 0; i < 64 && node && node != head && readable(node, 16); ++i, node = rd(node)) {
        const std::uint64_t entry = node - kEntryPropagate;
        const std::uint64_t data = entry_data(entry, kPageActive, readable);
        ++entries;
        if (data && readable(data, 8) && rd(data) == va(kTextDataVtable)) {
            ++texts;
            if (!first) first = entry;
        }
    }
    const std::uint64_t n = g_stranded_captures.fetch_add(1, std::memory_order_relaxed) + 1;
    g_stranded_entries.fetch_add(entries, std::memory_order_relaxed);
    g_stranded_texts.fetch_add(texts, std::memory_order_relaxed);
    if (log_this(n)) {
        char tn[32];
        thread_name(tn, sizeof(tn));
        host_log("sf text: a capture published with %llu entr%s still on its propagate list (%llu a text's%s%llx) - "
                 "listed after its propagate pass, never propagated; movie %llx, on %s (%llu such captures so far)",
                 static_cast<unsigned long long>(entries), entries == 1 ? "y" : "ies", static_cast<unsigned long long>(texts),
                 texts ? ", the first " : "", static_cast<unsigned long long>(first),
                 static_cast<unsigned long long>(ctx - kMovieContext), tn, static_cast<unsigned long long>(n));
    }
}

std::atomic<std::uint64_t> g_destranded{0}, g_destrand_entries{0};

GUEST_ABI std::int64_t capture_end_hook(std::uint64_t, std::uint64_t*) {
    const std::uint64_t ctx = t_capture_ctx;
    t_capture_ctx = 0;
    if (t_capturing) {
        t_capturing->capturing.store(nullptr, std::memory_order_release);
        check_stranded(t_capturing->movie.load(std::memory_order_relaxed) + kMovieContext);
    }
    t_capturing = nullptr;
    if (g_guard && ctx && readable(ctx + kContextActive, 16)) {
        // Still under the context's lock: the published snapshot is +0xd8,
        // the next one +0xd0.
        const int moved = destrand(rd(ctx + kContextCaptured), rd(ctx + kContextActive), readable);
        if (moved > 0) {
            const std::uint64_t k = g_destranded.fetch_add(1, std::memory_order_relaxed) + 1;
            g_destrand_entries.fetch_add(static_cast<std::uint64_t>(moved), std::memory_order_relaxed);
            if (log_this(k)) {
                host_log("sf text: a capture published %d entr%s listed after its propagate pass - moved to the next "
                         "snapshot's list, so its pass lays them out (%llu such captures so far)",
                         moved, moved == 1 ? "y" : "ies", static_cast<unsigned long long>(k));
            }
        }
    }
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
    if (c == kInside) return;
    // The thread's name as it had it at its first change here: reading it is
    // a file read on Linux, too dear for every change.
    thread_local char tn[16] = {};
    if (!tn[0]) {
        thread_name(tn, sizeof(tn));
        tn[sizeof(tn) - 1] = 0;
        if (!tn[0]) std::snprintf(tn, sizeof(tn), "?");
    }
    char mn[24];
    movie_name(w, mn, sizeof(mn));
    std::uint64_t f[24];
    const int nf = guest_frames(saved, f, 24);
    count_site(f, nf, c, tn, mn);
    if (c == kUnknown || !log_this(n)) return;
    const std::uint64_t data = entry ? entry_data(entry, kPageActive, readable) : arg;
    const bool is_text = data && readable(data, 8) && rd(data) == va(kTextDataVtable);
    // Beside NextCapture is Scaleform's design: the render side works on the
    // snapshot on display, not the one being built.
    const bool racing = c == kInCapture || c == kInAdvance || c == kHeldOther;
    host_log("sf text: %s %s (entry %llx%s, %llx) on %s, movie %llx '%s', wrapper %llx, callers %s; frames %s%s (%llu so "
             "far)",
             what, kClassText[c], static_cast<unsigned long long>(entry), is_text ? ", a text's" : "",
             static_cast<unsigned long long>(arg), tn, static_cast<unsigned long long>(ctx - kMovieContext), mn,
             static_cast<unsigned long long>(w), callers_text(saved).c_str(), frames_text(f, nf).c_str(),
             racing ? " - a race" : "", static_cast<unsigned long long>(n));
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
    const char* l = std::getenv("BBHOST_SF_MOVIE_LOCK");
    g_guard = !(g && *g == '0');
    g_check = c && *c == '1';
    g_lock = !(l && *l == '0');
    g_capture_us = t ? std::strtoull(t, nullptr, 10) : 0;
    g_slide = image->mem.slide;
    bool ok = true;
    if (g_guard) {
        ok = hook(image, kUpdateTransform, kUpdateTransformPrologue, sizeof(kUpdateTransformPrologue),
                  reinterpret_cast<void*>(&update_transform_hook), "TreeCacheText::UpdateTransform") && ok;
        ok = hook(image, kHandleChanges, kHandleChangesPrologue, sizeof(kHandleChangesPrologue),
                  reinterpret_cast<void*>(&handle_changes_hook), "TreeCacheText::HandleChanges") && ok;
    }
    if (g_guard || g_check) {
        ok = call_hook(image, kCapturePropagateCall, kPropagate, reinterpret_cast<void*>(&capture_begin_hook),
                       "Context::Capture's propagate pass") && ok;
        ok = call_hook(image, kCaptureUnlockCall, kPthreadMutexUnlockStub, reinterpret_cast<void*>(&capture_end_hook),
                       "Context::Capture's unlock") && ok;
    }
    if (g_check || g_lock) {
        ok = hook(image, kAdvanceJob, kAdvanceJobPrologue, sizeof(kAdvanceJobPrologue),
                  reinterpret_cast<void*>(&advance_job_hook), "the movie's Advance job") && ok;
    }
    bool locked = false;
    if (g_lock) {
        // The trampoline: the update's prologue, then a jump back past it.
        auto* page = static_cast<std::uint8_t*>(host_page_alloc(0x1000, true));
        const std::uint64_t at = va(kHudUpdate);
        if (page && std::memcmp(guest_ptr(image->mem, at), kHudUpdatePrologue, sizeof(kHudUpdatePrologue)) == 0) {
            constexpr std::size_t n = sizeof(kHudUpdatePrologue);
            std::memcpy(page, kHudUpdatePrologue, n);
            page[n] = 0xff;
            page[n + 1] = 0x25;  // jmp [rip+0]
            std::memset(page + n + 2, 0, 4);
            const std::uint64_t back = at + n;
            std::memcpy(page + n + 6, &back, 8);
            g_hud_trampoline = page;
            locked = hook(image, kHudUpdate, kHudUpdatePrologue, n, reinterpret_cast<void*>(&hud_update_hook),
                          "the HUD's update");
        } else {
            host_log("sf lock: the HUD's update (0x%llx) NOT hooked (%s)", static_cast<unsigned long long>(kHudUpdate),
                     page ? "unexpected bytes" : "no page for its trampoline");
        }
        ok = locked && ok;
    }
    if (g_check) {
        ok = hook(image, kDisplayJob, kDisplayJobPrologue, sizeof(kDisplayJobPrologue),
                  reinterpret_cast<void*>(&display_job_hook), "the movie's NextCapture job") && ok;
        ok = hook(image, kGetWritable, kGetWritablePrologue, sizeof(kGetWritablePrologue),
                  reinterpret_cast<void*>(&get_writable_hook), "Context getWritableData") && ok;
        ok = hook(image, kCreateEntry, kCreateEntryPrologue, sizeof(kCreateEntryPrologue),
                  reinterpret_cast<void*>(&create_entry_hook), "Context::CreateEntry") && ok;
        ok = hook(image, kAddToPropagate, kAddToPropagatePrologue, sizeof(kAddToPropagatePrologue),
                  reinterpret_cast<void*>(&add_to_propagate_hook), "Entry addToPropagate") && ok;
    }
    if (g_capture_us) {
        ok = call_hook(image, kCaptureLockCall, kPthreadMutexLockStub, reinterpret_cast<void*>(&capture_lock_hook),
                       "Context::Capture's lock") && ok;
    }
    host_log("sf lock: %s", locked ? "the HUD's update runs under its movie's lock, as the movie's Advance and NextCapture "
                                     "jobs do theirs"
                                   : g_lock ? "the HUD's update runs as the game has it (its hook was not placed)"
                                            : "off (BBHOST_SF_MOVIE_LOCK=0): the HUD's update runs as the game has it, "
                                              "beside its movie's jobs");
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
    if (g_lock) {
        const std::uint64_t runs = g_lock_runs.load(), waits = g_lock_waits.load(), us = g_lock_wait_us.load();
        host_log("sf lock: the HUD's update ran %llu time(s) under its movie's lock; it waited for the lock %llu time(s), "
                 "%llu us in all (%.1f us a run, the longest %llu us), %llu wait(s) past %llu ms ran without it; %llu run(s) "
                 "before its movie's Advance job was seen, %llu with the lock held already",
                 static_cast<unsigned long long>(runs), static_cast<unsigned long long>(waits),
                 static_cast<unsigned long long>(us), runs ? static_cast<double>(us) / static_cast<double>(runs) : 0.0,
                 static_cast<unsigned long long>(g_lock_wait_max.load()), static_cast<unsigned long long>(g_lock_timeouts.load()),
                 static_cast<unsigned long long>(kLockWaitUs / 1000), static_cast<unsigned long long>(g_lock_no_wrapper.load()),
                 static_cast<unsigned long long>(g_lock_already.load()));
    }
    if (g_guard) {
        host_log("sf text: %llu text update(s) ran without their layout's part; %llu node(s) marked again when it arrived, "
                 "%llu not marked, %d still waiting",
                 static_cast<unsigned long long>(g_skipped.load()), static_cast<unsigned long long>(g_marked.load()),
                 static_cast<unsigned long long>(g_unmarked.load()), g_waiting_n.load());
        host_log("sf text: %llu capture(s) published entries listed after their propagate pass, %llu entries moved to the "
                 "next snapshot's list; %llu time(s) a text's entry was found later on a list nothing drains and put back, "
                 "with %llu entries in all",
                 static_cast<unsigned long long>(g_destranded.load()), static_cast<unsigned long long>(g_destrand_entries.load()),
                 static_cast<unsigned long long>(g_restranded.load()), static_cast<unsigned long long>(g_restrand_entries.load()));
    }
    if (g_capture_us) {
        host_log("sf text: %llu capture(s) waited %llu us after their propagate pass",
                 static_cast<unsigned long long>(g_capture_sleeps.load()), static_cast<unsigned long long>(g_capture_us));
    }
    if (!g_check) return;
    host_log("sf text: %llu capture(s) published entries still on their propagate list: %llu entries, %llu of them texts",
             static_cast<unsigned long long>(g_stranded_captures.load()),
             static_cast<unsigned long long>(g_stranded_entries.load()),
             static_cast<unsigned long long>(g_stranded_texts.load()));
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
    // Who made the changes outside the lock: the game's code up the stack.
    for (const Site& s : g_sites) {
        if (!s.key.load(std::memory_order_acquire)) break;
        char row[256];
        int len = std::snprintf(row, sizeof(row), "%-15s %-16s", s.name, s.movie);
        for (int c = 1; c < kClasses && len >= 0 && len < static_cast<int>(sizeof(row)); ++c) {
            len += std::snprintf(row + len, sizeof(row) - static_cast<std::size_t>(len), " %llu",
                                 static_cast<unsigned long long>(s.n[c].load()));
        }
        for (int i = 0; i < kSiteFrames && s.f[i] && len >= 0 && len < static_cast<int>(sizeof(row)); ++i) {
            len += std::snprintf(row + len, sizeof(row) - static_cast<std::size_t>(len), "%s%llx", i ? " " : " | ",
                                 static_cast<unsigned long long>(s.f[i]));
        }
        host_log("sf text:   by the game's code: %s", row);
    }
    if (g_sites_full.load()) {
        host_log("sf text:   %llu change(s) from code past the %d places counted", static_cast<unsigned long long>(g_sites_full.load()),
                 kSites);
    }
    if (g_sites[0].key.load()) {
        host_log("sf text:   (by the game's code: the thread, the movie, then the classes after the first, in order, then "
                 "the first frames up the stack in the game's own code)");
    }
}
