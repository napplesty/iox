// iox — driver/completion_source.h: the external completion bridge (design §六).
#pragma once

#include "iox/core/fd.h"

namespace iox {

class io_context;

class completion_source {
public:
    virtual ~completion_source() = default;

    virtual iox::fd completion_fd() const noexcept { return {}; }

    virtual void on_ready(io_context&) noexcept {}

    virtual bool has_work() const noexcept { return false; }
};

}
