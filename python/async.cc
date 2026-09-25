// iox — python/async.cc: the asyncio bridge. One AsyncContext owns one io thread
// (one io_uring); every async method submits through context.post() and marshals the
// completion back onto the caller's asyncio loop via call_soon_threadsafe.
#include "async_api.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <iox/iox.h>

namespace nb = nanobind;
using namespace iox;

namespace iox_py {
namespace {

struct AsyncContext;

[[noreturn]] void raise_oserror(int code, const char* message) {
    PyObject* args = Py_BuildValue("(is)", code, message);
    PyErr_SetObject(PyExc_OSError, args);
    Py_XDECREF(args);
    throw nb::python_error();
}

[[noreturn]] void raise(const error& err) {
    const std::string message = err.message();
    raise_oserror(err.code(), message.c_str());
}

template <class Handle>
void check_valid(const Handle& handle) {
    if (!handle.valid()) {
        raise_oserror(EBADF, std::strerror(EBADF));
    }
}

struct cancel_slot {
    stdexec::inplace_stop_source stop_source;
};

nb::object alloc_bytes(std::size_t size) {
    nb::object py_bytes = nb::steal(PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size)));
    if (!py_bytes.is_valid()) {
        throw nb::python_error();
    }
    return py_bytes;
}

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

// One io thread per AsyncContext; run() serves for the context's whole lifetime
// (post() wakes it through the inbox watch). `core` is shared with the worker so
// that a dealloc happening inside a completion on the io thread itself can stop
// and detach instead of joining its own thread. The constructor waits until the
// worker is inside run(): run() clears the stop flag at entry, so a close()
// landing before that point would be silently lost.
//
// Shutdown: every submitted op registers its cancel_slot (track/untrack).
// close() flips `closing`, requests stop on a snapshot of the registry, and
// stops the ring only once no ops are in flight — either directly, or from the
// last op's untrack. That way a close() with pending ops (the classic: a
// cancelled aaccept still in the kernel) drains them instead of leaking them.
struct AsyncContext {
    std::shared_ptr<io_context> core = std::make_shared<io_context>();
    std::thread worker;
    std::mutex registry_mutex;
    std::unordered_set<std::shared_ptr<cancel_slot>> registry;
    std::atomic<std::size_t> in_flight{0};
    std::atomic<bool> closing{false};

    AsyncContext() : worker([core = core] { core->run(); }) {
        if (!core->ok()) {
            worker.join(); // run() returns immediately on a dead ring
            raise_oserror(EBUSY, "io_uring initialization failed");
        }
        std::binary_semaphore started(0); // released at run()'s first drain point
        core->post([](io_context&, std::uint64_t semaphore_bits) {
            reinterpret_cast<std::binary_semaphore*>(static_cast<std::uintptr_t>(semaphore_bits))->release();
        }, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&started)));
        nb::gil_scoped_release unheld;
        started.acquire();
    }

    void track(const std::shared_ptr<cancel_slot>& slot) {
        std::lock_guard lock{registry_mutex};
        if (closing.load()) {
            PyErr_SetString(PyExc_RuntimeError, "AsyncContext is closing");
            throw nb::python_error();
        }
        registry.insert(slot);
        in_flight.fetch_add(1);
    }

    void untrack(const std::shared_ptr<cancel_slot>& slot) {
        {
            std::lock_guard lock{registry_mutex};
            registry.erase(slot);
        }
        if (in_flight.fetch_sub(1) == 1 && closing.load()) {
            core->stop(); // last in-flight op during shutdown: run() may exit now
        }
    }

    void close() {
        bool expected = false;
        if (closing.compare_exchange_strong(expected, true)) { // initiate teardown once
            std::vector<std::shared_ptr<cancel_slot>> snapshot;
            {
                std::lock_guard lock{registry_mutex};
                snapshot.assign(registry.begin(), registry.end());
            }
            for (auto& slot : snapshot) {
                slot->stop_source.request_stop(); // cross-thread safe; a no-op when already done
            }
            if (in_flight.load() == 0) {
                core->stop();
            }
        }
        if (!worker.joinable()) {
            return;
        }
        if (std::this_thread::get_id() == worker.get_id()) {
            worker.detach();
            return;
        }
        nb::gil_scoped_release unheld; // the io thread may be waiting on the GIL
        worker.join();
    }

    ~AsyncContext() { close(); }
};

