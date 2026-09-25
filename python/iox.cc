// iox — python/iox.cc: nanobind bindings, a synchronous GIL-releasing Python API.
#include <nanobind/nanobind.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <iox/iox.h>

#include "async_api.h"

namespace nb = nanobind;
using namespace iox;

namespace {

[[noreturn]] void raise(const error& err) {
    PyObject* args = Py_BuildValue("(is)", err.code(), err.message().c_str());
    PyErr_SetObject(PyExc_OSError, args);
    Py_XDECREF(args);
    throw nb::python_error();
}

template <class Sender>
auto wait(io_context& context, Sender&& sender) {
    auto result = [&] {
        nb::gil_scoped_release unheld;
        return iox::exec::sync_wait(context, std::forward<Sender>(sender));
    }();
    if (result.stopped) {
        throw nb::value_error("operation stopped before completion");
    }
    if (!result) {
        raise(*result.error);
    }
    return std::move(*result.value);
}

struct PyIOContext {
    io_context context;
};

struct PyFile {
    PyIOContext* owner;
    fs::file file;

    explicit PyFile(PyIOContext& owner, fs::file file) : owner(&owner), file(std::move(file)) {}
};

struct PySocket {
    PyIOContext* owner;
    net::tcp::socket socket;

    explicit PySocket(PyIOContext& owner, net::tcp::socket socket) : owner(&owner), socket(std::move(socket)) {}
};

struct PyListener {
    PyIOContext* owner;
    net::tcp::acceptor acceptor;

    explicit PyListener(PyIOContext& owner, net::tcp::acceptor acceptor) : owner(&owner), acceptor(std::move(acceptor)) {}
};

nb::object bytes_resize(nb::object py_bytes, std::size_t size) {
    if (static_cast<std::size_t>(PyBytes_GET_SIZE(py_bytes.ptr())) == size) {
        return py_bytes;
    }
    PyObject* raw = py_bytes.release().ptr(); // _PyBytes_Resize consumes it on failure
    if (_PyBytes_Resize(&raw, static_cast<Py_ssize_t>(size)) != 0) {
        throw nb::python_error();
    }
    return nb::steal(raw);
}

template <class Submit>
nb::bytes read_into_bytes(std::size_t size, Submit submit) {
    nb::object py_bytes = nb::steal(PyBytes_FromStringAndSize(nullptr, size));
    if (!py_bytes.is_valid()) {
        throw nb::python_error();
    }
    const std::size_t transferred = submit({static_cast<char*>(PyBytes_AS_STRING(py_bytes.ptr())), size});
    return nb::steal<nb::bytes>(bytes_resize(std::move(py_bytes), transferred).release().ptr());
}

PyFile open_file(PyIOContext& owner, const std::string& path, unsigned mode) {
    auto file = [&] {
        nb::gil_scoped_release unheld;
        return fs::file::open(path.c_str(), static_cast<fs::mode>(mode));
    }();
    if (!file) {
        raise(file.error());
    }
    return PyFile{owner, std::move(*file)};
}

std::size_t file_write_at(PyFile& self, nb::bytes data, std::uint64_t offset) {
    // zero-copy: `data` pins the immutable bytes buffer for the whole call
    const char* buffer = data.c_str();
    const std::size_t total = data.size();
    std::size_t written = 0; // direct-mode files can short-write at block boundaries
    while (written < total) {
        const std::size_t transferred = std::get<0>(wait(self.owner->context,
                                                         io::write_at(self.owner->context, self.file,
                                                                      rbytes{as_rbytes(
                                                                          std::span<const char>{buffer + written,
                                                                                                total - written})},
                                                                      uoffset_t{offset + written})));
        if (transferred == 0) {
            raise(error::from_errno(EIO));
        }
        written += transferred;
    }
    return written;
}

nb::bytes file_read_at(PyFile& self, std::size_t size, std::uint64_t offset) {
    return read_into_bytes(size, [&](std::span<char> destination) {
        return std::get<0>(wait(self.owner->context,
                                io::read_at(self.owner->context, self.file,
                                            wbytes{as_wbytes(destination)},
                                            uoffset_t{offset})));
    });
}

template <class PyWrapper, auto Member>
std::size_t write_bytes(PyWrapper& self, nb::bytes data) {
    // zero-copy: `data` pins the immutable bytes buffer for the whole call
    wait(self.owner->context,
         io::write_all(self.owner->context, self.*Member,
                       rbytes{as_rbytes(std::span<const char>{data.c_str(), data.size()})}));
    return data.size();
}

template <class PyWrapper, auto Member>
nb::bytes read_bytes(PyWrapper& self, std::size_t size) {
    return read_into_bytes(size, [&](std::span<char> destination) {
        return std::get<0>(wait(self.owner->context,
                                io::read(self.owner->context, self.*Member,
                                         wbytes{as_wbytes(destination)})));
    });
}

template <class PyWrapper, auto Member>
std::size_t read_into_buffer(PyWrapper& self, nb::handle buffer) {
    Py_buffer view;
    if (PyObject_GetBuffer(buffer.ptr(), &view, PyBUF_SIMPLE | PyBUF_WRITABLE) < 0) {
        throw nb::python_error();
    }
    try {
        const std::size_t transferred = std::get<0>(wait(self.owner->context,
                                                         io::read(self.owner->context, self.*Member,
                                                                  wbytes{static_cast<std::byte*>(view.buf),
                                                                         static_cast<std::size_t>(view.len)})));
        PyBuffer_Release(&view);
        return transferred;
    } catch (...) {
        PyBuffer_Release(&view);
        throw;
    }
}

struct read_many_sync_state {
    io_context* context;
    std::size_t remaining;
    std::optional<error> first_error;
};

struct read_many_sync_receiver {
    using receiver_concept = stdexec::receiver_tag;
    read_many_sync_state* state;
    std::size_t* count_slot;

