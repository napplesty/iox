// iox — unified async IO for Linux
// include/iox/core/cpo.h — the vocabulary customization protocol.
#pragma once

#include <utility>

namespace iox::io {

template <class Tag, class... Args>
concept tag_invocable = requires(Tag&& tag, Args&&... args) {
    tag_invoke(std::forward<Tag>(tag), std::forward<Args>(args)...);
};

}
