// iox — unified async IO for Linux
// python/iox.cc — nanobind bindings: a synchronous, GIL-releasing Python API
// over the unified vocabulary. Every operation goes through io_uring; the
// GIL is dropped for the duration of the wait so worker threads keep
// running. ADR-0012 records the design.
//
//     import iox
//     ctx  = iox.Context()
//     f    = iox.open_file(ctx, "/tmp/x", iox.Mode.rw | iox.Mode.create)
//     f.write_at(b"hello", 0); f.fsync(); f.close()
//     s    = iox.connect(ctx, "127.0.0.1", 9000)
//     s.send(b"hi"); s.recv(4096)
// Python.h must come first: it owns the feature-test macros
// (_POSIX_C_SOURCE/_XOPEN_SOURCE) that libstdc++ headers also define.
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include <iox/iox.h>

namespace nb = nanobind;
using namespace iox;

namespace {

// ---- error translation ------------------------------------------------------

[[noreturn]] void raise(const error& e) {
    PyObject* args = Py_BuildValue("(is)", e.code(), e.message().c_str());
    PyErr_SetObject(PyExc_OSError, args); // OSError(errno, message)
    Py_XDECREF(args);
    throw nb::python_error();
}

// Drive one sender to completion with the GIL released, translating the
// sync_wait_result into values / OSError / RuntimeError. The release scope
// ends before any Python C-API runs again.
template <class Sndr>
auto wait(io_context& ctx, Sndr&& s) {
    auto res = [&] {
        nb::gil_scoped_release unheld;
        return iox::exec::sync_wait(ctx, std::forward<Sndr>(s));
    }();
    if (res.stopped) {
        throw nb::value_error("operation stopped before completion");
    }
    if (!res) {
        raise(*res.error);
    }
    return std::move(*res.value);
}

// ---- handles ----------------------------------------------------------------

struct PyIOContext {
    io_context ctx;
};

struct PyFile {
    PyIOContext* owner; // kept alive through nanobind keep_alive chains
    fs::file f;

    explicit PyFile(PyIOContext& c, fs::file file) : owner(&c), f(std::move(file)) {}
};

struct PySocket {
    PyIOContext* owner;
    net::tcp::socket s;

    explicit PySocket(PyIOContext& c, net::tcp::socket socket) : owner(&c), s(std::move(socket)) {}
};

struct PyListener {
    PyIOContext* owner;
    net::tcp::acceptor acceptor;

    explicit PyListener(PyIOContext& c, net::tcp::acceptor a) : owner(&c), acceptor(std::move(a)) {}
};

// Read `size` bytes through `submit(dest) -> byte count` into a freshly
// allocated Python bytes object, truncated to what arrived. The kernel
// writes straight into the object's buffer — one copy total (a std::string
// staging buffer would make it two).
template <class Submit>
nb::bytes read_into_bytes(std::size_t size, Submit submit) {
    PyObject* py = PyBytes_FromStringAndSize(nullptr, size);
    if (py == nullptr) {
        throw nb::python_error();
    }
    const std::size_t n = submit({PyBytes_AS_STRING(py), size});
    if (n != size && _PyBytes_Resize(&py, n) != 0) {
        throw nb::python_error();
    }
    return nb::steal<nb::bytes>(py);
}

// ---- file ops ----------------------------------------------------------------

// mode arrives as unsigned: Python's ``Mode.rw | Mode.create`` yields int.
PyFile open_file(PyIOContext& c, const std::string& path, unsigned m) {
    auto f = [&] { // open(2) can block on network filesystems
        nb::gil_scoped_release unheld;
        return fs::file::open(path.c_str(), static_cast<fs::mode>(m));
    }();
    if (!f) {
        raise(f.error());
    }
    return PyFile{c, std::move(*f)};
}

std::size_t file_write_at(PyFile& self, nb::bytes data, std::uint64_t offset) {
    std::string buf(data.c_str(), data.size()); // detach from Python memory
    return std::get<0>(wait(self.owner->ctx,
                            io::write_at(self.owner->ctx, self.f,
                                         rbytes{as_rbytes(std::span<const char>{buf})},
                                         uoffset_t{offset})));
}

nb::bytes file_read_at(PyFile& self, std::size_t size, std::uint64_t offset) {
    return read_into_bytes(size, [&](std::span<char> dest) {
        return std::get<0>(wait(self.owner->ctx,
                                io::read_at(self.owner->ctx, self.f,
                                            wbytes{as_wbytes(dest)},
                                            uoffset_t{offset})));
    });
}

std::size_t file_write(PyFile& self, nb::bytes data) {
    std::string buf(data.c_str(), data.size());
    return std::get<0>(wait(self.owner->ctx,
                            io::write(self.owner->ctx, self.f,
                                      rbytes{as_rbytes(std::span<const char>{buf})})));
}

nb::bytes file_read(PyFile& self, std::size_t size) {
    return read_into_bytes(size, [&](std::span<char> dest) {
        return std::get<0>(wait(self.owner->ctx,
                                io::read(self.owner->ctx, self.f,
                                         wbytes{as_wbytes(dest)})));
    });
}

void file_fsync(PyFile& self) {
    wait(self.owner->ctx, io::fsync(self.owner->ctx, self.f));
}

void file_close(PyFile& self) {
    if (self.f.valid()) {
        wait(self.owner->ctx, io::close(self.owner->ctx, self.f));
        self.f = fs::file{}; // moved-out state: destructor must not re-close
    }
}

// ---- tcp ops ------------------------------------------------------------------

PySocket tcp_connect(PyIOContext& c, const std::string& host, std::uint16_t port) {
    auto endpoint = net::endpoint::parse(host + ":" + std::to_string(port));
    if (!endpoint) {
        raise(endpoint.error());
    }
    auto socket = net::tcp::socket::unconnected(endpoint->family());
    if (!socket) {
        raise(socket.error());
    }
    wait(c.ctx, io::connect(c.ctx, *socket, *endpoint));
    return PySocket{c, std::move(*socket)};
}

PyListener tcp_listen(PyIOContext& c, const std::string& host, std::uint16_t port, int backlog) {
    auto endpoint = net::endpoint::parse(host + ":" + std::to_string(port));
    if (!endpoint) {
        raise(endpoint.error());
    }
    auto acceptor = [&] {
        nb::gil_scoped_release unheld;
        return net::tcp::acceptor::listen(*endpoint, backlog);
    }();
    if (!acceptor) {
        raise(acceptor.error());
    }
    return PyListener{c, std::move(*acceptor)};
}

PySocket listener_accept(PyListener& self) {
    auto socket = std::get<0>(wait(self.owner->ctx, io::accept(self.owner->ctx, self.acceptor)));
    return PySocket{*self.owner, std::move(socket)};
}

std::size_t sock_send(PySocket& self, nb::bytes data) {
    std::string buf(data.c_str(), data.size());
    wait(self.owner->ctx,
         io::write_all(self.owner->ctx, self.s,
                       rbytes{as_rbytes(std::span<const char>{buf})}));
    return buf.size(); // write_all: everything or OSError
}

nb::bytes sock_recv(PySocket& self, std::size_t size) {
    return read_into_bytes(size, [&](std::span<char> dest) {
        return std::get<0>(wait(self.owner->ctx,
                                io::read(self.owner->ctx, self.s,
                                         wbytes{as_wbytes(dest)})));
    }); // n == 0 is EOF: b""
}

void sock_close(PySocket& self) {
    if (self.s.valid()) {
        wait(self.owner->ctx, io::close(self.owner->ctx, self.s));
        self.s = net::tcp::socket{};
    }
}

// ---- misc ----------------------------------------------------------------------

void sleep_for(PyIOContext& c, double seconds) {
    if (seconds < 0) {
        throw nb::value_error("sleep duration must be non-negative");
    }
    wait(c.ctx, io::sleep_for(c.ctx, std::chrono::nanoseconds{
                                         static_cast<std::int64_t>(seconds * 1e9)}));
}

} // namespace

