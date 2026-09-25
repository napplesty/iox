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
    std::array<std::byte, 4096> buffer{};
    auto read_result = ex::sync_wait(plain, io::read_at(plain, inert, wbytes{buffer.data(), buffer.size()},
                                        uoffset_t{0}));
    REQUIRE_FALSE(read_result);
    REQUIRE(read_result.error);
    CHECK(read_result.error->code() == EOPNOTSUPP);
}

TEST_CASE("nvme: read_at round-trips LBA0 bytes and is self-consistent") {
    NVME_OR_SKIP()
    io_context context{uring::ring_params{.sqe128 = true}};
    const std::size_t length = dev.lba_size() * 8;
    auto buffer1 = std::make_unique_for_overwrite<std::byte[]>(length);
    auto buffer2 = std::make_unique_for_overwrite<std::byte[]>(length);

    auto first_read = ex::sync_wait(context, io::read_at(context, dev, wbytes{buffer1.get(), length}, uoffset_t{0}));
    REQUIRE(first_read);
    CHECK(std::get<0>(*first_read) == length);

    auto second_read = ex::sync_wait(context, io::read_at(context, dev, wbytes{buffer2.get(), length}, uoffset_t{0}));
    REQUIRE(second_read);
    CHECK(std::get<0>(*second_read) == length);
    CHECK(std::memcmp(buffer1.get(), buffer2.get(), length) == 0);
}

TEST_CASE("nvme: byte/LBA misalignment is a typed EINVAL, zero is a value 0") {
    NVME_OR_SKIP()
    io_context context{uring::ring_params{.sqe128 = true}};
    std::array<std::byte, 4096> buffer{};
    const auto lba_size = dev.lba_size();

    auto misaligned_offset_result = ex::sync_wait(context, io::read_at(context, dev, wbytes{buffer.data(), buffer.size()},
                                                  uoffset_t{lba_size + 1}));
    REQUIRE_FALSE(misaligned_offset_result);
    REQUIRE(misaligned_offset_result.error);
    CHECK(misaligned_offset_result.error->code() == EINVAL);

    auto misaligned_length_result = ex::sync_wait(context, io::read_at(context, dev,
                                                  wbytes{buffer.data(), lba_size + 1}, uoffset_t{0}));
    REQUIRE_FALSE(misaligned_length_result);
    CHECK(misaligned_length_result.error->code() == EINVAL);

    auto zero_length_result = ex::sync_wait(context, io::read_at(context, dev, wbytes{buffer.data(), 0}, uoffset_t{0}));
    REQUIRE(zero_length_result);
    CHECK(std::get<0>(*zero_length_result) == 0);
}

TEST_CASE("nvme: fsync is a CACHE FLUSH passthu (or a typed refusal)") {
    NVME_OR_SKIP()
    io_context context{uring::ring_params{.sqe128 = true}};
    auto result = ex::sync_wait(context, io::fsync(context, dev));
    if (!result) {
        REQUIRE(result.error);
        CHECK(result.error->code() == EACCES);
    }
}

TEST_CASE("nvme: io::close clears the device's fd slot on completion") {
    NVME_OR_SKIP()
    io_context context{uring::ring_params{.sqe128 = true}};
    auto close_result = ex::sync_wait(context, io::close(context, dev));
    REQUIRE(close_result);
    CHECK_FALSE(dev.valid());
}

#ifdef IOX_NVME_TEST_WRITE
TEST_CASE("nvme: write_at round-trip on the LAST LBA (explicit opt-in only)") {
    NVME_OR_SKIP()
    io_context context{uring::ring_params{.sqe128 = true}};
    const std::size_t length = dev.lba_size();
    const std::uint64_t offset = dev.size_bytes() - length;
    auto source = std::make_unique_for_overwrite<std::byte[]>(length);
    auto back = std::make_unique_for_overwrite<std::byte[]>(length);
    for (std::size_t index = 0; index < length; ++index) {
        source[index] = static_cast<std::byte>(0x5A ^ (index & 0xFF));
    }

    auto write_result = ex::sync_wait(context, io::write_at(context, dev, rbytes{source.get(), length}, uoffset_t{offset}));
    REQUIRE(write_result);
    CHECK(std::get<0>(*write_result) == length);

    auto fsync_result = ex::sync_wait(context, io::fsync(context, dev));
    REQUIRE(fsync_result);

    auto read_result = ex::sync_wait(context, io::read_at(context, dev, wbytes{back.get(), length}, uoffset_t{offset}));
    REQUIRE(read_result);
    CHECK(std::memcmp(source.get(), back.get(), length) == 0);
}
#endif

TEST_CASE("nvme: capability answers — device DMA is zero-copy direct") {
    const nvme::device* device = nullptr;
    CHECK(io::supports(io::zero_copy, *device));
    CHECK(io::supports(io::dma, *device));
    CHECK_FALSE(io::supports(io::mmap, *device));
}

TEST_CASE("nvme: passthru node names map to their sysfs block entries") {
    CHECK(std::string{nvme::block_suffix("ng0n1")} == "0n1");
    CHECK(std::string{nvme::block_suffix("nvme0n1")} == "0n1");
    CHECK(std::string{nvme::block_suffix("nvme0")} == "0");
    CHECK(std::string{nvme::block_suffix("sda")} == "sda");
}
