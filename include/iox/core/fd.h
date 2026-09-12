// iox — unified async IO for Linux
// fd.h — strong type for a raw kernel file descriptor (design §四.⑤:
// no bare ints in the vocabulary).
//
// Non-owning: it refers to an fd without managing its lifetime. Owning
// handles (fs::file, net sockets, pipe ends, …) arrive in M2+ and expose
// fds through this type. Raw fds enter iox via io::poll/read/write etc.
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

} // namespace iox
