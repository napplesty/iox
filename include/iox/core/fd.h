// iox — core/fd.h: strong fd type + unique_fd RAII owner (design §四.⑤).
#pragma once

#include <unistd.h>

#include <type_traits>
#include <utility>

namespace iox {

struct fd {
    int v = -1;

    fd() noexcept = default;
    explicit fd(int raw) noexcept : v(raw) {}

    bool valid() const noexcept { return v >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    friend bool operator==(fd lhs, fd rhs) noexcept { return lhs.v == rhs.v; }
    friend bool operator!=(fd lhs, fd rhs) noexcept { return !(lhs == rhs); }
};

class unique_fd {
public:
    unique_fd() noexcept = default;
    explicit unique_fd(iox::fd value) noexcept : fd_(value) {}
    explicit unique_fd(int raw) noexcept : fd_(raw) {}
    ~unique_fd() { reset(); }

    unique_fd(unique_fd&& other) noexcept : fd_(std::exchange(other.fd_, iox::fd{})) {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, iox::fd{});
        }
        return *this;
    }
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    iox::fd get() const noexcept { return fd_; }
    iox::fd* slot() noexcept { return &fd_; }

    iox::fd release() noexcept { return std::exchange(fd_, iox::fd{}); }

    void reset() noexcept {
        if (fd_.valid()) {
            ::close(fd_.v);
            fd_ = iox::fd{};
        }
    }

private:
    iox::fd fd_{};
};

static_assert(std::is_nothrow_move_constructible_v<unique_fd> &&
              !std::is_copy_constructible_v<unique_fd>);

}
