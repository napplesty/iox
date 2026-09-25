// iox — unified async IO for Linux
// include/iox/fs/inotify_event.h — walk the variable-length records an fs::watcher
#pragma once

#include <sys/inotify.h>

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "iox/core/buffer.h"

namespace iox::fs {

struct event {
    int wd = -1;
    std::uint32_t mask = 0;
    std::uint32_t cookie = 0;
    std::string_view name;

    bool has(std::uint32_t bit) const noexcept { return (mask & bit) != 0; }
};

class event_range {
public:
    event_range() noexcept = default;
    event_range(wbytes buffer, std::size_t byte_count) noexcept
        : current_(reinterpret_cast<const char*>(buffer.data())),
          end_(reinterpret_cast<const char*>(buffer.data()) + byte_count) {}

    class iterator {
    public:
        iterator() noexcept = default;
        iterator(const char* position, const char* end) noexcept : position_(position), end_(end) {}

        event operator*() const noexcept {
            const auto* record = reinterpret_cast<const ::inotify_event*>(position_);
            const char* name = record->len > 0 ? position_ + sizeof(::inotify_event) : nullptr;
            std::string_view name_view;
            if (name != nullptr) {
                const std::size_t room =
                    end_ > name ? static_cast<std::size_t>(end_ - name) : 0;
                name_view = std::string_view{name,
                                     ::strnlen(name, std::min<std::size_t>(record->len, room))};
            }
            return event{record->wd, record->mask, record->cookie, name_view};
        }

        iterator& operator++() noexcept {
            const auto* record = reinterpret_cast<const ::inotify_event*>(position_);
            position_ += sizeof(::inotify_event) + record->len;
            if (position_ > end_) {
                position_ = end_; // a truncated or lying record length must not walk
            }
            return *this;
        }

        friend bool operator==(iterator lhs, iterator rhs) noexcept { return lhs.position_ == rhs.position_; }
        friend bool operator!=(iterator lhs, iterator rhs) noexcept { return !(lhs == rhs); }

    private:
        const char* position_ = nullptr;
        const char* end_ = nullptr;
    };

    iterator begin() const noexcept { return iterator{current_, end_}; }
    iterator end() const noexcept { return iterator{end_, end_}; }
    bool empty() const noexcept { return current_ == end_; }
    std::size_t size() const noexcept {
        std::size_t count = 0;
        for (const auto& [[maybe_unused]] entry : *this) {
            ++count;
        }
        return count;
    }

private:
    const char* current_ = nullptr;
    const char* end_ = nullptr;
};

}
