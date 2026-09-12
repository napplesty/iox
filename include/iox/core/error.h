// iox — unified async IO for Linux
// error.h — error value type shared by sync (std::expected) and async
//             (set_error) paths.
//
// M1 scope: errno-compatible code + message lookup. Domain classification
// (system/net/fs/driver) lands in M2 per the design doc §四.④.
#pragma once

#include <cerrno>

#include <cstring>
#include <string>
#include <utility>

namespace iox {

/// errno-compatible error code. Cheap to store and move (hot-path friendly);
/// text is materialized only on demand.
class error {
public:
    error() noexcept = default;

    /// Construct from a negative errno value, as returned by io_uring CQEs
    /// and most raw syscalls (e.g. -EAGAIN).
    static error from_negative(int neg_errno) noexcept { return error{-neg_errno}; }

    /// Construct from a positive errno value.
    static error from_errno(int e) noexcept { return error{e}; }

    int code() const noexcept { return code_; }
    explicit operator bool() const noexcept { return code_ != 0; }

    /// Short symbolic name (e.g. "EAGAIN"); "OK" when !*this.
    const char* name() const noexcept;

    /// Human readable message; falls back to strerror_r text.
    std::string message() const;

    friend bool operator==(const error& a, const error& b) noexcept { return a.code_ == b.code_; }
    friend bool operator!=(const error& a, const error& b) noexcept { return !(a == b); }

private:
    explicit error(int code) noexcept : code_(code) {}
    int code_ = 0;
};

namespace detail {
/// Table-backed errno name lookup so name() is async-signal-safe and
/// allocation-free. Only the errno values iox actually surfaces are listed.
const char* errno_name(int e) noexcept;
} // namespace detail

inline const char* error::name() const noexcept {
    return code_ == 0 ? "OK" : detail::errno_name(code_);
}

inline std::string error::message() const {
    if (code_ == 0) {
        return "success";
    }
    char buf[256];
    return std::string{::strerror_r(code_, buf, sizeof(buf))};
}

} // namespace iox