struct AsyncFile {
    nb::object context_ref;
    AsyncContext* context;
    fs::file file;

    AsyncFile(nb::object context_ref, AsyncContext* context, fs::file file)
        : context_ref(std::move(context_ref)), context(context), file(std::move(file)) {}
};

struct AsyncTcpSocket {
    nb::object context_ref;
    AsyncContext* context;
    net::tcp::socket socket;

    AsyncTcpSocket(nb::object context_ref, AsyncContext* context, net::tcp::socket socket)
        : context_ref(std::move(context_ref)), context(context), socket(std::move(socket)) {}
};

struct AsyncListener {
    nb::object context_ref;
    AsyncContext* context;
    net::tcp::acceptor acceptor;

    AsyncListener(nb::object context_ref, AsyncContext* context, net::tcp::acceptor acceptor)
        : context_ref(std::move(context_ref)), context(context), acceptor(std::move(acceptor)) {}
};

// Loop-thread guards: the future may have been cancelled between the io thread
// scheduling the completion and the loop running it; re-check before touching
// the result. The value goes through std::optional because this nanobind
// rejects None for object/handle parameters. Leaked on purpose — they must
// outlive the interpreter.
nb::object& result_guard() {
    static nb::object* callback = new nb::object(nb::cpp_function(
        [](nb::object future, std::optional<nb::object> value) {
            if (!nb::cast<bool>(future.attr("cancelled")())) {
                future.attr("set_result")(value ? std::move(*value) : nb::object(nb::none()));
            }
        }));
    return *callback;
}

nb::object& exception_guard() {
    static nb::object* callback = new nb::object(nb::cpp_function([](nb::object future, nb::object exception) {
        if (!nb::cast<bool>(future.attr("cancelled")())) {
            future.attr("set_exception")(std::move(exception));
        }
    }));
    return *callback;
}

struct bytes_converter { // keep is the preallocated result bytes; shrink to the transfer count
    template <class Operation>
    static nb::object convert(Operation& operation, std::size_t transferred) {
        return bytes_resize(std::move(operation.keep), transferred);
    }
};
struct int_converter {
    template <class Operation>
    static nb::object convert(Operation&, std::size_t transferred) {
        return nb::int_(transferred);
    }
};
struct none_converter {
    template <class Operation>
    static nb::object convert(Operation&) {
        return nb::none();
    }
};
struct keep_converter { // connect: the result object was built before submission
    template <class Operation>
    static nb::object convert(Operation& operation) {
        return std::move(operation.keep);
    }
};
struct socket_converter { // accept: wrap the adopted socket
    template <class Operation>
    static nb::object convert(Operation& operation, net::tcp::socket socket) {
        return nb::cast(AsyncTcpSocket{operation.context_ref, operation.context, std::move(socket)});
    }
};

// One heap-allocated, immobile operation per in-flight request. The nested
// receiver points back at the enclosing async_operation; completions run on the io
// thread, acquire the GIL, marshal onto the loop, then destroy the operation.
// get_env needs an explicit return type: operation_t is aliased inside the class,
// where async_operation is still incomplete, so auto deduction cannot work.
template <class Sender, class Converter>
struct async_operation {
    using stop_environment =
        stdexec::env<stdexec::prop<stdexec::get_stop_token_t, stdexec::inplace_stop_token>>;

    struct receiver {
        using receiver_concept = stdexec::receiver_tag;
        async_operation* operation;

