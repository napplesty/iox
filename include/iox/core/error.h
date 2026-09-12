// iox — unified async IO for Linux
// include/iox/core/error.h — error value type shared by sync (std::expected) and async
#pragma once

#include <cerrno>

#include <cstring>
#include <string>
#include <utility>

namespace iox {

class error {
public:
    error() noexcept = default;

    static error from_negative(int neg_errno) noexcept { return error{-neg_errno}; }

    static error from_errno(int e) noexcept { return error{e}; }

    int code() const noexcept { return code_; }
    explicit operator bool() const noexcept { return code_ != 0; }

  // Short symbolic name (e.g. "EAGAIN"); "OK" when !*this.
    const char* name() const noexcept;

    std::string message() const;

    friend bool operator==(const error& a, const error& b) noexcept { return a.code_ == b.code_; }
    friend bool operator!=(const error& a, const error& b) noexcept { return !(a == b); }

private:
    explicit error(int code) noexcept : code_(code) {}
    int code_ = 0;
};

namespace detail {
const char* errno_name(int e) noexcept;
}

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

}