NB_MODULE(iox, m) {
    m.doc() = "iox — unified async IO (io_uring) with a synchronous, GIL-releasing "
              "Python surface.\n\n"
              "One Context per thread: contexts (and the handles bound to them) "
              "must not be shared across threads — the ring and its operation "
              "state are single-threaded by design. Every call blocks only the "
              "calling thread; the GIL is released while waiting.";    nb::class_<PyIOContext>(m, "Context", "Owns one io_context (one io_uring instance).")
        .def(nb::init<>());

    nb::enum_<fs::mode>(m, "Mode", nb::is_arithmetic())
        .value("read", fs::mode::read)
        .value("write", fs::mode::write)
        .value("rw", fs::mode::rw)
        .value("create", fs::mode::create)
        .value("truncate", fs::mode::truncate)
        .value("append", fs::mode::append)
        .value("direct", fs::mode::direct);

    m.def("open_file", &open_file, nb::arg("ctx"), nb::arg("path"),
          nb::arg("mode") = static_cast<unsigned>(fs::mode::read), nb::keep_alive<0, 1>(),
          "Synchronously open a file; the data path runs through io_uring.");

    nb::class_<PyFile>(m, "File")
        .def("read_at", &file_read_at, nb::arg("size"), nb::arg("offset"))
        .def("write_at", &file_write_at, nb::arg("data"), nb::arg("offset"))
        .def("read", &file_read, nb::arg("size"))
        .def("write", &file_write, nb::arg("data"))
        .def("fsync", &file_fsync)
        .def("close", &file_close)
        .def("valid", [](PyFile& self) { return self.f.valid(); });

    m.def("connect", &tcp_connect, nb::arg("ctx"), nb::arg("host"), nb::arg("port"),
          nb::keep_alive<0, 1>(), "Connect a TCP socket through io::connect.");
    m.def("listen", &tcp_listen, nb::arg("ctx"), nb::arg("host"), nb::arg("port"),
          nb::arg("backlog") = 128, nb::keep_alive<0, 1>(),
          "Create a TCP acceptor (blocking listen(2)).");

    nb::class_<PyListener>(m, "Listener")
        .def("accept", &listener_accept, nb::keep_alive<0, 1>());

    nb::class_<PySocket>(m, "TcpSocket")
        .def("send", &sock_send, nb::arg("data"))
        .def("recv", &sock_recv, nb::arg("size"))
        .def("close", &sock_close)
        .def("valid", [](PySocket& self) { return self.s.valid(); });

    m.def("sleep", &sleep_for, nb::arg("ctx"), nb::arg("seconds"),
          "Sleep through the ring (io_uring timeout, GIL released).");
}
