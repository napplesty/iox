// iox — unified async IO for Linux
// include/iox/core/fd.h — strong type for a raw kernel file descriptor (design §四.⑤:
#pragma once

namespace iox {

struct fd {
    int v = -1;

    fd() noexcept = default;
    explicit fd(int raw) noexcept : v(raw) {}

    bool valid() const noexcept { return v >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    friend bool operator==(fd a, fd b) noexcept { return a.v == b.v; }
    friend bool operator!=(fd a, fd b) noexcept { return !(a == b); }
};

}
