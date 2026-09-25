// iox — unified async IO for Linux
// bench/echo.cc — Topology is identical for both implementations: one server thread and one
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <iox/core/exec.h>
#include <iox/compose/loop.h>
#include <iox/compose/write_all.h>
#include <iox/compose/detach.h>
#include <iox/net/tcp.h>
#include <iox/ops.h>

using namespace iox;
namespace ex = iox::exec;

namespace {

using clock_t_ = std::chrono::steady_clock;
constexpr std::size_t kPayload = 64;
constexpr int kConns = 4;
constexpr int kInflight = 4;

struct result {
    std::uint64_t round_trips = 0;
    double p50_ns = 0, p99_ns = 0;
};

std::uint16_t listen_on_ephemeral(int& fd_out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 64) != 0) {
        ::close(fd);
        fd_out = -1;
        return 0;
    }
    socklen_t length = sizeof(address);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    fd_out = fd;
    return ntohs(address.sin_port);
}

int connect_one(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7f000001);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        pollfd poll_fd{fd, POLLOUT, 0};
        (void)::poll(&poll_fd, 1, 2000);
    }
    return fd;
}

struct samples {
    std::vector<double> nanoseconds;
    void add(clock_t_::duration duration) {
        if (nanoseconds.size() < 200000) {
            nanoseconds.push_back(std::chrono::duration<double, std::nano>(duration).count());
        }
    }
    void finalize(result& out) {
        if (nanoseconds.empty()) {
            return;
        }
        std::sort(nanoseconds.begin(), nanoseconds.end());
        out.round_trips = nanoseconds.size();
        out.p50_ns = nanoseconds[nanoseconds.size() / 2];
        out.p99_ns = nanoseconds[(nanoseconds.size() * 99) / 100];
    }
};

