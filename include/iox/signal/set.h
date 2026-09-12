// iox — unified async IO for Linux
// include/iox/signal/set.h — the signal mask half of signal handling.
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

}
