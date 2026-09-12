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
    io_context ctx;
    blocking_pool pool(ctx);

    auto r = ex::sync_wait(ctx, pool.run([] { return std::string{"from a worker"}; }));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == "from a worker");
}

TEST_CASE("blocking_pool: worker exceptions become set_error(exception_ptr)") {
    io_context ctx;
    blocking_pool pool(ctx);

    bool threw = false;
    try {
        auto r = ex::sync_wait(ctx, pool.run([]() -> int {
            throw std::runtime_error{"worker blew up"};
        }));
        (void)r;
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string_view{e.what()} == "worker blew up");
    }
    CHECK(threw);
}

TEST_CASE("blocking_pool: many tasks serialize back correctly") {
    io_context ctx;
    blocking_pool pool(ctx, 2);

    int delivered = 0;
    for (int i = 0; i < 32; ++i) {
        ex::detach(pool.run([i] { return i * 2; }) | ex::then([&](int v) {
                      delivered += (v % 2 == 0) ? 1 : 0;
                  }));
    }
    ctx.run_for(std::chrono::milliseconds(500));
    CHECK(delivered == 32);
}

TEST_CASE("net::resolve: localhost resolves to a loopback endpoint") {
    io_context ctx;
    blocking_pool pool(ctx);

    auto r = ex::sync_wait(ctx, net::resolve(pool, "localhost", "8080"));
    REQUIRE(r);
    const auto& eps = std::get<0>(*r);
    REQUIRE_FALSE(eps.empty());
    bool loopback = false;
    for (const auto& ep : eps) {
        loopback = loopback || ep.to_string().find("127.0.0.1") == 0 ||
                   ep.to_string().find("[::1]") == 0;
        CHECK(ep.port() == 8080);
    }
    CHECK(loopback);
}

TEST_CASE("net::resolve: bad host reports an error") {
    io_context ctx;
    blocking_pool pool(ctx);

    bool threw = false;
    try {
        auto r = ex::sync_wait(ctx, net::resolve(pool, "no.such.host.invalid.", "80"));
        (void)r;
    } catch (const std::system_error&) {
        threw = true;
    }
    CHECK(threw);
}
