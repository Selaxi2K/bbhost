// The event poller's pace and generations (net/poll_pacing.h): every row of
// the server's long-poll guide for clients - WaitMs from the first poll, then
// min(25 s, WaitMaxMs); none and the old 250 ms pace once an answer lacks
// WaitMaxMs, until one has it again; the read timeout above the wait and the
// connection's own a few seconds; the next poll at once after each answer;
// errors, timeouts, 5xx and 429 backing off 1, 2, 4 ... 30 s with 20% of
// jitter, from the start again after an answer; a refused sign-in once a
// minute - then ten minutes of polls against five simulated servers, the
// generations (a poller stopped while its poll was held gets no turn, gives
// up what the poll brought, and the next one's first poll waits for it), and
// each mutant caught.
#define POLL_PACING_MUTANTS
int g_poll_pacing_mutant = 0;
#include "net/poll_pacing.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

namespace pp = net::poll_pacing;

int g_fail = 0;
bool g_quiet = false;
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_fail;
        if (!g_quiet) std::printf("FAIL: %s\n", what);
    }
}

pp::Outcome answer(long long wait_max_ms, long long elapsed_ms, bool event = false) {
    pp::Outcome o;
    o.answered = true;
    o.wait_max_ms = wait_max_ms;
    o.elapsed_ms = elapsed_ms;
    o.event = event;
    return o;
}
pp::Outcome failure(long status, long long elapsed_ms = 0, bool refused = false) {
    pp::Outcome o;
    o.status = status;
    o.elapsed_ms = elapsed_ms;
    o.refused = refused;
    return o;
}

void first_poll_and_timeouts() {
    const pp::State s;
    check(s.wait_ms == 25000, "the first poll sends WaitMs 25000");
    check(s.backoff_ms == 0, "no back-off before the first poll");
    check(pp::timeout_for(25000) == 30000, "the read timeout of a 25 s wait is 30 s");
    check(pp::timeout_for(10000) == 15000, "the read timeout is the wait and 5 s");
    check(pp::timeout_for(0) == 4000, "a poll naming no wait keeps the old 4 s");
    check(pp::connect_for(25000) == 5000, "the connection's own timeout is 5 s, apart from the hold");
    check(pp::connect_for(0) == 4000, "and never longer than the whole request");
}

void answers() {
    const pp::State s;
    pp::Next n = pp::next(s, answer(25000, 25003), 0.5);
    check(n.state.wait_ms == 25000 && n.sleep_ms == 0, "a held empty answer: WaitMs 25000, the next poll at once");
    n = pp::next(s, answer(10000, 10002), 0.5);
    check(n.state.wait_ms == 10000 && pp::timeout_for(n.state.wait_ms) == 15000, "WaitMaxMs 10000: WaitMs 10000, 15 s timeout");
    n = pp::next(s, answer(60000, 25002), 0.5);
    check(n.state.wait_ms == 25000, "WaitMaxMs above 25 s: still 25000");
    n = pp::next(s, answer(25000, 1200, true), 0.5);
    check(n.state.wait_ms == 25000 && n.sleep_ms == 0, "an event: the next poll at once");
    n = pp::next(s, answer(-1, 40, true), 0.5);
    check(n.state.wait_ms == 0 && n.sleep_ms == 0, "an event from a server without long polls: drained at once too");
    n = pp::next(s, answer(-1, 40), 0.5);
    check(n.state.wait_ms == 0, "no WaitMaxMs: the next poll names no wait");
    check(n.sleep_ms == 250, "no WaitMaxMs: the old 250 ms pace");
    check(pp::timeout_for(n.state.wait_ms) == 4000, "no WaitMaxMs: the old 4 s timeout");
    n = pp::next(s, answer(0, 40), 0.5);
    check(n.state.wait_ms == 0 && n.sleep_ms == 250, "WaitMaxMs 0 holds nothing: the old pace");
    // A server that answered a held poll at once, empty (stopping, or past
    // its cap on held polls): no tight loop.
    n = pp::next(s, answer(25000, 3), 0.5);
    check(n.state.wait_ms == 25000 && n.sleep_ms == 247, "an empty answer back in 3 ms waits out the 250 ms");
    n = pp::next(s, answer(25000, 300), 0.5);
    check(n.sleep_ms == 0, "one back in 300 ms was held: at once");
}