        stop_environment get_env() const noexcept {
            return {stdexec::prop{stdexec::get_stop_token, operation->cancel->stop_source.get_token()}};
        }
        template <class... Values>
        void set_value(Values&&... values) && noexcept {
            finish_ok(operation, std::forward<Values>(values)...);
        }
        void set_error(error err) && noexcept { finish_err(operation, err); }
        void set_error(std::exception_ptr) && noexcept { finish_err(operation, error::from_errno(EIO)); }
        void set_stopped() && noexcept { finish_stopped(operation); }
    };
    using operation_t = decltype(stdexec::connect(std::declval<Sender>(), std::declval<receiver>()));

    AsyncContext* context;
    nb::object context_ref;
    nb::object loop, future, keep;
    std::shared_ptr<cancel_slot> cancel;
    Py_buffer view{};
    bool has_view = false;
    bool tracked = false; // set once registered in context; the destructor untracks
    operation_t operation_state;

    template <class FwdSender>
    async_operation(AsyncContext& context, nb::object context_ref, nb::object loop,
                    nb::object future, nb::object keep, const Py_buffer* initial_view,
                    FwdSender&& sender)
        : context(&context), context_ref(std::move(context_ref)), loop(std::move(loop)),
          future(std::move(future)), keep(std::move(keep)),
          cancel(std::make_shared<cancel_slot>()),
          operation_state(stdexec::connect(std::forward<FwdSender>(sender), receiver{this})) {
        if (initial_view != nullptr) {
            view = *initial_view;
            has_view = true;
        }
    }

    async_operation(const async_operation&) = delete;
    async_operation& operator=(const async_operation&) = delete;

    ~async_operation() { // every delete path holds the GIL; context_ref still holds context here
        if (tracked) {
            context->untrack(cancel);
        }
        if (has_view) {
            PyBuffer_Release(&view);
        }
    }

    template <class... Values>
    static void finish_ok(async_operation* operation, Values&&... values) noexcept {
        nb::gil_scoped_acquire gil;
        try {
            if (!nb::cast<bool>(operation->future.attr("cancelled")())) {
                operation->loop.attr("call_soon_threadsafe")(
                    result_guard(), operation->future, Converter::convert(*operation, std::forward<Values>(values)...));
            }
        } catch (...) { // a closed loop rejects call_soon_threadsafe; drop the completion
        }
        delete operation;
    }

    static void finish_err(async_operation* operation, error err) noexcept {
        nb::gil_scoped_acquire gil;
        try {
            if (!nb::cast<bool>(operation->future.attr("cancelled")())) {
                PyObject* exception =
                    PyObject_CallFunction(PyExc_OSError, "(is)", err.code(), err.message().c_str());
                operation->loop.attr("call_soon_threadsafe")(exception_guard(), operation->future, nb::steal(exception));
            }
        } catch (...) {
        }
        delete operation;
    }

    static void finish_stopped(async_operation* operation) noexcept {
        nb::gil_scoped_acquire gil;
        try {
            // a user-cancelled future needs no help; a close()-force-stopped op
            // must not leave an awaiting task suspended forever
            if (!nb::cast<bool>(operation->future.attr("cancelled")())) {
                operation->loop.attr("call_soon_threadsafe")(operation->future.attr("cancel"));
            }
        } catch (...) { // a closed loop rejects call_soon_threadsafe
        }
        delete operation;
    }
};

template <class Operation>
void start_posted(io_context&, std::uint64_t operation_bits) {
    stdexec::start(reinterpret_cast<Operation*>(static_cast<std::uintptr_t>(operation_bits))->operation_state);
}

void wire_cancel(nb::object& future, std::shared_ptr<cancel_slot> slot) {
    future.attr("add_done_callback")(nb::cpp_function([slot = std::move(slot)](nb::object future) {
        if (nb::cast<bool>(future.attr("cancelled")())) {
            slot->stop_source.request_stop();
        }
    }));
}