result run_iox(int seconds) {
    std::atomic<bool> stop{false};

    auto acceptor = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
    if (!acceptor) {
        std::fprintf(stderr, "iox bench: listen failed: %s\n",
                     acceptor.error().message().c_str());
        return {};
    }
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    ::getsockname(acceptor->accept_handle().v, reinterpret_cast<sockaddr*>(&address), &address_length);
    const auto port = ntohs(address.sin_port);
    std::atomic<int> live_sessions{0};
    const auto server_start = clock_t_::now();
    std::thread server([&] {
        io_context context;
        struct session {
            net::tcp::socket socket;
            std::unique_ptr<std::byte[]> buffer = std::make_unique<std::byte[]>(kPayload);
        };
        auto serve_one = [&](net::tcp::socket sock) {
            auto* session_ptr = new session{std::move(sock)};
            live_sessions.fetch_add(1);
            ex::detach(io::loop(context, [session_ptr, &context, eof = false]() mutable {
                            return io::read(context, session_ptr->socket,
                                            wbytes{session_ptr->buffer.get(), kPayload})
                                 | ex::let_value([session_ptr, &context, &eof](std::size_t count) {
                                       eof = (count == 0);
                                       return io::write_all(
                                           context, session_ptr->socket,
                                           rbytes{session_ptr->buffer.get(), count});
                                   })
                                 | ex::then([&eof] { return eof; });
                        })
                        | ex::upon_error([](auto&&) {})
                        | ex::then([session_ptr, &live_sessions] {
                              delete session_ptr;
                              live_sessions.fetch_sub(1);
                          }));
        };
        std::atomic<uint64_t> accepts{0};
        ex::detach(io::loop(context, [&] {
            return io::accept(context, *acceptor)
                 | ex::then([&](net::tcp::socket socket) {
                       accepts.fetch_add(1);
                       serve_one(std::move(socket));
                       return false;
                   });
        }));
        while ((!stop.load(std::memory_order_relaxed) || live_sessions.load() > 0)
               && clock_t_::now() < server_start + std::chrono::seconds(seconds)
                    + std::chrono::seconds(5)) {
            context.run_for(std::chrono::milliseconds(50));
        }
    });

    result out;
    std::thread client([&] {
        io_context context;
        samples latency_samples;
        std::atomic<int> finished{0};
        std::atomic<uint64_t> iterations{0};

        struct slot {
            net::tcp::socket socket;
            std::unique_ptr<std::byte[]> buffer = std::make_unique<std::byte[]>(kPayload);
            clock_t_::time_point start_time;
            explicit slot(net::tcp::socket sock) : socket(std::move(sock)) {}
        };
        auto slots = std::make_unique<std::unique_ptr<slot>[]>(kConns * kInflight);
        for (int index = 0; index < kConns * kInflight; ++index) {
            auto socket = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
            if (!socket) {
                return;
            }
            auto connect_result = ex::sync_wait(
                context, io::connect(context, *socket,
                                     *net::endpoint::ipv4("127.0.0.1", port)));
            if (!connect_result) {
                return;
            }
            slots[index] = std::make_unique<slot>(std::move(*socket));
        }

        for (int index = 0; index < kConns * kInflight; ++index) {
            slot* slot_ptr = slots[index].get();
            ex::detach(
                io::loop(context, [slot_ptr, &latency_samples, &stop, &context, &iterations] {
                    slot_ptr->start_time = clock_t_::now();
                    return io::write_all(context, slot_ptr->socket, rbytes{slot_ptr->buffer.get(), kPayload})
                         | ex::let_value([slot_ptr, &context] {
                               return io::read(context, slot_ptr->socket,
                                               wbytes{slot_ptr->buffer.get(), kPayload});
                           })
                         | ex::then([slot_ptr, &latency_samples, &stop, &iterations](std::size_t count) {
                               ++iterations;
                               if (count == kPayload) {
                                   latency_samples.add(clock_t_::now() - slot_ptr->start_time);
                               }
                               return stop.load(std::memory_order_relaxed);
                           });
                })
                | ex::upon_error([](auto&&) {})
                | ex::then([&] { finished.fetch_add(1); }));
        }

        const auto deadline = clock_t_::now() + std::chrono::seconds(seconds);
        while (clock_t_::now() < deadline) {
            context.run_for(std::chrono::milliseconds(20));
        }
        stop.store(true);
        const auto drain_deadline = clock_t_::now() + std::chrono::seconds(5);
        while (finished.load() < kConns * kInflight
               && clock_t_::now() < drain_deadline) {
            context.run_for(std::chrono::milliseconds(20));
        }
        latency_samples.finalize(out);
    });

    client.join();
    stop.store(true);
    server.join();
    return out;
}

struct raw_conn {
    int fd = -1;
    bool awaiting_send = false;
    std::byte buffer[kPayload];
};

struct raw_ping {
    int fd = -1;
    bool want_read = false;
    clock_t_::time_point start_time;
    std::byte buffer[kPayload];
};

struct raw_ctx {
    uring::ring ring;
    std::vector<raw_conn> connections;
};

void raw_submit_echo(uring::ring& ring, raw_conn& connection) {
    io_uring_sqe* sqe = ring.next_sqe();
    if (sqe == nullptr) {
        ring.flush();
        sqe = ring.next_sqe();
    }
    ::io_uring_prep_recv(sqe, connection.fd, connection.buffer, kPayload, 0);
    ::io_uring_sqe_set_data(sqe, &connection);
}

