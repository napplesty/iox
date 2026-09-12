// iox — unified async IO for Linux
// driver/capabilities.h — runtime secondary capability query (design §四.①):
// the compile-time concepts say what the VOCABULARY accepts; io::supports
// asks a HANDLE whether an optional, hardware-dependent capability is
// available at run time (zero-copy, mmap, …).
//
//     if (io::supports(io::zero_copy, handle)) { take the fast path }
//
// Defaults: fd-backed handles report zero_copy = true (registered buffers
// exist on every ring) and everything else false. Driver handles override
// via tag_invoke(supports_t, Tag, const H&).
#pragma once

#include <type_traits>
#include <concepts>

#include "iox/core/cpo.h"

#include "iox/core/concepts.h"
#include "iox/core/fd.h"

namespace iox::io {

// capability tags
inline constexpr struct zero_copy_t {} zero_copy{};
inline constexpr struct mmap_t {} mmap{};
inline constexpr struct dma_t {} dma{}; // device DMA engines (M6+ drivers)

namespace detail {

struct supports_t final {};

template <class Tag, class H>
concept supports_customized =
    tag_invocable<supports_t, Tag, const H&>;

} // namespace detail

/// Does `h` provide the (optional) capability `tag` at run time?
template <class Tag, class H>
bool supports(Tag tag, const H& h) noexcept {
    if constexpr (detail::supports_customized<Tag, std::remove_cvref_t<H>>) {
        return static_cast<bool>(
            tag_invoke(detail::supports_t{}, tag, h));
    } else if constexpr (std::same_as<std::remove_cvref_t<H>, iox::fd> ||
                         readable<std::remove_cvref_t<H>> ||
                         writable<std::remove_cvref_t<H>>) {
        // fd driver defaults: registered buffers give every fd handle a
        // zero-copy path; everything else is fd-unspecific.
        return std::is_same_v<Tag, zero_copy_t>;
    } else {
        return false;
    }
}

} // namespace iox::io