template <class Converter, class Sender>
nb::object submit(AsyncContext& self, nb::object context_ref, nb::object keep, Sender&& sender,
                  const Py_buffer* view = nullptr) {
    using operation_t = async_operation<std::remove_cvref_t<Sender>, Converter>;
    if (self.core->stopped()) {
        PyErr_SetString(PyExc_RuntimeError, "AsyncContext is closed");
        throw nb::python_error();
    }
    nb::object loop = nb::module_::import_("asyncio").attr("get_running_loop")();
    nb::object future = loop.attr("create_future")();
    auto* operation = new operation_t(self, std::move(context_ref), loop, future, std::move(keep),
                                      view, std::forward<Sender>(sender));
    try {
        self.track(operation->cancel);
        operation->tracked = true;
        wire_cancel(future, operation->cancel);
    } catch (...) {
        delete operation; // untracks only when track() succeeded
        throw;
    }
    self.core->post(&start_posted<operation_t>,
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(operation)));
    return future;
}

// ---- AsyncContext factories -------------------------------------------------

AsyncFile context_open_file(nb::handle self_handle, const std::string& path, unsigned mode) {
    auto& self = nb::cast<AsyncContext&>(self_handle);
    auto file = [&] {
        nb::gil_scoped_release unheld;
        return fs::file::open(path.c_str(), static_cast<fs::mode>(mode));
    }();
    if (!file) {
        raise(file.error());
    }
    return AsyncFile{nb::borrow(self_handle), &self, std::move(*file)};
}

AsyncListener context_listen(nb::handle self_handle, const std::string& host, std::uint16_t port,
                             int backlog) {
    auto& self = nb::cast<AsyncContext&>(self_handle);
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
    return AsyncListener{nb::borrow(self_handle), &self, std::move(*acceptor)};
}

nb::object context_connect(nb::handle self_handle, const std::string& host, std::uint16_t port) {
    auto& self = nb::cast<AsyncContext&>(self_handle);
    auto endpoint = net::endpoint::parse(host + ":" + std::to_string(port));
    if (!endpoint) {
        raise(endpoint.error());
    }
    auto socket = net::tcp::socket::unconnected(endpoint->family());
    if (!socket) {
        raise(socket.error());
    }
    auto sender = io::connect(*self.core, *socket, *endpoint); // fd + sockaddr captured by value
    nb::object context_ref = nb::borrow(self_handle);
    nb::object keep = nb::cast(AsyncTcpSocket{context_ref, &self, std::move(*socket)});
    return submit<keep_converter>(self, std::move(context_ref), std::move(keep), std::move(sender));
}

nb::object context_sleep(nb::handle self_handle, double seconds) {
    auto& self = nb::cast<AsyncContext&>(self_handle);
    if (seconds < 0) {
        throw nb::value_error("sleep duration must be non-negative");
    }
    return submit<none_converter>(self, nb::borrow(self_handle), nb::object(),
                                  io::sleep_for(*self.core, std::chrono::nanoseconds{
                                                               static_cast<std::int64_t>(seconds * 1e9)}));
}

// ---- AsyncFile --------------------------------------------------------------

nb::object file_aread(AsyncFile& self, std::size_t size) {
    check_valid(self.file);
    nb::object buffer = alloc_bytes(size);
    return submit<bytes_converter>(*self.context, self.context_ref, buffer,
                                   io::read(*self.context->core, self.file,
                                            wbytes{as_wbytes(std::span<char>{
                                                PyBytes_AS_STRING(buffer.ptr()), size})}));
}

nb::object file_awrite(AsyncFile& self, nb::bytes data) {
    check_valid(self.file);
    nb::object keep = data; // zero-copy: submit the immutable bytes buffer in place
    return submit<int_converter>(*self.context, self.context_ref, std::move(keep),
                                 io::write(*self.context->core, self.file,
                                           rbytes{as_rbytes(
                                               std::span<const char>{data.c_str(), data.size()})}));
}