void errors() {
    // Network errors (0), timeouts (0), 5xx and 429: 1, 2, 4, 8, 16, 30, 30 s.
    const long kinds[] = {0, 503, 429, 500};
    for (long kind : kinds) {
        pp::State s;
        const int want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
        bool ok = true;
        for (int w : want) {
            const pp::Next n = pp::next(s, failure(kind), 0.5);
            ok = ok && n.state.backoff_ms == w && n.sleep_ms == w && n.state.wait_ms == 25000;
            s = n.state;
        }
        check(ok, kind == 0 ? "no answer backs off 1, 2, 4, 8, 16, 30, 30 s, WaitMs kept"
                            : kind == 429 ? "429 backs off the same" : "5xx backs off the same");
        // An answer resets it; the next error starts at 1 s again.
        const pp::Next a = pp::next(s, answer(25000, 25000), 0.5);
        check(a.state.backoff_ms == 0 && a.sleep_ms == 0, "an answer ends the back-off");
        const pp::Next e = pp::next(a.state, failure(kind), 0.5);
        check(e.state.backoff_ms == 1000, "the next error starts at 1 s again");
    }
    // The jitter: 20% either way, the draw's whole range.
    pp::State s;
    s.backoff_ms = 4000;
    check(pp::next(s, failure(0), 0.0).sleep_ms == 6400, "jitter at the draw's low end: 0.8 x 8 s");
    check(pp::next(s, failure(0), 0.999999).sleep_ms == 9600, "jitter at its high end: 1.2 x 8 s");
    check(pp::next(s, failure(0), 0.25).sleep_ms == 7200, "jitter a quarter in: 0.9 x 8 s");
    s.backoff_ms = 30000;
    const int top = pp::next(s, failure(0), 0.999999).sleep_ms;
    check(top == 36000 && pp::next(s, failure(0), 0.0).sleep_ms == 24000, "at the cap: 24-36 s");
    // An error says nothing about the server's long polls.
    pp::State old;
    old.wait_ms = 0;
    check(pp::next(old, failure(0), 0.5).state.wait_ms == 0, "an error after a server without long polls: still no WaitMs");
    check(pp::next(pp::State{}, failure(503), 0.5).state.wait_ms == 25000, "an error keeps WaitMs 25000");
}

void refused() {
    pp::State s;
    s.backoff_ms = 4000;
    pp::Next n = pp::next(s, failure(401, 0, true), 0.5);
    check(n.sleep_ms == 60000, "401: the next poll a minute on");
    check(n.state.backoff_ms == 4000 && n.state.wait_ms == 25000, "401: the back-off and WaitMs as they were");
    n = pp::next(pp::State{}, failure(0, 0, true), 0.5);
    check(n.sleep_ms == 60000, "a refused sign-in waits a minute whatever the failure");
    n = pp::next(pp::State{}, failure(401, 0, false), 0.5);
    check(n.sleep_ms == 60000, "a 401 waits a minute even before the refusal is noted");
}

// The server losing WaitMaxMs (an older server behind the same name) and
// getting it back: each poll's WaitMs, timeout and pause.
void loses_and_regains() {
    struct Step {
        pp::Outcome o;
        int wait, timeout, sleep;
        const char* what;
    } steps[] = {
        {answer(25000, 25004), 25000, 30000, 0, "long polls"},
        {answer(-1, 30), 0, 4000, 250, "WaitMaxMs gone: no WaitMs, the old pace"},
        {answer(-1, 25), 0, 4000, 250, "still gone"},
        {failure(0, 4000), 0, 4000, 1000, "an error on the way: backs off, no WaitMs"},
        {answer(-1, 30), 0, 4000, 250, "answered: the old pace again"},
        {answer(25000, 2600), 25000, 30000, 0, "WaitMaxMs back: WaitMs 25000, 30 s, at once"},
        {answer(25000, 25001), 25000, 30000, 0, "long polls again"},
    };
    pp::State s;
    bool ok = true;
    for (const Step& st : steps) {
        const pp::Next n = pp::next(s, st.o, 0.5);
        const bool good = n.state.wait_ms == st.wait && pp::timeout_for(n.state.wait_ms) == st.timeout && n.sleep_ms == st.sleep;
        if (!good && !g_quiet)
            std::printf("  step '%s': WaitMs %d timeout %d sleep %d, want %d %d %d\n", st.what, n.state.wait_ms,
                        pp::timeout_for(n.state.wait_ms), n.sleep_ms, st.wait, st.timeout, st.sleep);
        ok = ok && good;
        s = n.state;
    }
    check(ok, "a server losing WaitMaxMs and getting it back");
}

