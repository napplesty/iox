// iox — unified async IO for Linux
// stdio/stream.h — typed standard-stream handles.
//
// Non-owning views over fds 0/1/2 with direction in the type:
// io::read(ctx, iox::std_in(), buf) compiles, io::write(ctx, iox::std_in(),
// buf) does not.
#pragma once

#include "iox/core/concepts.h"
#include "iox/core/fd.h"

namespace iox {

class in_channel {
public:
    iox::fd read_handle() const noexcept { return iox::fd{0}; }

    in_channel() noexcept = default;
};

class out_channel {
public:
    iox::fd write_handle() const noexcept { return iox::fd{1}; }

    out_channel() noexcept = default;
};

class err_channel {
public:
    iox::fd write_handle() const noexcept { return iox::fd{2}; }

    err_channel() noexcept = default;
};

inline constexpr in_channel std_in{};
inline constexpr out_channel std_out{};
inline constexpr err_channel std_err{};

static_assert(io::readable<in_channel> && !io::writable<in_channel>);
static_assert(!io::readable<out_channel> && io::writable<out_channel>);
static_assert(!io::readable<err_channel> && io::writable<err_channel>);

} // namespace iox
