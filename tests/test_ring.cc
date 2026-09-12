// Unit tests for the liburing wrapper: raw ring mechanics without
// io_context. Real kernel round trips (nop), flush semantics, move behavior.
#include <doctest/doctest.h>

#include <iox/runtime/io_context.h>

using namespace iox;

namespace {

struct nop_op final : op_base {
    int fired = 0;
    std::int32_t last_res = -1;

    static void thunk(op_base* self, io_context&, std::int32_t res, std::uint32_t) noexcept {
        auto* o = static_cast<nop_op*>(self);
        ++o->fired;
        o->last_res = res;
    }

    nop_op() noexcept : op_base(&nop_op::thunk) {}
};

} // namespace

TEST_CASE("ring initializes") {
    uring::ring r{uring::ring_params{.entries = 64}};
    CHECK(r.ok());
}

TEST_CASE("nop round trip through the kernel") {
    uring::ring r{uring::ring_params{.entries = 64}};
    REQUIRE(r.ok());

    nop_op op;
    io_uring_sqe* sqe = r.next_sqe();
    REQUIRE(sqe != nullptr);
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data(sqe, &op);

    CHECK(r.enters() == 0);
    REQUIRE(r.flush_and_wait(1) >= 0);
    CHECK(r.enters() == 1);

    unsigned seen = 0;
    r.for_each_cqe([&](io_uring_cqe* cqe) {
        ++seen;
        auto* target = static_cast<nop_op*>(::io_uring_cqe_get_data(cqe));
        ++target->fired;
        target->last_res = cqe->res;
    });
    CHECK(seen == 1);
    CHECK(op.fired == 1);
    CHECK(op.last_res == 0); // nop completes with 0
    CHECK(r.cq_ready() == 0);
}

TEST_CASE("flush with nothing pending is a no-op submit") {
    uring::ring r{uring::ring_params{.entries = 64}};
    REQUIRE(r.ok());
    CHECK(r.flush() == 0);
}

TEST_CASE("cq_entries override is honored") {
    uring::ring r{uring::ring_params{.entries = 64, .cq_entries = 1024}};
    CHECK(r.ok());
}

TEST_CASE("ring move transfers ownership") {
    uring::ring a{uring::ring_params{.entries = 64}};
    REQUIRE(a.ok());
    uring::ring b{std::move(a)};
    CHECK(!a.ok());
    CHECK(b.ok());

    // The moved-from ring must not tear down the kernel state on destruction
    // (verified by b still working):
    nop_op op;
    io_uring_sqe* sqe = b.next_sqe();
    REQUIRE(sqe != nullptr);
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data(sqe, &op);
    REQUIRE(b.flush_and_wait(1) >= 0);
    b.for_each_cqe([&](io_uring_cqe* cqe) {
        auto* target = static_cast<nop_op*>(::io_uring_cqe_get_data(cqe));
        ++target->fired;
    });
    CHECK(op.fired == 1);
}
