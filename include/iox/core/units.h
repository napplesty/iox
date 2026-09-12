// iox — unified async IO for Linux
// units.h — strong newtypes for positions and sizes (design §四.⑤).
//
// Plain integers in IO signatures are a classic bug farm: read(fd, buf,
// offset, len) style calls invite transposed arguments that compile fine and
// corrupt data at runtime. These wrappers make the compiler catch them —
// lightweight formal verification for the most common IO mistakes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace iox {

/// Absolute position within a seekable object (file offset).
struct uoffset_t {
    std::uint64_t v = 0;

    uoffset_t() noexcept = default;
    explicit uoffset_t(std::uint64_t value) noexcept : v(value) {}

    friend uoffset_t operator+(uoffset_t a, std::uint64_t d) noexcept { return uoffset_t{a.v + d}; }
    friend uoffset_t operator-(uoffset_t a, std::uint64_t d) noexcept { return uoffset_t{a.v - d}; }
    friend std::uint64_t operator-(uoffset_t a, uoffset_t b) noexcept { return a.v - b.v; }
    friend bool operator==(uoffset_t a, uoffset_t b) noexcept { return a.v == b.v; }
    friend bool operator<(uoffset_t a, uoffset_t b) noexcept { return a.v < b.v; }
};

/// Byte count for transfers.
struct io_size_t {
    std::size_t v = 0;

    io_size_t() noexcept = default;
    explicit io_size_t(std::size_t value) noexcept : v(value) {}

    friend bool operator==(io_size_t a, io_size_t b) noexcept { return a.v == b.v; }
};

} // namespace iox
