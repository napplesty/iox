// iox — unified async IO for Linux
// tests/test_xdp.cc — on the dev box, so every case self-skips with the reason when creation is
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

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
    auto memory = make_umem();
    REQUIRE(memory);
    CHECK(memory->data() != nullptr);
    CHECK(memory->chunk_count() == 64);
    CHECK(memory->chunk_size() == 4096);
}

TEST_CASE("xdp: socket create + bind + TX round through the completion source") {
    auto memory = make_umem();
    REQUIRE(memory);
    auto xsk = xdp::socket::create(*memory, "lo", 0, 64);
    if (!xsk) {
        MESSAGE("skip: AF_XDP unavailable (" << xsk.error().message() << ")");
        return;
    }
    REQUIRE(xsk->valid());

    io_context context;
    context.attach_source(*xsk);

    auto frame = xsk->tx_frame();
    REQUIRE(frame);
    frame->len = 64;
    std::memset(frame->data, 0xAB, frame->len);

    auto result = ex::sync_wait(context, xdp::write_frame(context, *xsk, *frame));
    REQUIRE(result);
    CHECK(std::get<0>(*result) == 64);

    context.detach_source(*xsk);
    context.run_for(20ms);
}

TEST_CASE("xdp: initial_fill splits the pool between RX and TX") {
    CHECK(xdp::detail::initial_fill(64, 64) == 32); // even split under capacity
    CHECK(xdp::detail::initial_fill(1, 64) == 1);   // a single chunk still receives
    CHECK(xdp::detail::initial_fill(8, 4) == 4);    // clamped to ring capacity
    CHECK(xdp::detail::initial_fill(8, 2) == 2);    // capacity smaller than the share
}

TEST_CASE("xdp: refill_fill_ring moves pool chunks into the fill ring") {
    std::uint32_t producer = 0, consumer = 0;
    std::uint64_t descriptors[8]{};
    xdp::detail::ring_view<std::uint64_t> fill{
        .producer = &producer, .consumer = &consumer, .flags = nullptr,
        .desc = descriptors, .mask = 7, .cached_prod = 0};
    std::vector<std::uint64_t> pool{100, 200, 300};

    xdp::detail::refill_fill_ring(fill, pool, 8);
    CHECK(producer == 3);
    CHECK(pool.empty());
    CHECK(descriptors[0] == 300); // pool back is moved first
    CHECK(descriptors[1] == 200);
    CHECK(descriptors[2] == 100);
    CHECK(fill.cached_prod == 3);

    consumer = 3; // the kernel consumed all three
    pool.push_back(400);
    xdp::detail::refill_fill_ring(fill, pool, 8);
    CHECK(producer == 4);
    CHECK(descriptors[3] == 400);
    CHECK(pool.empty());

    producer = consumer + 8; // ring full: nothing moves even with chunks available
    fill.cached_prod = producer;
    pool.push_back(500);
    xdp::detail::refill_fill_ring(fill, pool, 8);
    CHECK(producer == consumer + 8);
    CHECK(pool.size() == 1);
}

TEST_CASE("xdp: capability answers and registration") {
    const xdp::socket* socket = nullptr;
    CHECK(io::supports(io::dma, *socket));
    CHECK_FALSE(io::supports(io::zero_copy, *socket));
    static_assert(iox::driver::registered_driver<xdp::socket>);
}
