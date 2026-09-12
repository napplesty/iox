// iox — unified async IO for Linux
// runtime/op.h — op_base: the operation ABI every vocabulary op, driver op,
// and bridge op (cancel receipts, source watches, deadlines) rides on.
// One function pointer per op, no vtable (design §三.①⑧); the completion
// path stamps its address into SQE user_data.
#pragma once

#include <cstdint>

namespace iox {

class io_context;

struct op_base {
    using thunk_t = void (*)(op_base*, io_context&, std::int32_t res,
                             std::uint32_t flags) noexcept;
    thunk_t thunk;

    explicit op_base(thunk_t t) noexcept : thunk(t) {}
};

} // namespace iox
