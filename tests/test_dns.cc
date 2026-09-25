// iox — unified async IO for Linux
// tests/test_dns.cc — and net::resolve over loopback names.
#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <system_error>

#include <iox/runtime/blocking_pool.h>
#include <iox/compose/detach.h>
#include <iox/core/exec.h>
#include <iox/net/dns.h>

using namespace iox;
namespace ex = iox::exec;

TEST_CASE("blocking_pool: delivers a worker value on the io thread") {
    io_context context;
    blocking_pool pool(context);

    auto result = ex::sync_wait(context, pool.run([] { return std::string{"from a worker"}; }));
    REQUIRE(result);
    CHECK(std::get<0>(*result) == "from a worker");
}

TEST_CASE("blocking_pool: worker exceptions become set_error(exception_ptr)") {
    io_context context;
    blocking_pool pool(context);

    bool threw = false;
    try {
        auto result = ex::sync_wait(context, pool.run([]() -> int {
            throw std::runtime_error{"worker blew up"};
        }));
        (void)result;
    } catch (const std::runtime_error& error) {
        threw = true;
        CHECK(std::string_view{error.what()} == "worker blew up");
    }
    CHECK(threw);
}

TEST_CASE("blocking_pool: many tasks serialize back correctly") {
    io_context context;
    blocking_pool pool(context, 2);

    int delivered = 0;
    for (int index = 0; index < 32; ++index) {
        ex::detach(pool.run([index] { return index * 2; }) | ex::then([&](int value) {
                      delivered += (value % 2 == 0) ? 1 : 0;
                  }));
    }
    context.run_for(std::chrono::milliseconds(500));
    CHECK(delivered == 32);
}

TEST_CASE("net::resolve: localhost resolves to a loopback endpoint") {
    io_context context;
    blocking_pool pool(context);

    auto result = ex::sync_wait(context, net::resolve(pool, "localhost", "8080"));
    REQUIRE(result);
    const auto& endpoints = std::get<0>(*result);
    REQUIRE_FALSE(endpoints.empty());
    bool loopback = false;
    for (const auto& endpoint : endpoints) {
        loopback = loopback || endpoint.to_string().find("127.0.0.1") == 0 ||
                   endpoint.to_string().find("[::1]") == 0;
        CHECK(endpoint.port() == 8080);
    }
    CHECK(loopback);
}

TEST_CASE("net::resolve: bad host reports an error") {
    io_context context;
    blocking_pool pool(context);

    bool threw = false;
    try {
        auto result = ex::sync_wait(context, net::resolve(pool, "no.such.host.invalid.", "80"));
        (void)result;
    } catch (const std::system_error&) {
        threw = true;
    }
    CHECK(threw);
}
