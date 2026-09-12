// iox — unified async IO for Linux
// tests/test_capabilities.cc — the
#include <doctest/doctest.h>

#include <iox/core/buffer.h>
#include <iox/core/concepts.h>
#include <iox/core/exec.h>
#include <iox/fs/file.h>
#include <iox/ops.h>
#include <iox/pipe/channel.h>
#include <iox/stdio/stream.h>
#include <iox/core/units.h>

using namespace iox;

static_assert(io::readable<fs::file> && io::writable<fs::file> && io::seekable<fs::file>);
static_assert(io::readable<pipe::read_end> && !io::writable<pipe::read_end>);
static_assert(!io::readable<pipe::write_end> && io::writable<pipe::write_end>);
static_assert(io::readable<in_channel> && !io::writable<in_channel>);
static_assert(!io::readable<out_channel> && io::writable<out_channel>);
static_assert(!io::seekable<pipe::read_end> && !io::seekable<out_channel>);

template <class CPO, class Void, class...>
struct cpo_callable : std::false_type {};
template <class CPO, class... As>
struct cpo_callable<CPO, std::void_t<decltype(std::declval<const CPO&>()(
                                 std::declval<As>()...))>, As...> : std::true_type {};
template <class CPO, class... As>
inline constexpr bool cpo_callable_v = cpo_callable<CPO, void, As...>::value;

static_assert(!cpo_callable_v<io::read_t, io_context&, pipe::write_end&, iox::wbytes>);
static_assert(!cpo_callable_v<io::write_t, io_context&, pipe::read_end&, iox::rbytes>);
static_assert(!cpo_callable_v<io::write_t, io_context&, in_channel, iox::rbytes>);
static_assert(!cpo_callable_v<io::read_at_t, io_context&, pipe::read_end&, iox::wbytes,
                              iox::uoffset_t>);
static_assert(!cpo_callable_v<io::close_t, io_context&, out_channel&>);
static_assert(!cpo_callable_v<io::read_at_t, io_context&, fs::file&, iox::wbytes,
                              iox::io_size_t>);

static_assert(requires(io_context& ctx, pipe::read_end& r, pipe::write_end& w,
                       fs::file& f, iox::wbytes wb, iox::rbytes rb, iox::uoffset_t o,
                       iox::registered_buffer& reg) {
    io::read(ctx, r, wb);
    io::write(ctx, w, rb);
    io::read(ctx, f, wb);
    io::read_at(ctx, f, wb, o);
    io::write_at(ctx, f, rb, o);
    io::read(ctx, std_in, wb);
    io::write(ctx, std_out, rb);
    io::fsync(ctx, f);
    io::close(ctx, f);
    io::close(ctx, w);
    io::read(ctx, r, reg);
    io::write(ctx, w, reg);
});

TEST_CASE("capability proofs are compile-time contracts") {
    CHECK(true);
}
