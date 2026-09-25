// iox — unified async IO for Linux
// tests/test_ring.cc — io_context. Real kernel round trips (nop), flush semantics, move behavior.
#include <doctest/doctest.h>

#include <iox/runtime/io_context.h>

using namespace iox;

namespace {

struct nop_op final : op_base {
    int fired = 0;
    std::int32_t last_result = -1;

    static void thunk(op_base* self, io_context&, std::int32_t result, std::uint32_t) noexcept {
        auto* operation = static_cast<nop_op*>(self);
        ++operation->fired;
        operation->last_result = result;
    }

    nop_op() noexcept : op_base(&nop_op::thunk) {}
};

}

TEST_CASE("ring initializes") {
    uring::ring ring{uring::ring_params{.entries = 64}};
    CHECK(ring.ok());
}

TEST_CASE("nop round trip through the kernel") {
    uring::ring ring{uring::ring_params{.entries = 64}};
    REQUIRE(ring.ok());

    nop_op op;
    io_uring_sqe* sqe = ring.next_sqe();
    REQUIRE(sqe != nullptr);
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data(sqe, &op);

    CHECK(ring.enters() == 0);
    REQUIRE(ring.flush_and_wait(1) >= 0);
    CHECK(ring.enters() == 1);

    unsigned seen = 0;
    ring.for_each_cqe([&](io_uring_cqe* cqe) {
        ++seen;
        auto* target = static_cast<nop_op*>(::io_uring_cqe_get_data(cqe));
        ++target->fired;
        target->last_result = cqe->res;
    });
    CHECK(seen == 1);
    CHECK(op.fired == 1);
    CHECK(op.last_result == 0);
    CHECK(ring.cq_ready() == 0);
}

TEST_CASE("flush with nothing pending is a no-op submit") {
    uring::ring ring{uring::ring_params{.entries = 64}};
    REQUIRE(ring.ok());
    CHECK(ring.flush() == 0);
}

TEST_CASE("cq_entries override is honored") {
    uring::ring ring{uring::ring_params{.entries = 64, .cq_entries = 1024}};
    CHECK(ring.ok());
}

TEST_CASE("ring move transfers ownership") {
    uring::ring moved_from{uring::ring_params{.entries = 64}};
    REQUIRE(moved_from.ok());
    uring::ring moved_to{std::move(moved_from)};
    CHECK(!moved_from.ok());
    CHECK(moved_to.ok());

    nop_op op;
    io_uring_sqe* sqe = moved_to.next_sqe();
    REQUIRE(sqe != nullptr);
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data(sqe, &op);
    REQUIRE(moved_to.flush_and_wait(1) >= 0);
    moved_to.for_each_cqe([&](io_uring_cqe* cqe) {
        auto* target = static_cast<nop_op*>(::io_uring_cqe_get_data(cqe));
        ++target->fired;
    });
    CHECK(op.fired == 1);
}
