// iox — driver/registry.h: compile-time driver registration (design §六).
#pragma once

namespace iox::driver {

template <class D>
inline constexpr bool registered_driver = false;

}
