#pragma once

// The event poller's pace and its generations (net/session.cpp), as code the
// poller and tests/poll_pacing_test.cpp share.
//
// The private server holds an empty POST /np/events/poll until an event for
// the player arrives or the wait ends (The Hunter's Dream, since 2026-10-17,
// its docs/np-long-poll.md for client authors). A poll may name how long in
// "WaitMs", up to the server's "WaitMaxMs", which every answer of such a
// server carries. So the poller asks for 25 s from its first poll on, and
// for min(25 s, WaitMaxMs) once an answer has said; its read timeout is the
// wait and 5 s, the connection's own (name lookup, TCP, TLS) stays a few
// seconds; and the next poll goes out as soon as an answer is in, since the
// server did the waiting. An idle player sends a poll every 25 s instead of
// four a second. An answer without WaitMaxMs comes from a server that does
// not hold polls and answers at once: the poller names no wait and goes back
// to its old pace, a poll 250 ms after each answer, until an answer carries
// WaitMaxMs again. Errors, timeouts, 5xx and 429 back off 1, 2, 4, 8 ... 30 s
// with 20% of jitter either way, from the first again after an answer; a
// refused sign-in (HTTP 401, net::account_refused) asks once a minute.
//
// One thing past the guide: an empty answer that came back in under 250 ms
// was not held, whatever WaitMaxMs said (a server stopping answers every held
// poll at once, and so does one past its cap on held polls), and the next
// poll waits out the rest of the 250 ms - at most the old pace, never a
// tight loop. A held answer takes the wait, or as long as an event took to
// arrive; an answer with an event is followed at once.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#ifdef POLL_PACING_MUTANTS
extern int g_poll_pacing_mutant;
#define POLL_PACING_MUTANT(n) (g_poll_pacing_mutant == (n))
#else
#define POLL_PACING_MUTANT(n) false
#endif