// Ten minutes of an idle player's polls against a simulated server.
struct Sim {
    int polls = 0, waits_sent = 0, max_in_a_second = 0;
};
enum class Server { kHolds, kOld, kAnswersAtOnce, kDownTwoMinutes, kRefuses };
Sim simulate(Server kind, double seconds) {
    Sim r;
    pp::State s;
    double t = 0.0, second_start = 0.0;
    int in_second = 0;
    unsigned draw = 12345;
    while (t < seconds) {
        ++r.polls;
        if (t - second_start >= 1.0) {
            second_start = t;
            in_second = 0;
        }
        if (++in_second > r.max_in_a_second) r.max_in_a_second = in_second;
        if (s.wait_ms > 0) ++r.waits_sent;
        pp::Outcome o;
        switch (kind) {
        case Server::kHolds: o = answer(25000, s.wait_ms > 0 ? s.wait_ms : 2500); break;
        case Server::kOld: o = answer(-1, 5); break;
        case Server::kAnswersAtOnce: o = answer(25000, 2); break;
        case Server::kDownTwoMinutes:
            o = t < 120.0 ? failure(0, 1) : answer(25000, s.wait_ms > 0 ? s.wait_ms : 2500);
            break;
        case Server::kRefuses: o = failure(401, 20, true); break;
        }
        draw = draw * 1103515245u + 12345u;
        const pp::Next n = pp::next(s, o, (draw >> 8) / double(1u << 24));
        t += o.elapsed_ms / 1000.0 + n.sleep_ms / 1000.0;
        s = n.state;
    }
    return r;
}

void ten_minutes() {
    const Sim held = simulate(Server::kHolds, 600);
    if (!g_quiet) std::printf("  a server that holds: %d polls in 10 min, %d with WaitMs\n", held.polls, held.waits_sent);
    check(held.polls >= 23 && held.polls <= 25 && held.waits_sent == held.polls, "an idle player: a poll every 25 s");
    const Sim old = simulate(Server::kOld, 600);
    if (!g_quiet) std::printf("  a server without long polls: %d polls (%d a second at most), %d with WaitMs\n", old.polls, old.max_in_a_second, old.waits_sent);
    check(old.waits_sent == 1 && old.polls > 2000 && old.polls < 2500, "without long polls: the first poll's WaitMs, then four a second");
    check(old.max_in_a_second <= 5, "without long polls: no tight loop");
    const Sim fast = simulate(Server::kAnswersAtOnce, 600);
    if (!g_quiet) std::printf("  a server that answers held polls at once: %d polls (%d a second at most)\n", fast.polls, fast.max_in_a_second);
    check(fast.max_in_a_second <= 5 && fast.polls < 2500, "a server that holds nothing despite WaitMaxMs: no tight loop");
    const Sim down = simulate(Server::kDownTwoMinutes, 600);
    if (!g_quiet) std::printf("  a server down for the first 2 min: %d polls\n", down.polls);
    check(down.polls >= 25 && down.polls <= 30, "down 2 min: ~8 tries backing off, then a poll every 25 s");
    const Sim refusing = simulate(Server::kRefuses, 600);
    check(refusing.polls == 10, "a refused sign-in: once a minute");
}

