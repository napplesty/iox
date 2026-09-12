// iox — unified async IO for Linux
// driver/registry.h — compile-time driver registration (design §六).
//
// A driver registers itself by specializing `registered_driver` outside the
// library — the core never changes:
//
//     namespace iox::driver {
//         template <> inline constexpr bool registered_driver<demo::driver> = true;
//     }
//
// M5 keeps the registry compile-time (types are enumerated where needed);
// runtime discovery (id → factory, dlopen) arrives when a second real
// driver exists — nothing here blocks it.
#pragma once

namespace iox::driver {

/// Specialize to true for every driver type; the core and tools enumerate
/// drivers through this trait.
template <class D>
inline constexpr bool registered_driver = false;

} // namespace iox::driver