namespace net::poll_pacing {

constexpr int kWaitMs = 25000;         // the wait asked for, at most the server's WaitMaxMs
constexpr int kReadMarginMs = 5000;    // a poll that names a wait: its read timeout is the wait and this
constexpr int kConnectMs = 5000;       // the connection within it (name lookup, TCP, TLS): not the hold
constexpr int kPlainTimeoutMs = 4000;  // a poll that names no wait (the old timeout)
constexpr int kPaceMs = 250;           // the old pace: a poll this long after each empty answer
constexpr int kBackoffFirstMs = 1000;
constexpr int kBackoffMaxMs = 30000;
constexpr int kRefusedMs = 60000;      // a refused sign-in: once a minute

// What the poller carries from one poll to the next.
struct State {
    int wait_ms = POLL_PACING_MUTANT(1) ? 0 : kWaitMs;  // the next poll's WaitMs; 0: it names none
    int backoff_ms = 0;                                  // the last back-off; 0 after an answer
};

// How a poll ended.
struct Outcome {
    bool answered = false;        // a 2xx with a JSON body
    long long wait_max_ms = -1;   // the answer's WaitMaxMs; -1: it has none
    bool event = false;           // the answer's HasEvent
    long long elapsed_ms = 0;     // from sending the poll to its end
    long status = 0;              // not answered: the HTTP status; 0 when none came (no connection, a timeout)
    bool refused = false;         // not answered: the server refused this PC's sign-in (a 401)
};

struct Next {
    State state;       // for the next poll
    int sleep_ms = 0;  // before it goes out
};

// The read timeout of a poll that sends `wait_ms` (0: names no wait).
inline int timeout_for(int wait_ms) {
    if (wait_ms <= 0) return kPlainTimeoutMs;
    return POLL_PACING_MUTANT(4) ? wait_ms : wait_ms + kReadMarginMs;
}

// Its connection's own timeout: a few seconds, within the whole.
inline int connect_for(int wait_ms) {
    if (POLL_PACING_MUTANT(5)) return timeout_for(wait_ms);
    return std::min(kConnectMs, timeout_for(wait_ms));
}

// The back-off after `prev_ms` (0: none yet): 1 s, then twice the last, at most 30 s.
inline int backoff_after(int prev_ms) {
    if (prev_ms <= 0) return kBackoffFirstMs;
    if (POLL_PACING_MUTANT(7)) return std::min(prev_ms * 2, 4000);  // the old cap
    return std::min(prev_ms * 2, kBackoffMaxMs);
}

// `back_ms` with 20% of jitter either way; `unit` is the caller's draw in [0, 1).
inline int jittered(int back_ms, double unit) {
    if (unit < 0.0) unit = 0.0;
    if (unit >= 1.0) unit = 0.999999;
    const double spread = POLL_PACING_MUTANT(8) ? 0.0 : 0.4;
    return static_cast<int>(back_ms * (1.0 - spread / 2 + spread * unit) + 0.5);
}

// The next poll's WaitMs, its pause before it, and the back-off, from how the
// last one ended. `unit` is a draw in [0, 1) for the jitter.
inline Next next(const State& s, const Outcome& o, double unit) {
    Next n;
    n.state = s;
    if (o.answered) {
        n.state.backoff_ms = POLL_PACING_MUTANT(9) ? s.backoff_ms : 0;
        if (POLL_PACING_MUTANT(3)) {
            n.state.wait_ms = kWaitMs;  // WaitMaxMs ignored: long polls whatever the server
        } else if (o.wait_max_ms > 0) {
            n.state.wait_ms = POLL_PACING_MUTANT(2) ? kWaitMs : static_cast<int>(std::min<long long>(kWaitMs, o.wait_max_ms));
        } else {
            n.state.wait_ms = 0;  // a server without long polls: it answers at once
        }
        if (o.event) {
            n.sleep_ms = POLL_PACING_MUTANT(11) ? kPaceMs : 0;  // drained at once, as before
        } else if (n.state.wait_ms > 0) {
            // The server waited: the next poll now. One it did not hold
            // (answered within kPaceMs) waits out the rest of the old pace.
            if (POLL_PACING_MUTANT(6)) n.sleep_ms = kPaceMs;
            else if (POLL_PACING_MUTANT(13)) n.sleep_ms = 0;
            else n.sleep_ms = static_cast<int>(std::max<long long>(0, kPaceMs - std::max<long long>(0, o.elapsed_ms)));
        } else {
            n.sleep_ms = kPaceMs;
        }
        return n;
    }
    if (POLL_PACING_MUTANT(14)) n.state.wait_ms = 0;  // an error read as "no long polls"
    if ((o.refused || o.status == 401) && !POLL_PACING_MUTANT(10)) {
        n.sleep_ms = kRefusedMs;  // not until a minute on (net/account.h); the back-off stays where it was
        return n;
    }
    if (POLL_PACING_MUTANT(12) && o.status >= 500) {
        n.sleep_ms = kRefusedMs;
        return n;
    }
    n.state.backoff_ms = backoff_after(s.backoff_ms);
    n.sleep_ms = jittered(n.state.backoff_ms, unit);
    return n;
}

// The poller's generations and its one request at a time. poller_start begins
// a generation and poller_stop ends it; each poller thread belongs to one. A
// thread whose generation has ended - the context stopped, and maybe started
// again, while its poll was held - must not give what that poll brought to the
// handler, ack it or move a cursor: nothing acked it, so the next generation's
// poller gets the same event. And one request is out at a time: a new
// poller's first poll waits until an old one's held poll has ended (its
// request asks live() about once a second and gives up once it is false).
class Gate {
public:
    // A new generation, the current one from now on.
    std::uint64_t start() {
        std::lock_guard<std::mutex> lk(mu_);
        current_ = POLL_PACING_MUTANT(16) ? (counter_ ? counter_ : ++counter_) : ++counter_;
        cv_.notify_all();
        return current_;
    }
    // No generation is current.
    void stop() {
        std::lock_guard<std::mutex> lk(mu_);
        current_ = 0;
        cv_.notify_all();
    }
    bool running() const {
        std::lock_guard<std::mutex> lk(mu_);
        return current_ != 0;
    }
    bool live(std::uint64_t gen) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (POLL_PACING_MUTANT(15)) return current_ != 0;  // any poller running, not this one
        return gen != 0 && current_ == gen;
    }
    // Waits until no other request is out and takes the turn; false (no turn)
    // once `gen` is over.
    bool begin(std::uint64_t gen) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!POLL_PACING_MUTANT(17)) cv_.wait(lk, [&] { return !busy_ || current_ != gen; });
        if (gen == 0 || current_ != gen) return false;
        busy_ = true;
        return true;
    }
    void end() {
        std::lock_guard<std::mutex> lk(mu_);
        busy_ = false;
        cv_.notify_all();
    }
    // Sleeps up to `ms`; false when `gen` ended meanwhile (at once when it had).
    bool sleep(std::uint64_t gen, int ms) {
        std::unique_lock<std::mutex> lk(mu_);
        return !cv_.wait_for(lk, std::chrono::milliseconds(ms < 0 ? 0 : ms), [&] { return current_ != gen; });
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t counter_ = 0, current_ = 0;
    bool busy_ = false;
};

}  // namespace net::poll_pacing