    void finish_one() const noexcept {
        if (--state->remaining == 0) {
            state->context->stop();
        }
    }
    void set_value(std::size_t transferred) && noexcept {
        *count_slot = transferred;
        finish_one();
    }
    void set_error(error err) && noexcept {
        if (!state->first_error) {
            state->first_error = err;
        }
        finish_one();
    }
    void set_error(std::exception_ptr) && noexcept {
        if (!state->first_error) {
            state->first_error = error::from_errno(EIO);
        }
        finish_one();
    }
    void set_stopped() && noexcept {
        if (!state->first_error) {
            state->first_error = error::from_errno(ECANCELED);
        }
        finish_one();
    }
};

nb::list file_read_at_many(PyFile& self, const nb::list& specs) {
    using sender_t = decltype(io::read_at(std::declval<io_context&>(), std::declval<fs::file&>(),
                                          wbytes{}, uoffset_t{}));
    using operation_t = decltype(stdexec::connect(std::declval<sender_t>(),
                                                  std::declval<read_many_sync_receiver>()));
    const std::size_t count = nb::len(specs);
    read_many_sync_state state{&self.owner->context, count, std::nullopt};
    std::vector<std::size_t> counts(count, 0);
    std::vector<nb::object> buffers;
    std::vector<std::unique_ptr<operation_t>> operations;
    buffers.reserve(count);
    operations.reserve(count);
    std::size_t index = 0;
    for (nb::handle item : specs) {
        const auto [size, offset] = nb::cast<std::pair<std::size_t, std::uint64_t>>(item);
        nb::object buffer = nb::steal(PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size)));
        if (!buffer.is_valid()) {
            throw nb::python_error();
        }
        operations.push_back(std::make_unique<operation_t>(stdexec::connect(
            io::read_at(self.owner->context, self.file,
                        wbytes{as_wbytes(std::span<char>{PyBytes_AS_STRING(buffer.ptr()), size})},
                        uoffset_t{offset}),
            read_many_sync_receiver{&state, &counts[index]})));
        buffers.push_back(std::move(buffer));
        ++index;
    }
    {
        nb::gil_scoped_release unheld;
        {
            batch_scope batch(self.owner->context); // one io_uring_enter for the whole gather
            for (auto& operation : operations) {
                stdexec::start(*operation);
            }
        }
        while (state.remaining != 0) {
            self.owner->context.restart();
            self.owner->context.run();
        }
        self.owner->context.restart();
    }
    if (state.first_error) {
        raise(*state.first_error);
    }
    nb::list result;
    for (index = 0; index < count; ++index) {
        result.append(bytes_resize(std::move(buffers[index]), counts[index]));
    }
    return result;
}

void file_fsync(PyFile& self) {
    wait(self.owner->context, io::fsync(self.owner->context, self.file));
}

template <class PyWrapper, auto Member>
void close_handle(PyWrapper& self) {
    auto& handle = self.*Member;
    if (handle.valid()) {
        wait(self.owner->context, io::close(self.owner->context, handle));
        handle = std::remove_cvref_t<decltype(handle)>{}; // moved-out state: destructor must not re-close
    }
}

