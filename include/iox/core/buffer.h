// iox — unified async IO for Linux
// include/iox/core/buffer.h — direction-typed buffer views (design §四.②).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace iox {

template <class T>
class wview {
    static_assert(std::is_same_v<T, std::byte> || !std::is_const_v<T>,
                  "wview element must be mutable");

public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;

    wview() noexcept = default;
    wview(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <class U, std::size_t N>
    wview(U (&arr)[N]) noexcept : data_(arr), size_(N) {}

    template <class U>
    wview(std::span<U> s) noexcept : data_(s.data()), size_(s.size()) {}

    T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    wview first(std::size_t n) const noexcept { return wview{data_, n < size_ ? n : size_}; }
    wview last(std::size_t n) const noexcept {
        return wview{data_ + (size_ - (n < size_ ? n : size_)), n < size_ ? n : size_};
    }
    wview subview(std::size_t off, std::size_t n) const noexcept {
        off = off < size_ ? off : size_;
        return wview{data_ + off, n < (size_ - off) ? n : (size_ - off)};
    }

    std::span<T> as_span() const noexcept { return std::span<T>{data_, size_}; }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

template <class T>
class rview {
public:
    using element_type = const T;
    using value_type = std::remove_cv_t<T>;

    rview() noexcept = default;
    rview(const T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <class U, std::size_t N>
    rview(U (&arr)[N]) noexcept : data_(arr), size_(N) {}

    template <class U>
    rview(std::span<const U> s) noexcept : data_(s.data()), size_(s.size()) {}

    template <class U>
    rview(wview<U> w) noexcept : data_(w.data()), size_(w.size()) {}

    const T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    rview first(std::size_t n) const noexcept { return rview{data_, n < size_ ? n : size_}; }
    rview last(std::size_t n) const noexcept {
        return rview{data_ + (size_ - (n < size_ ? n : size_)), n < size_ ? n : size_};
    }
    rview subview(std::size_t off, std::size_t n) const noexcept {
        off = off < size_ ? off : size_;
        return rview{data_ + off, n < (size_ - off) ? n : (size_ - off)};
    }

    std::span<const T> as_span() const noexcept { return std::span<const T>{data_, size_}; }

private:
    const T* data_ = nullptr;
    std::size_t size_ = 0;
};

using wbytes = wview<std::byte>;
using rbytes = rview<std::byte>;

struct registered_buffer {
    std::uint32_t index = 0;
    void* owner = nullptr;
    std::byte* data = nullptr;
    std::size_t size = 0;

    wbytes writable() const noexcept { return wbytes{data, size}; }
    rbytes readable() const noexcept { return rbytes{data, size}; }
    rbytes readable_first(std::size_t n) const noexcept { return readable().first(n); }
};

template <class T>
wbytes as_wbytes(wview<T> w) noexcept {
    return wbytes{reinterpret_cast<std::byte*>(w.data()), w.size() * sizeof(T)};
}
template <class T>
rbytes as_rbytes(rview<T> r) noexcept {
    return rbytes{reinterpret_cast<const std::byte*>(r.data()), r.size() * sizeof(T)};
}

inline wbytes as_wbytes(std::span<char> s) noexcept {
    return wbytes{reinterpret_cast<std::byte*>(s.data()), s.size()};
}
inline rbytes as_rbytes(std::span<const char> s) noexcept {
    return rbytes{reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
inline wbytes as_wbytes(std::span<std::byte> s) noexcept { return wbytes{s}; }
inline rbytes as_rbytes(std::span<const std::byte> s) noexcept { return rbytes{s}; }

}
