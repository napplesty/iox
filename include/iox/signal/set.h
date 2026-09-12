// iox — unified async IO for Linux
// signal/set.h — the signal mask half of signal handling.
//
// signalfd turns signals into readable data — but only if they are blocked,
// otherwise the default disposition fires first and the process dies before
// any completion exists. signal::set blocks its signals on the CALLING thread
// (create it on the thread that runs the io_context, before spawning any
// workers that should also miss them) and unblocks them again on destruction.
//
// Lifetime rule: destroy the signal::watcher (signalfd) before the set. A
// read from the signalfd CONSUMES the pending signal; a pending-but-unread
// signal that gets unblocked delivers with its default disposition (SIGINT
// would terminate — usually harmless at shutdown, but surprising mid-run).
#pragma once

#include <pthread.h>
#include <signal.h>

#include <initializer_list>
#include <utility>

namespace iox::signal {

class set {
public:
    set(std::initializer_list<int> signatures) noexcept {
        ::sigemptyset(&mask_);
        for (int s : signatures) {
            ::sigaddset(&mask_, s);
        }
        // Surgical restore: SIG_UNBLOCK only what we blocked, so a mask the
        // user changed in between survives.
        ::pthread_sigmask(SIG_BLOCK, &mask_, nullptr);
        blocking_ = true;
    }

    set() noexcept = default;
    ~set() { reset(); }

    set(set&& other) noexcept
        : mask_(other.mask_), blocking_(std::exchange(other.blocking_, false)) {}
    set& operator=(set&& other) noexcept {
        if (this != &other) {
            reset();
            mask_ = other.mask_;
            blocking_ = std::exchange(other.blocking_, false);
        }
        return *this;
    }
    set(const set&) = delete;
    set& operator=(const set&) = delete;

    bool valid() const noexcept { return blocking_; }

    /// The blocked mask — hand this to signal::watcher::create.
    const ::sigset_t& mask() const noexcept { return mask_; }

    void reset() noexcept {
        if (blocking_) {
            ::pthread_sigmask(SIG_UNBLOCK, &mask_, nullptr);
            blocking_ = false;
        }
    }

private:
    ::sigset_t mask_{};
    bool blocking_ = false;
};

} // namespace iox::signal