PySocket tcp_connect(PyIOContext& owner, const std::string& host, std::uint16_t port) {
    auto endpoint = net::endpoint::parse(host + ":" + std::to_string(port));
    if (!endpoint) {
        raise(endpoint.error());
    }
    auto socket = net::tcp::socket::unconnected(endpoint->family());
    if (!socket) {
        raise(socket.error());
    }
    wait(owner.context, io::connect(owner.context, *socket, *endpoint));
    return PySocket{owner, std::move(*socket)};
}

PyListener tcp_listen(PyIOContext& owner, const std::string& host, std::uint16_t port, int backlog) {
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
    return PyListener{owner, std::move(*acceptor)};
}

PySocket listener_accept(PyListener& self) {
    auto socket = std::get<0>(wait(self.owner->context, io::accept(self.owner->context, self.acceptor)));
    return PySocket{*self.owner, std::move(socket)};
}

void sleep_for(PyIOContext& owner, double seconds) {
    if (seconds < 0) {
        throw nb::value_error("sleep duration must be non-negative");
    }
    wait(owner.context, io::sleep_for(owner.context, std::chrono::nanoseconds{
                                                         static_cast<std::int64_t>(seconds * 1e9)}));
}

}

NB_MODULE(iox, module_) {
    module_.doc() = "iox — unified async IO (io_uring) with a synchronous, GIL-releasing "
              "Python surface.\n\n"
              "One Context per thread: contexts (and the handles bound to them) "
              "must not be shared across threads — the ring and its operation "
              "state are single-threaded by design. Every call blocks only the "
              "calling thread; the GIL is released while waiting.";    nb::class_<PyIOContext>(module_, "Context", "Owns one io_context (one io_uring instance).")
        .def(nb::init<>());

    nb::enum_<fs::mode>(module_, "Mode", nb::is_arithmetic())
        .value("read", fs::mode::read)
        .value("write", fs::mode::write)
        .value("rw", fs::mode::rw)
        .value("create", fs::mode::create)
        .value("truncate", fs::mode::truncate)
        .value("append", fs::mode::append)
        .value("direct", fs::mode::direct);

    module_.def("open_file", &open_file, nb::arg("context"), nb::arg("path"),
          nb::arg("mode") = static_cast<unsigned>(fs::mode::read), nb::keep_alive<0, 1>(),
          "Synchronously open a file; the data path runs through io_uring.");

    nb::class_<PyFile>(module_, "File")
        .def("read_at", &file_read_at, nb::arg("size"), nb::arg("offset"))
        .def("read_at_many", &file_read_at_many, nb::arg("specs"),
             "Gather N (size, offset) reads in one ring submission; returns a list of bytes.")
        .def("write_at", &file_write_at, nb::arg("data"), nb::arg("offset"))
        .def("read", &read_bytes<PyFile, &PyFile::file>, nb::arg("size"))
        .def("read_into", &read_into_buffer<PyFile, &PyFile::file>, nb::arg("buffer"),
             "Read into a writable buffer (bytearray); returns the byte count.")
        .def("write", &write_bytes<PyFile, &PyFile::file>, nb::arg("data"))
        .def("fsync", &file_fsync)
        .def("close", &close_handle<PyFile, &PyFile::file>)
        .def("valid", [](PyFile& self) { return self.file.valid(); });

    module_.def("connect", &tcp_connect, nb::arg("context"), nb::arg("host"), nb::arg("port"),
          nb::keep_alive<0, 1>(), "Connect a TCP socket through io::connect.");
    module_.def("listen", &tcp_listen, nb::arg("context"), nb::arg("host"), nb::arg("port"),
          nb::arg("backlog") = 128, nb::keep_alive<0, 1>(),
          "Create a TCP acceptor (blocking listen(2)).");

    nb::class_<PyListener>(module_, "Listener")
        .def("accept", &listener_accept, nb::keep_alive<0, 1>());

    nb::class_<PySocket>(module_, "TcpSocket")
        .def("send", &write_bytes<PySocket, &PySocket::socket>, nb::arg("data"))
        .def("recv", &read_bytes<PySocket, &PySocket::socket>, nb::arg("size"))
        .def("recv_into", &read_into_buffer<PySocket, &PySocket::socket>, nb::arg("buffer"),
             "Receive into a writable buffer (bytearray); returns the byte count.")
        .def("close", &close_handle<PySocket, &PySocket::socket>)
        .def("valid", [](PySocket& self) { return self.socket.valid(); });

    module_.def("sleep", &sleep_for, nb::arg("context"), nb::arg("seconds"),
          "Sleep through the ring (io_uring timeout, GIL released).");

    iox_py::register_async(module_);
}