nb::object file_aread_at(AsyncFile& self, std::size_t size, std::uint64_t offset) {
    check_valid(self.file);
    nb::object buffer = alloc_bytes(size);
    return submit<bytes_converter>(*self.context, self.context_ref, buffer,
                                   io::read_at(*self.context->core, self.file,
                                               wbytes{as_wbytes(std::span<char>{
                                                   PyBytes_AS_STRING(buffer.ptr()), size})},
                                               uoffset_t{offset}));
}

nb::object file_awrite_at(AsyncFile& self, nb::bytes data, std::uint64_t offset) {
    check_valid(self.file);
    nb::object keep = data; // zero-copy
    return submit<int_converter>(*self.context, self.context_ref, std::move(keep),
                                 io::write_at(*self.context->core, self.file,
                                              rbytes{as_rbytes(
                                                  std::span<const char>{data.c_str(), data.size()})},
                                              uoffset_t{offset}));
}

nb::object file_afsync(AsyncFile& self) {
    check_valid(self.file);
    return submit<none_converter>(*self.context, self.context_ref, nb::object(),
                                  io::fsync(*self.context->core, self.file));
}

template <class PyWrapper, auto Member>
nb::object aclose_handle(PyWrapper& self) {
    auto& handle = self.*Member;
    check_valid(handle);
    return submit<none_converter>(*self.context, self.context_ref, nb::object(),
                                  io::close(*self.context->core, std::move(handle))); // steals the fd now
}

// ---- AsyncFile.aread_at_many: N reads, one submission, a counting receiver --

struct read_many_operation {
    using stop_environment =
        stdexec::env<stdexec::prop<stdexec::get_stop_token_t, stdexec::inplace_stop_token>>;

    struct item_receiver {
        using receiver_concept = stdexec::receiver_tag;
        read_many_operation* operation;
        std::size_t index;

        stop_environment get_env() const noexcept {
            return {stdexec::prop{stdexec::get_stop_token, operation->cancel->stop_source.get_token()}};
        }
        void set_value(std::size_t transferred) && noexcept { item_ok(operation, index, transferred); }
        void set_error(error err) && noexcept { item_err(operation, err); }
        void set_error(std::exception_ptr) && noexcept { item_err(operation, error::from_errno(EIO)); }
        void set_stopped() && noexcept { item_stopped(operation); }
    };
    using sender_t = decltype(io::read_at(std::declval<io_context&>(), std::declval<fs::file&>(),
                                          wbytes{}, uoffset_t{}));
    using operation_t = decltype(stdexec::connect(std::declval<sender_t>(), std::declval<item_receiver>()));

    // all completions are serialized on the io thread: no synchronization needed
    AsyncContext* context = nullptr;
    nb::object context_ref, loop, future;
    std::vector<nb::object> buffers;
    std::vector<std::unique_ptr<operation_t>> operations;
    std::shared_ptr<cancel_slot> cancel;
    std::size_t remaining = 0;
    std::optional<error> first_error;
    bool stopped = false;
    bool tracked = false; // set once registered in context; the destructor untracks

    ~read_many_operation() {
        if (tracked) {
            context->untrack(cancel);
        }
    }

