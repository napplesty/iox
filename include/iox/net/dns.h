// iox — unified async IO for Linux
// include/iox/net/dns.h — name resolution over the blocking escape hatch.
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

        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
        if (rc != 0) {
            throw std::system_error{rc, std::generic_category(),
                                    "getaddrinfo: " + std::string{::gai_strerror(rc)}};
        }
        std::vector<endpoint> out;
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (auto ep = endpoint::from_native(ai->ai_addr, ai->ai_addrlen)) {
                out.push_back(std::move(*ep));
            }
        }
        ::freeaddrinfo(res);
        return out;
    });
}

}