void generations() {
    pp::Gate g;
    check(!g.running() && !g.live(0) && !g.live(1), "no generation before a start");
    const std::uint64_t a = g.start();
    check(g.running() && g.live(a), "a start's generation is live");
    g.stop();
    check(!g.running() && !g.live(a), "a stop ends it");
    const std::uint64_t b = g.start();
    check(b != a && g.live(b), "the next start is a generation of its own");
    check(!g.live(a), "a stopped generation stays over once another starts");
    const bool stale_turn = g.begin(a);
    if (stale_turn) g.end();
    check(!stale_turn, "a stopped poller gets no turn");
    const bool turn = g.begin(b);
    if (turn) g.end();
    check(turn, "the current one does");
    // sleep: at once false when over, woken by a stop.
    check(!g.sleep(a, 1000), "an ended generation does not sleep");
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        g.stop();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const bool slept = g.sleep(b, 5000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    stopper.join();
    check(!slept && ms < 1000, "a stop wakes a poller's sleep");
}

// A poller stopped while its poll is held, and a new one started: the old
// one's poll comes back with an event, which it must give up (no handler, no
// ack); the new one's first poll goes out only after the old one's is over,
// and gets the event.
void held_poll_across_a_restart() {
    pp::Gate g;
    std::atomic<int> out{0}, most_out{0}, handled_by_old{0}, handled_by_new{0};
    std::atomic<bool> old_back{false}, release_old{false};
    const auto take_turn = [&](std::uint64_t gen) {
        if (!g.begin(gen)) return false;
        const int now = ++out;
        int prev = most_out.load();
        while (now > prev && !most_out.compare_exchange_weak(prev, now)) {}
        return true;
    };
    const std::uint64_t first = g.start();
    std::thread old_poller([&] {
        if (!take_turn(first)) return;
        // The held poll: the server answers it with an event once the restart is done.
        while (!release_old.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool live = g.live(first);
        if (live) ++handled_by_old;  // the guard says no: the event is the next poller's
        --out;
        old_back = true;
        g.end();
    });
    for (int i = 0; i < 2000 && out.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(out.load() == 1, "the first poller's poll is out");
    g.stop();
    const std::uint64_t second = g.start();
    std::atomic<bool> new_went_early{false};
    std::thread new_poller([&] {
        if (!take_turn(second)) return;
        if (!old_back.load()) new_went_early = true;
        if (g.live(second)) ++handled_by_new;
        --out;
        g.end();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool new_waited = handled_by_new.load() == 0;
    release_old = true;
    old_poller.join();
    new_poller.join();
    check(new_waited && !new_went_early.load(), "the new poller's first poll waits for the held one");
    check(most_out.load() == 1, "never two polls out");
    check(handled_by_old.load() == 0, "the stopped poller gives up what its held poll brought");
    check(handled_by_new.load() == 1, "the new poller gets it");
}

int run_all() {
    g_fail = 0;
    first_poll_and_timeouts();
    answers();
    errors();
    refused();
    loses_and_regains();
    ten_minutes();
    generations();
    held_poll_across_a_restart();
    return g_fail;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a hang shows where
    std::printf("poll_pacing_test: the event poller's long polls (net/poll_pacing.h)\n");
    const int fails = run_all();
    static const char* const kMutants[] = {"",
                                           "no WaitMs on the first poll",
                                           "WaitMaxMs not honoured as the cap",
                                           "long polls against a server without WaitMaxMs",
                                           "the read timeout no longer than the wait",
                                           "the connection's timeout the whole request's",
                                           "a pause after every held answer",
                                           "the old 4 s cap on the back-off",
                                           "no jitter",
                                           "the back-off not reset by an answer",
                                           "a 401 backing off like an error",
                                           "a pause after an event",
                                           "5xx waiting a minute like a 401",
                                           "an early empty answer followed at once (a tight loop)",
                                           "an error read as a server without long polls",
                                           "any running poller taken for this one",
                                           "a start reusing the stopped generation",
                                           "a turn taken while another poll is out"};
    const int mutants = static_cast<int>(sizeof kMutants / sizeof kMutants[0]) - 1;
    int caught = 0;
    g_quiet = true;
    for (int m = 1; m <= mutants; ++m) {
        g_poll_pacing_mutant = m;
        const int f = run_all();
        caught += f > 0;
        std::printf("  mutant %d (%s): %s\n", m, kMutants[m], f ? "caught" : "NOT CAUGHT");
    }
    g_poll_pacing_mutant = 0;
    std::printf("poll_pacing_test: %s (%d failures; %d of %d mutants caught)\n", fails == 0 && caught == mutants ? "pass" : "FAIL",
                fails, caught, mutants);
    return fails == 0 && caught == mutants ? 0 : 1;
}