    static void item_ok(read_many_operation* operation, std::size_t index, std::size_t transferred) noexcept {
        nb::gil_scoped_acquire gil;
        try {
            operation->buffers[index] = bytes_resize(std::move(operation->buffers[index]), transferred);
        } catch (...) {
            if (!operation->first_error) {
                operation->first_error = error::from_errno(ENOMEM);
            }
        }
        if (--operation->remaining == 0) {
            finish(operation);
        }
    }
    static void item_err(read_many_operation* operation, error err) noexcept {
        nb::gil_scoped_acquire gil;
        if (!operation->first_error) {
            operation->first_error = err;
        }
        if (--operation->remaining == 0) {
            finish(operation);
        }
    }
    static void item_stopped(read_many_operation* operation) noexcept {
        nb::gil_scoped_acquire gil;
        operation->stopped = true; // the whole gather is dropped
        if (--operation->remaining == 0) {
            finish(operation);
        }
    }
    static void finish(read_many_operation* operation) noexcept { // GIL held by the item_* callers
        try {
            const bool cancelled = nb::cast<bool>(operation->future.attr("cancelled")());
            if (operation->stopped) {
                if (!cancelled) { // close()-force-stopped: wake the awaiting task
                    operation->loop.attr("call_soon_threadsafe")(operation->future.attr("cancel"));
                }
            } else if (!cancelled) {
                if (operation->first_error) {
                    PyObject* exception = PyObject_CallFunction(PyExc_OSError, "(is)",
                                                                operation->first_error->code(),
                                                                operation->first_error->message().c_str());
                    operation->loop.attr("call_soon_threadsafe")(exception_guard(), operation->future,
                                                                 nb::steal(exception));
                } else {
                    nb::list result;
                    for (auto& buffer : operation->buffers) {
                        result.append(std::move(buffer));
                    }
                    operation->loop.attr("call_soon_threadsafe")(result_guard(), operation->future,
                                                                 std::move(result));
                }
            }
        } catch (...) {
        }
        delete operation;
    }
};

void start_posted_many(io_context&, std::uint64_t operation_bits) {
    auto* operation = reinterpret_cast<read_many_operation*>(static_cast<std::uintptr_t>(operation_bits));
    for (auto& operation_state : operation->operations) {
        stdexec::start(*operation_state);
    }
}

nb::object file_aread_at_many(AsyncFile& self, const nb::list& specs) {
    check_valid(self.file);
    if (self.context->core->stopped()) {
        PyErr_SetString(PyExc_RuntimeError, "AsyncContext is closed");
        throw nb::python_error();
    }
    nb::object loop = nb::module_::import_("asyncio").attr("get_running_loop")();
    nb::object future = loop.attr("create_future")();
    auto* operation = new read_many_operation;
    operation->context = self.context;
    operation->context_ref = self.context_ref;
    operation->loop = loop;
    operation->future = future;
    operation->cancel = std::make_shared<cancel_slot>();
    try {
        const std::size_t count = nb::len(specs);
        operation->buffers.reserve(count);
        operation->operations.reserve(count);
        for (nb::handle item : specs) {
            const auto [size, offset] = nb::cast<std::pair<std::size_t, std::uint64_t>>(item);
            nb::object buffer = alloc_bytes(size);
            const std::size_t index = operation->operations.size();
            operation->operations.push_back(std::make_unique<read_many_operation::operation_t>(stdexec::connect(
                io::read_at(*self.context->core, self.file,
                            wbytes{as_wbytes(std::span<char>{PyBytes_AS_STRING(buffer.ptr()), size})},
                            uoffset_t{offset}),
                read_many_operation::item_receiver{operation, index})));
            operation->buffers.push_back(std::move(buffer));
        }
    } catch (...) {
        delete operation;
        throw;
    }
    operation->remaining = operation->operations.size();
    if (operation->remaining == 0) {
        delete operation;
        future.attr("set_result")(nb::list());
        return future;
    }
    try {
        self.context->track(operation->cancel);
        operation->tracked = true;
        wire_cancel(future, operation->cancel);
    } catch (...) {
        delete operation; // untracks only when track() succeeded
        throw;
    }
    self.context->core->post(&start_posted_many,
                             static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(operation)));
    return future;
}

// ---- AsyncTcpSocket ---------------------------------------------------------

nb::object sock_asend(AsyncTcpSocket& self, nb::bytes data) {
    check_valid(self.socket);
    nb::object keep = data; // zero-copy
    return submit<int_converter>(*self.context, self.context_ref, std::move(keep),
                                 io::write(*self.context->core, self.socket,
                                           rbytes{as_rbytes(
                                               std::span<const char>{data.c_str(), data.size()})}));
}

