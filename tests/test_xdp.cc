// iox — unified async IO for Linux
// tests/test_xdp.cc — on the dev box, so every case self-skips with the reason when creation is
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>

#include <iox/core/exec.h>
#include <iox/ops.h>
#include <iox/xdp/socket.h>
#include <iox/xdp/read.h>
#include <iox/xdp/write_frame.h>

namespace ex = iox::exec;
using namespace std::chrono_literals;
using namespace iox;

static std::expected<xdp::umem, error> make_umem() {
    return xdp::umem::create(64, 4096);
}

TEST_CASE("xdp: umem allocates the shared packet memory") {
    auto m = make_umem();
    REQUIRE(m);
    CHECK(m->data() != nullptr);
    CHECK(m->chunk_count() == 64);
    CHECK(m->chunk_size() == 4096);
}

TEST_CASE("xdp: socket create + bind + TX round through the completion source") {
    auto mem = make_umem();
    REQUIRE(mem);
    auto xsk = xdp::socket::create(*mem, "lo", 0, 64);
    if (!xsk) {
        MESSAGE("skip: AF_XDP unavailable (" << xsk.error().message() << ")");
        return;
    }
    REQUIRE(xsk->valid());

    io_context ctx;
    ctx.attach_source(*xsk);

    xdp::frame f = xsk->tx_frame();
    REQUIRE(f.data != nullptr);
    f.len = 64;
    std::memset(f.data, 0xAB, f.len);

    auto r = ex::sync_wait(ctx, xdp::write_frame(ctx, *xsk, f));
    REQUIRE(r);
    CHECK(std::get<0>(*r) == 64);

    ctx.detach_source(*xsk);
    ctx.run_for(20ms);
}

TEST_CASE("xdp: capability answers and registration") {
    const xdp::socket* s = nullptr;
    CHECK(io::supports(io::dma, *s));
    CHECK_FALSE(io::supports(io::zero_copy, *s));
    static_assert(iox::driver::registered_driver<xdp::socket>);
}
