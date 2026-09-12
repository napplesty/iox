// iox — unified async IO for Linux
// tests/test_nvme.cc — (M6). The whole file self-skips when the passthru node is not accessible
#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <iox/core/exec.h>
#include <iox/nvme/device.h>
#include <iox/nvme/admin.h>
#include <iox/nvme/io.h>
#include <iox/ops.h>

using namespace std::chrono_literals;
namespace ex = iox::exec;
using namespace iox;

namespace {

std::expected<nvme::device, error> open_test_device() {
    const char* path = std::getenv("IOX_NVME_PATH");
    return nvme::device::open(path != nullptr ? path : "/dev/ng0n1");
}

#define NVME_OR_SKIP()                                                                        \
    auto dev_opt = open_test_device();                                                        \
    if (!dev_opt) {                                                                           \
        MESSAGE("skip: no NVMe passthru access (" << dev_opt.error().message() << ")");       \
        return;                                                                               \
    }                                                                                         \
    auto dev = std::move(*dev_opt);                                                           \
    REQUIRE(dev.valid());

}

TEST_CASE("nvme: probe loads geometry (nsid, lba size, capacity)") {
    NVME_OR_SKIP()
    CHECK(dev.nsid() > 0);
    CHECK(dev.lba_size() >= 512);
    CHECK(dev.lba_size() <= 16384);
    CHECK((dev.lba_size() & (dev.lba_size() - 1)) == 0);
    CHECK(dev.lba_count() > 0);
    CHECK(dev.size_bytes() > 1024ULL * 1024 * 1024);
}

TEST_CASE("nvme: ops on a non-sqe128 ring are a typed EOPNOTSUPP up front") {
    io_context plain;
    nvme::device inert; // invalid fd is fine: the op never reaches the kernel
    std::array<std::byte, 4096> buf{};
    auto r = ex::sync_wait(plain, io::read_at(plain, inert, wbytes{buf.data(), buf.size()},
                                        uoffset_t{0}));
    REQUIRE_FALSE(r);
    REQUIRE(r.error);
    CHECK(r.error->code() == EOPNOTSUPP);
}

TEST_CASE("nvme: read_at round-trips LBA0 bytes and is self-consistent") {
    NVME_OR_SKIP()
    io_context ctx{uring::ring_params{.sqe128 = true}};
    const std::size_t len = dev.lba_size() * 8;
    auto buf1 = std::make_unique_for_overwrite<std::byte[]>(len);
    auto buf2 = std::make_unique_for_overwrite<std::byte[]>(len);

    auto r1 = ex::sync_wait(ctx, io::read_at(ctx, dev, wbytes{buf1.get(), len}, uoffset_t{0}));
    REQUIRE(r1);
    CHECK(std::get<0>(*r1) == len);

    auto r2 = ex::sync_wait(ctx, io::read_at(ctx, dev, wbytes{buf2.get(), len}, uoffset_t{0}));
    REQUIRE(r2);
    CHECK(std::get<0>(*r2) == len);
    CHECK(std::memcmp(buf1.get(), buf2.get(), len) == 0);
}

TEST_CASE("nvme: byte/LBA misalignment is a typed EINVAL, zero is a value 0") {
    NVME_OR_SKIP()
    io_context ctx{uring::ring_params{.sqe128 = true}};
    std::array<std::byte, 4096> buf{};
    const auto lbs = dev.lba_size();

    auto mis_off = ex::sync_wait(ctx, io::read_at(ctx, dev, wbytes{buf.data(), buf.size()},
                                                  uoffset_t{lbs + 1}));
    REQUIRE_FALSE(mis_off);
    REQUIRE(mis_off.error);
    CHECK(mis_off.error->code() == EINVAL);

    auto mis_len = ex::sync_wait(ctx, io::read_at(ctx, dev,
                                                  wbytes{buf.data(), lbs + 1}, uoffset_t{0}));
    REQUIRE_FALSE(mis_len);
    CHECK(mis_len.error->code() == EINVAL);

    auto zero = ex::sync_wait(ctx, io::read_at(ctx, dev, wbytes{buf.data(), 0}, uoffset_t{0}));
    REQUIRE(zero);
    CHECK(std::get<0>(*zero) == 0);
}

TEST_CASE("nvme: fsync is a CACHE FLUSH passthu (or a typed refusal)") {
    NVME_OR_SKIP()
    io_context ctx{uring::ring_params{.sqe128 = true}};
    auto r = ex::sync_wait(ctx, io::fsync(ctx, dev));
    if (!r) {
        REQUIRE(r.error);
        CHECK(r.error->code() == EACCES);
    }
}

TEST_CASE("nvme: io::close clears the device's fd slot on completion") {
    NVME_OR_SKIP()
    io_context ctx{uring::ring_params{.sqe128 = true}};
    auto w = ex::sync_wait(ctx, io::close(ctx, dev));
    REQUIRE(w);
    CHECK_FALSE(dev.valid());
}

#ifdef IOX_NVME_TEST_WRITE
TEST_CASE("nvme: write_at round-trip on the LAST LBA (explicit opt-in only)") {
    NVME_OR_SKIP()
    io_context ctx{uring::ring_params{.sqe128 = true}};
    const std::size_t len = dev.lba_size();
    const std::uint64_t off = dev.size_bytes() - len;
    auto src = std::make_unique_for_overwrite<std::byte[]>(len);
    auto back = std::make_unique_for_overwrite<std::byte[]>(len);
    for (std::size_t i = 0; i < len; ++i) {
        src[i] = static_cast<std::byte>(0x5A ^ (i & 0xFF));
    }

    auto w = ex::sync_wait(ctx, io::write_at(ctx, dev, rbytes{src.get(), len}, uoffset_t{off}));
    REQUIRE(w);
    CHECK(std::get<0>(*w) == len);

    auto f = ex::sync_wait(ctx, io::fsync(ctx, dev));
    REQUIRE(f);

    auto r = ex::sync_wait(ctx, io::read_at(ctx, dev, wbytes{back.get(), len}, uoffset_t{off}));
    REQUIRE(r);
    CHECK(std::memcmp(src.get(), back.get(), len) == 0);
}
#endif

TEST_CASE("nvme: capability answers — device DMA is zero-copy direct") {
    const nvme::device* d = nullptr;
    CHECK(io::supports(io::zero_copy, *d));
    CHECK(io::supports(io::dma, *d));
    CHECK_FALSE(io::supports(io::mmap, *d));
}
