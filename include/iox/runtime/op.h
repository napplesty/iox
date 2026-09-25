// iox — runtime/op.h: op_base: the operation ABI every op completes through.
#pragma once

#include <cstdint>

namespace iox {

class io_context;

struct op_base {
    using thunk_t = void (*)(op_base*, io_context&, std::int32_t result,
                             std::uint32_t flags) noexcept;
    thunk_t thunk;

    explicit op_base(thunk_t thunk_fn) noexcept : thunk(thunk_fn) {}
};

}