nb::object sock_arecv(AsyncTcpSocket& self, std::size_t size) {
    check_valid(self.socket);
    nb::object buffer = alloc_bytes(size);
    return submit<bytes_converter>(*self.context, self.context_ref, buffer,
                                   io::read(*self.context->core, self.socket,
                                            wbytes{as_wbytes(std::span<char>{
                                                PyBytes_AS_STRING(buffer.ptr()), size})}));
}

nb::object sock_arecv_into(AsyncTcpSocket& self, nb::handle buffer) {
    check_valid(self.socket);
    Py_buffer view;
    if (PyObject_GetBuffer(buffer.ptr(), &view, PyBUF_SIMPLE | PyBUF_WRITABLE) < 0) {
        throw nb::python_error();
    }
    try {
        return submit<int_converter>(*self.context, self.context_ref, nb::borrow(buffer),
                                     io::read(*self.context->core, self.socket,
                                              wbytes{static_cast<std::byte*>(view.buf),
                                                     static_cast<std::size_t>(view.len)}),
                                     &view);
    } catch (...) {
        PyBuffer_Release(&view);
        throw;
    }
}

// ---- AsyncListener ----------------------------------------------------------

nb::object listener_aaccept(AsyncListener& self) {
    check_valid(self.acceptor);
    return submit<socket_converter>(*self.context, self.context_ref, nb::object(),
                                    io::accept(*self.context->core, self.acceptor));
}

} // namespace

void register_async(nb::module_ module_) {
    nb::class_<AsyncContext>(module_, "AsyncContext",
                             "Owns an io_context plus a dedicated io thread; async methods "
                             "return awaitable asyncio futures on the running loop.")
        .def(nb::init<>())
        .def("close", &AsyncContext::close, "Stop the io thread (idempotent).")
        .def("__enter__", [](nb::handle self) { return nb::borrow(self); })
        .def("__exit__", [](AsyncContext& self, nb::args) { self.close(); })
        .def("open_file", &context_open_file, nb::arg("path"),
             nb::arg("mode") = static_cast<unsigned>(fs::mode::read))
        .def("listen", &context_listen, nb::arg("host"), nb::arg("port"), nb::arg("backlog") = 128)
        .def("connect", &context_connect, nb::arg("host"), nb::arg("port"))
        .def("sleep", &context_sleep, nb::arg("seconds"));

    nb::class_<AsyncFile>(module_, "AsyncFile")
        .def("aread", &file_aread, nb::arg("size"))
        .def("awrite", &file_awrite, nb::arg("data"))
        .def("aread_at", &file_aread_at, nb::arg("size"), nb::arg("offset"))
        .def("awrite_at", &file_awrite_at, nb::arg("data"), nb::arg("offset"))
        .def("aread_at_many", &file_aread_at_many, nb::arg("specs"),
             "Gather N (size, offset) reads in one submission; awaits to a list of bytes.")
        .def("afsync", &file_afsync)
        .def("aclose", &aclose_handle<AsyncFile, &AsyncFile::file>)
        .def("valid", [](AsyncFile& self) { return self.file.valid(); });

    nb::class_<AsyncTcpSocket>(module_, "AsyncTcpSocket")
        .def("asend", &sock_asend, nb::arg("data"))
        .def("arecv", &sock_arecv, nb::arg("size"))
        .def("arecv_into", &sock_arecv_into, nb::arg("buffer"),
             "Receive into a writable buffer (bytearray); awaits to the byte count.")
        .def("aclose", &aclose_handle<AsyncTcpSocket, &AsyncTcpSocket::socket>)
        .def("valid", [](AsyncTcpSocket& self) { return self.socket.valid(); });

    nb::class_<AsyncListener>(module_, "AsyncListener")
        .def("aaccept", &listener_aaccept)
        .def("valid", [](AsyncListener& self) { return self.acceptor.valid(); });
}

} // namespace iox_py
