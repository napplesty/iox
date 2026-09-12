// iox — unified async IO for Linux
// core/cpo.h — the vocabulary customization protocol.
//
// iox operations (io::read, io::write, …) are CPOs: invoking
// `io::read(ctx, handle, dest)` resolves, via ordinary unqualified
// `tag_invoke` lookup + ADL, to either
//   • a driver's overload — `auto tag_invoke(io::read_t, io_context&, my_handle&, wbytes)`
//     written next to the handle type, found by ADL, or
//   • the fd-driver default in iox::io (the templates at the bottom of
//     each ops header).
// A concrete driver overload always beats the constrained default template
// in overload resolution. iox uses its own tag_invoke name (not
// stdexec's) so the SPI does not move when stdexec internals churn.
#pragma once

#include <utility>

namespace iox::io {

template <class Tag, class... Args>
concept tag_invocable = requires(Tag&& tag, Args&&... args) {
    tag_invoke(std::forward<Tag>(tag), std::forward<Args>(args)...);
};

} // namespace iox::io
