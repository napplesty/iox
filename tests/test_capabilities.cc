// Compile-time capability proofs (design §四.①): "types as lightweight
// formal verification". Every assertion here is a static_assert — the
// vocabulary refuses wrong-direction and wrong-capability misuse at compile
// time, and this file pins that contract down so refactors cannot silently
// loosen it.
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

// --- direction is a property of the type -------------------------------
static_assert(io::readable<fs::file> && io::writable<fs::file> && io::seekable<fs::file>);
static_assert(io::readable<pipe::read_end> && !io::writable<pipe::read_end>);
static_assert(!io::readable<pipe::write_end> && io::writable<pipe::write_end>);
static_assert(io::readable<in_channel> && !io::writable<in_channel>);
static_assert(!io::readable<out_channel> && io::writable<out_channel>);
static_assert(!io::seekable<pipe::read_end> && !io::seekable<out_channel>);

// --- the vocabulary has no call site for wrong-capability use -----------
// Callability detection. (The void_t idiom, not a bare requires-expression:
// GCC 15 reports "all candidates removed by constraints" inside a requires
// body as a hard error instead of a soft false.)
template <class CPO, class Void, class...>
struct cpo_callable : std::false_type {};
template <class CPO, class... As>
struct cpo_callable<CPO, std::void_t<decltype(std::declval<const CPO&>()(
                                 std::declval<As>()...))>, As...> : std::true_type {};
template <class CPO, class... As>
inline constexpr bool cpo_callable_v = cpo_callable<CPO, void, As...>::value;

// io::read on a write end? No overload exists.
static_assert(!cpo_callable_v<io::read_t, io_context&, pipe::write_end&, iox::wbytes>);
// io::write on a read end? Same.
static_assert(!cpo_callable_v<io::write_t, io_context&, pipe::read_end&, iox::rbytes>);
// io::write on stdin? No.
static_assert(!cpo_callable_v<io::write_t, io_context&, in_channel, iox::rbytes>);
// io::read_at on a pipe (not seekable)? No.
static_assert(!cpo_callable_v<io::read_at_t, io_context&, pipe::read_end&, iox::wbytes,
                              iox::uoffset_t>);
// io::close on a stdio view (not owned)? No.
static_assert(!cpo_callable_v<io::close_t, io_context&, out_channel&>);
// Swapping offset and size does not compile: uoffset_t is a distinct type.
static_assert(!cpo_callable_v<io::read_at_t, io_context&, fs::file&, iox::wbytes,
                              iox::io_size_t>);

// --- the right calls do exist -------------------------------------------
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
    io::read(ctx, r, reg);   // registered zero-copy path
    io::write(ctx, w, reg);
});

TEST_CASE("capability proofs are compile-time contracts") {
    CHECK(true); // all proofs are static_asserts above
}