result run_raw(int seconds) {
    std::atomic<bool> stop{false};
    int listen_fd = -1;
    const auto port = listen_on_ephemeral(listen_fd);

    std::thread server([&] {
        uring::ring ring{uring::ring_params{.entries = 256}};
        struct accept_slot {
            iox::op_base base;
            static void thunk(iox::op_base*, io_context&, std::int32_t, std::uint32_t) noexcept {}
            accept_slot() noexcept : base(&thunk) {}
        };
        std::vector<accept_slot> accepts(8);
        std::vector<raw_conn> connections;
        connections.reserve(64);

        auto arm_accept = [&](int index) {
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
            }
            ::io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            ::io_uring_sqe_set_data(sqe, &accepts[index]);
        };
        for (int index = 0; index < 8; ++index) {
            arm_accept(index);
        }

        while (!stop.load(std::memory_order_relaxed)) {
            ring.flush_and_wait(1);
            ring.for_each_cqe([&](io_uring_cqe* cqe) {
                void* user_data = io_uring_cqe_get_data(cqe);
                if (user_data >= accepts.data() && user_data < accepts.data() + accepts.size()) {
                    if (cqe->res >= 0) {
                        connections.push_back(raw_conn{cqe->res, false, {}});
                        raw_submit_echo(ring, connections.back());
                        arm_accept(static_cast<int>(
                            static_cast<accept_slot*>(user_data) - accepts.data()));
                    }
                } else if (auto* connection = static_cast<raw_conn*>(user_data); connection != nullptr) {
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    if (connection->awaiting_send) {
                        ::io_uring_prep_recv(sqe, connection->fd, connection->buffer, kPayload, 0);
                        ::io_uring_sqe_set_data(sqe, connection);
                        connection->awaiting_send = false;
                    } else {
                        ::io_uring_prep_send(sqe, connection->fd, connection->buffer,
                                             static_cast<std::size_t>(cqe->res),
                                             MSG_NOSIGNAL);
                        ::io_uring_sqe_set_data(sqe, connection);
                        connection->awaiting_send = true;
                    }
                }
            });
        }
        for (auto& connection : connections) {
            ::close(connection.fd);
        }
    });

    result out;
    std::thread client([&] {
        uring::ring ring{uring::ring_params{.entries = 256}};
        samples latency_samples;
        std::vector<raw_ping> pings(kConns * kInflight);
        std::atomic<int> finished{0};
        std::atomic<uint64_t> iterations{0};

        for (auto& ping : pings) {
            ping.fd = connect_one(port);
            ping.want_read = false;
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
            }
            ::memset(ping.buffer, 'x', kPayload);
            ping.start_time = clock_t_::now();
            ::io_uring_prep_send(sqe, ping.fd, ping.buffer, kPayload, MSG_NOSIGNAL);
            ::io_uring_sqe_set_data(sqe, &ping);
            ping.want_read = true;
        }

        while (finished.load() < kConns * kInflight) {
            ring.flush_and_wait(1);
            ring.for_each_cqe([&](io_uring_cqe* cqe) {
                auto* ping = static_cast<raw_ping*>(io_uring_cqe_get_data(cqe));
                if (ping == nullptr || ping->fd < 0) {
                    return;
                }
                if (ping->want_read) {
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    ::io_uring_prep_recv(sqe, ping->fd, ping->buffer, kPayload, 0);
                    ::io_uring_sqe_set_data(sqe, ping);
                    ping->want_read = false;
                } else {
                    if (cqe->res == static_cast<std::int32_t>(kPayload)) {
                        latency_samples.add(clock_t_::now() - ping->start_time);
                    }
                    if (stop.load(std::memory_order_relaxed)) {
                        ping->fd = -1;
                        finished.fetch_add(1);
                        return;
                    }
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    ping->start_time = clock_t_::now();
                    ::io_uring_prep_send(sqe, ping->fd, ping->buffer, kPayload, MSG_NOSIGNAL);
                    ::io_uring_sqe_set_data(sqe, ping);
                    ping->want_read = true;
                }
            });
        }
        latency_samples.finalize(out);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread([seconds, &stop] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop.store(true);
    }).detach();

    client.join();
    server.join();
    ::close(listen_fd);
    return out;
}

}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 2;
    std::printf("TCP echo, %d connections × %d in-flight × %zu B, %ds windows\n", kConns,
                kInflight, kPayload, seconds);

    const result iox_result = run_iox(seconds);
    const result raw_result = run_raw(seconds);

    auto line = [](const char* name, const result& result) {
        std::printf("  %-5s: %8.0f kpps   p50 %7.1f us   p99 %7.1f us\n", name,
                    static_cast<double>(result.round_trips) / 1e3,
                    result.p50_ns / 1000.0, result.p99_ns / 1000.0);
    };
    line("iox", iox_result);
    line("raw", raw_result);
    if (raw_result.round_trips != 0) {
        std::printf("  ratio : %.0f %% of raw round-trips\n",
                    100.0 * static_cast<double>(iox_result.round_trips) /
                        static_cast<double>(raw_result.round_trips));
    }
    return 0;
}
