// iox — unified async IO for Linux
// include/iox/net/endpoint.h — socket addresses with typed construction and parsing.
#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <cstring>
#include <string>
#include <string_view>

#include "iox/core/error.h"

namespace iox::net {

class endpoint {
public:
    enum class family : std::uint8_t { ipv4, ipv6, unix_path };
    using family_t = family;

    endpoint() noexcept = default;

    static std::expected<endpoint, error> ipv4(std::string_view address, std::uint16_t port) noexcept {
        sockaddr_in ipv4_address{};
        ipv4_address.sin_family = AF_INET;
        ipv4_address.sin_port = htons(port);
        char scratch[INET_ADDRSTRLEN + 1];
        if (address.size() > INET_ADDRSTRLEN ||
            ::inet_pton(AF_INET, c_str(address, scratch, sizeof(scratch)), &ipv4_address.sin_addr) != 1) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        return endpoint{reinterpret_cast<const sockaddr*>(&ipv4_address), sizeof(ipv4_address)};
    }

    static std::expected<endpoint, error> ipv4_any(std::uint16_t port) noexcept {
        return ipv4("0.0.0.0", port);
    }

    static std::expected<endpoint, error> ipv6(std::string_view address, std::uint16_t port) noexcept {
        sockaddr_in6 ipv6_address{};
        ipv6_address.sin6_family = AF_INET6;
        ipv6_address.sin6_port = htons(port);
        char scratch[INET6_ADDRSTRLEN + 1];
        if (address.size() > INET6_ADDRSTRLEN ||
            ::inet_pton(AF_INET6, c_str(address, scratch, sizeof(scratch)), &ipv6_address.sin6_addr) != 1) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        return endpoint{reinterpret_cast<const sockaddr*>(&ipv6_address), sizeof(ipv6_address)};
    }

    static std::expected<endpoint, error> unix(std::string_view path, bool abstract = false) noexcept {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        sockaddr_un unix_address{};
        unix_address.sun_family = AF_UNIX;
        if (abstract) {
            unix_address.sun_path[0] = '\0';
            ::memcpy(unix_address.sun_path + 1, path.data(), path.size());
        } else {
            ::memcpy(unix_address.sun_path, path.data(), path.size());
        }
        return endpoint{reinterpret_cast<const sockaddr*>(&unix_address),
                        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size())};
    }

    static std::expected<endpoint, error> parse(std::string_view text) noexcept {
        if (text.starts_with('[')) {
            const auto close_bracket = text.find(']');
            if (close_bracket == std::string_view::npos || close_bracket + 2 > text.size() ||
                text[close_bracket + 1] != ':') {
                return std::unexpected(error::from_errno(EINVAL));
            }
            const auto address = text.substr(1, close_bracket - 1);
            const auto port = parse_port(text.substr(close_bracket + 2));
            if (!port) {
                return std::unexpected(port.error());
            }
            return ipv6(address, *port);
        }
        if (text.starts_with('@')) {
            return unix(text.substr(1), true);
        }
        if (text.starts_with('/')) {
            return unix(text);
        }
        if (text.starts_with("unix:")) {
            return unix(text.substr(5));
        }
        if (const auto colon = text.find(':'); colon != std::string_view::npos) {
            const auto port = parse_port(text.substr(colon + 1));
            if (!port) {
                return std::unexpected(port.error());
            }
            return ipv4(text.substr(0, colon), *port);
        }
        return std::unexpected(error::from_errno(EINVAL));
    }

    static std::expected<endpoint, error> from_native(const sockaddr* address, socklen_t length) noexcept {
        if (address == nullptr || length > sizeof(sockaddr_storage)) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        switch (address->sa_family) {
        case AF_INET:
        case AF_INET6:
        case AF_UNIX:
            return endpoint{address, length};
        default:
            return std::unexpected(error::from_errno(EAFNOSUPPORT));
        }
    }

    family family() const noexcept {
        switch (native().ss_family) {
        case AF_INET:
            return family::ipv4;
        case AF_INET6:
            return family::ipv6;
        default:
            return family::unix_path;
        }
    }

    std::uint16_t port() const noexcept {
        if (const auto* ipv4_address = as_ipv4()) {
            return ntohs(ipv4_address->sin_port);
        }
        if (const auto* ipv6_address = as_ipv6()) {
            return ntohs(ipv6_address->sin6_port);
        }
        return 0;
    }

    std::string to_string() const {
        char buffer[128];
        if (const auto* ipv4_address = as_ipv4()) {
            ::inet_ntop(AF_INET, &ipv4_address->sin_addr, buffer, sizeof(buffer));
            return std::string{buffer} + ":" + std::to_string(port());
        }
        if (const auto* ipv6_address = as_ipv6()) {
            ::inet_ntop(AF_INET6, &ipv6_address->sin6_addr, buffer, sizeof(buffer));
            return "[" + std::string{buffer} + "]:" + std::to_string(port());
        }
        const auto* unix_address = as_unix();
        if (unix_address->sun_path[0] == '\0') {
            return "@" + std::string{unix_address->sun_path + 1, strnlen(unix_address->sun_path + 1, sizeof(unix_address->sun_path) - 1)};
        }
        return std::string{unix_address->sun_path, strnlen(unix_address->sun_path, sizeof(unix_address->sun_path))};
    }

    const sockaddr* data() const noexcept {
        return reinterpret_cast<const sockaddr*>(&storage_);
    }
    socklen_t size() const noexcept { return length_; }
    const sockaddr_storage& native() const noexcept { return storage_; }

    friend bool operator==(const endpoint& lhs, const endpoint& rhs) noexcept {
        return lhs.length_ == rhs.length_ && ::memcmp(&lhs.storage_, &rhs.storage_, lhs.length_) == 0;
    }

private:
    endpoint(const sockaddr* address, socklen_t length) noexcept : length_(length) {
        ::memcpy(&storage_, address, length);
    }

    static const char* c_str(std::string_view source, char* dest, std::size_t capacity) noexcept {
        ::memcpy(dest, source.data(), source.size());
        dest[source.size()] = '\0';
        return dest;
    }

    static std::expected<std::uint16_t, error> parse_port(std::string_view text) noexcept {
        if (text.empty() || text.size() > 5) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        std::uint32_t value = 0;
        for (const char digit : text) {
            if (digit < '0' || digit > '9') {
                return std::unexpected(error::from_errno(EINVAL));
            }
            value = value * 10 + static_cast<std::uint32_t>(digit - '0');
        }
        if (value > 65535) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        return static_cast<std::uint16_t>(value);
    }

    const sockaddr_in* as_ipv4() const noexcept {
        return storage_.ss_family == AF_INET ? reinterpret_cast<const sockaddr_in*>(&storage_) : nullptr;
    }
    const sockaddr_in6* as_ipv6() const noexcept {
        return storage_.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6*>(&storage_) : nullptr;
    }
    const sockaddr_un* as_unix() const noexcept {
        return storage_.ss_family == AF_UNIX ? reinterpret_cast<const sockaddr_un*>(&storage_) : nullptr;
    }

    sockaddr_storage storage_{};
    socklen_t length_ = 0;
};

}
