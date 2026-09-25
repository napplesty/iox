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

static_assert(requires(io_context& context, pipe::read_end& reader, pipe::write_end& writer,
                       fs::file& file, iox::wbytes write_buffer, iox::rbytes read_buffer, iox::uoffset_t offset,
                       iox::registered_buffer& registered) {
    io::read(context, reader, write_buffer);
    io::write(context, writer, read_buffer);
    io::read(context, file, write_buffer);
    io::read_at(context, file, write_buffer, offset);
    io::write_at(context, file, read_buffer, offset);
    io::read(context, std_in, write_buffer);
    io::write(context, std_out, read_buffer);
    io::fsync(context, file);
    io::close(context, file);
    io::close(context, writer);
    io::read(context, reader, registered);
    io::write(context, writer, registered);
});

TEST_CASE("capability proofs are compile-time contracts") {
    CHECK(true);
}
