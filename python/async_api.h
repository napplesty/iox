// iox — python/async_api.h: the seam between the sync bindings and the asyncio bridge.
#pragma once

#include <nanobind/nanobind.h>

namespace iox_py {

void register_async(nanobind::module_ module_);

}
