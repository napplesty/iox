// iox — core/error.h: the error value shared by sync (std::expected) and async (set_error) paths.
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

    static error from_errno(int error_number) noexcept { return error{error_number}; }

    int code() const noexcept { return code_; }
    explicit operator bool() const noexcept { return code_ != 0; }

    const char* name() const noexcept;

    std::string message() const;

    friend bool operator==(const error& lhs, const error& rhs) noexcept { return lhs.code_ == rhs.code_; }
    friend bool operator!=(const error& lhs, const error& rhs) noexcept { return !(lhs == rhs); }

private:
    explicit error(int code) noexcept : code_(code) {}
    int code_ = 0;
};

namespace detail {
const char* errno_name(int error_number) noexcept;
}

inline const char* error::name() const noexcept {
    return code_ == 0 ? "OK" : detail::errno_name(code_);
}

inline std::string error::message() const {
    if (code_ == 0) {
        return "success";
    }
    char buffer[256];
    return std::string{::strerror_r(code_, buffer, sizeof(buffer))};
}

}
