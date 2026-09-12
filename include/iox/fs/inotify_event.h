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
    event_range(wbytes buffer, std::size_t n) noexcept
        : current_(reinterpret_cast<const char*>(buffer.data())),
          end_(reinterpret_cast<const char*>(buffer.data()) + n) {}

    class iterator {
    public:
        iterator() noexcept = default;
        iterator(const char* p, const char* end) noexcept : p_(p), end_(end) {}

        event operator*() const noexcept {
            const auto* record = reinterpret_cast<const ::inotify_event*>(p_);
            const char* name = record->len > 0 ? p_ + sizeof(::inotify_event) : nullptr;
            std::string_view n;
            if (name != nullptr) {
                const std::size_t room =
                    end_ > name ? static_cast<std::size_t>(end_ - name) : 0;
                n = std::string_view{name,
                                     ::strnlen(name, std::min<std::size_t>(record->len, room))};
            }
            return event{record->wd, record->mask, record->cookie, n};
        }

        iterator& operator++() noexcept {
            const auto* record = reinterpret_cast<const ::inotify_event*>(p_);
            p_ += sizeof(::inotify_event) + record->len;
            if (p_ > end_) {
                p_ = end_; // a truncated or lying record length must not walk
            }
            return *this;
        }

        friend bool operator==(iterator a, iterator b) noexcept { return a.p_ == b.p_; }
        friend bool operator!=(iterator a, iterator b) noexcept { return !(a == b); }

    private:
        const char* p_ = nullptr;
        const char* end_ = nullptr;
    };

    iterator begin() const noexcept { return iterator{current_, end_}; }
    iterator end() const noexcept { return iterator{end_, end_}; }
    bool empty() const noexcept { return current_ == end_; }
    std::size_t size() const noexcept {
        std::size_t n = 0;
        for (const auto& [[maybe_unused]] e : *this) {
            ++n;
        }
        return n;
    }

private:
    const char* current_ = nullptr;
    const char* end_ = nullptr;
};

}
