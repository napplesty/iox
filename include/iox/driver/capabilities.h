// iox — driver/capabilities.h: runtime secondary capability query (design §四.①).
#pragma once

#include <type_traits>
#include <concepts>

#include "iox/core/cpo.h"

#include "iox/core/concepts.h"
#include "iox/core/fd.h"

namespace iox::io {

inline constexpr struct zero_copy_t {} zero_copy{};
inline constexpr struct mmap_t {} mmap{};
inline constexpr struct dma_t {} dma{};

namespace detail {

struct supports_t final {};

template <class Tag, class H>
concept supports_customized =
    tag_invocable<supports_t, Tag, const H&>;

}

template <class Tag, class H>
bool supports(Tag tag, const H& h) noexcept {
    if constexpr (detail::supports_customized<Tag, std::remove_cvref_t<H>>) {
        return static_cast<bool>(
            tag_invoke(detail::supports_t{}, tag, h));
    } else if constexpr (std::same_as<std::remove_cvref_t<H>, iox::fd> ||
                         readable<std::remove_cvref_t<H>> ||
                         writable<std::remove_cvref_t<H>>) {
        return std::is_same_v<Tag, zero_copy_t>;
    } else {
        return false;
    }
}

}
