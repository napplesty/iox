// iox — net/dns.h: name resolution over the blocking escape hatch.
#pragma once

#include <netdb.h>

#include <string>
#include <system_error>
#include <vector>

#include "iox/runtime/blocking_pool.h"
#include "iox/net/endpoint.h"

namespace iox::net {

template <class Pool>
auto resolve(Pool& pool, std::string host, std::string service) {
    return pool.run([host = std::move(host), service = std::move(service)]() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (status != 0) {
            throw std::system_error{status, std::generic_category(),
                                    "getaddrinfo: " + std::string{::gai_strerror(status)}};
        }
        std::vector<endpoint> endpoints;
        for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
            if (auto address = endpoint::from_native(entry->ai_addr, entry->ai_addrlen)) {
                endpoints.push_back(std::move(*address));
            }
        }
        ::freeaddrinfo(result);
        return endpoints;
    });
}

}
