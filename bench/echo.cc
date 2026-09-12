// echo — M3 benchmark: TCP echo round-trip throughput and latency.
//
// Topology is identical for both implementations: one server thread and one
// client thread, K connections, M in-flight 64-byte request/response slots
// per connection. The client records per-round-trip latency.
//
//   iox : accept loop + spawned echo sessions (io::loop), ping loops on the
//         client — the vocabulary end to end, no hand-rolled state machines.
//   raw : the same thing written directly against liburing.
//
// Perf gate (design §八): iox ≥ 85% of raw.
//
//     ./build/bench_echo [seconds=2]
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
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) != 0 ||
        ::listen(fd, 64) != 0) {
        ::close(fd);
        fd_out = -1;
        return 0;
    }
    socklen_t len = sizeof(sin);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&sin), &len);
    fd_out = fd;
    return ntohs(sin.sin_port);
}

int connect_one(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001);
    sin.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        pollfd p{fd, POLLOUT, 0};
        (void)::poll(&p, 1, 2000);
    }
    return fd;
}

// ---- shared client-side latency bookkeeping --------------------------------

struct samples {
    std::vector<double> ns;
    void add(clock_t_::duration d) {
        if (ns.size() < 200000) {
            ns.push_back(std::chrono::duration<double, std::nano>(d).count());
        }
    }
    void finalize(result& r) {
        if (ns.empty()) {
            return;
        }
        std::sort(ns.begin(), ns.end());
        r.round_trips = ns.size();
        r.p50_ns = ns[ns.size() / 2];
        r.p99_ns = ns[(ns.size() * 99) / 100];
    }
};

// ---- iox implementation -----------------------------------------------------

result run_iox(int seconds) {
    std::atomic<bool> stop{false};

    // Bind the iox acceptor up front on an ephemeral port (NOT the port of
    // listen_on_ephemeral's fd — that listener stays open for run_raw, and a
    // second bind on the same port would fail EADDRINUSE, leaving *acceptor an
    // empty expected and every accept on a garbage fd).
    auto acceptor = net::tcp::acceptor::listen(*net::endpoint::ipv4_any(0));
    if (!acceptor) {
        std::fprintf(stderr, "iox bench: listen failed: %s\n",
                     acceptor.error().message().c_str());
        return {};
    }
    sockaddr_in sin{};
    socklen_t slen = sizeof(sin);
    ::getsockname(acceptor->accept_handle().v, reinterpret_cast<sockaddr*>(&sin), &slen);
    const auto port = ntohs(sin.sin_port);
    std::atomic<int> live_sessions{0};
    const auto server_start = clock_t_::now();
    std::thread server([&] {
        io_context ctx;
        struct session {
            net::tcp::socket sock;
            std::unique_ptr<std::byte[]> buf = std::make_unique<std::byte[]>(kPayload);
        };
        auto serve_one = [&](net::tcp::socket s) {
            auto* sn = new session{std::move(s)};
            live_sessions.fetch_add(1);
            // eof must live in the loop lambda's capture (op state), NOT in
            // its body: the chain completes asynchronously after the body
            // has returned — a body-local would dangle.
            ex::detach(io::loop(ctx, [sn, &ctx, eof = false]() mutable {
                            return io::read(ctx, sn->sock,
                                            wbytes{sn->buf.get(), kPayload})
                                 | ex::let_value([sn, &ctx, &eof](std::size_t n) {
                                       eof = (n == 0);
                                       return io::write_all(
                                           ctx, sn->sock,
                                           rbytes{sn->buf.get(), n});
                                   })
                                 | ex::then([&eof] { return eof; });
                        })
                        | ex::upon_error([](auto&&) {})
                        | ex::then([sn, &live_sessions] {
                              delete sn;
                              live_sessions.fetch_sub(1);
                          }));
        };
        std::atomic<uint64_t> accepts{0};
        ex::detach(io::loop(ctx, [&] {
            return io::accept(ctx, *acceptor)
                 | ex::then([&](net::tcp::socket sk) {
                       accepts.fetch_add(1);
                       serve_one(std::move(sk));
                       return false; // keep accepting until the context stops
                   });
        }));
        // drain in-flight sessions after stop (clients closing => EOF),
        // bounded so a stuck peer cannot wedge the benchmark
        while ((!stop.load(std::memory_order_relaxed) || live_sessions.load() > 0)
               && clock_t_::now() < server_start + std::chrono::seconds(seconds)
                    + std::chrono::seconds(5)) {
            ctx.run_for(std::chrono::milliseconds(50));
        }
    });

    // client thread
    result r;
    std::thread client([&] {
        io_context ctx;
        samples s;
        std::atomic<int> finished{0};
        std::atomic<uint64_t> iters{0};

        struct slot {
            net::tcp::socket sock;
            std::unique_ptr<std::byte[]> buf = std::make_unique<std::byte[]>(kPayload);
            clock_t_::time_point t0;
            explicit slot(net::tcp::socket s) : sock(std::move(s)) {}
        };
        auto slots = std::make_unique<std::unique_ptr<slot>[]>(kConns * kInflight);
        for (int i = 0; i < kConns * kInflight; ++i) {
            auto sock = net::tcp::socket::unconnected(net::endpoint::family::ipv4);
            if (!sock) {
                return;
            }
            auto conn = ex::sync_wait(
                ctx, io::connect(ctx, *sock,
                                 *net::endpoint::ipv4("127.0.0.1", port)));
            if (!conn) {
                return;
            }
            slots[i] = std::make_unique<slot>(std::move(*sock));
        }

        for (int i = 0; i < kConns * kInflight; ++i) {
            slot* st = slots[i].get();
            ex::detach(
                io::loop(ctx, [st, &s, &stop, &ctx, &iters] {
                    st->t0 = clock_t_::now();
                    return io::write_all(ctx, st->sock, rbytes{st->buf.get(), kPayload})
                         | ex::let_value([st, &ctx] {
                               return io::read(ctx, st->sock,
                                               wbytes{st->buf.get(), kPayload});
                           })
                         | ex::then([st, &s, &stop, &iters](std::size_t n) {
                               ++iters;
                               if (n == kPayload) {
                                   s.add(clock_t_::now() - st->t0);
                               }
                               return stop.load(std::memory_order_relaxed);
                           });
                })
                | ex::upon_error([](auto&&) {})
                | ex::then([&] { finished.fetch_add(1); }));
        }

        const auto deadline = clock_t_::now() + std::chrono::seconds(seconds);
        while (clock_t_::now() < deadline) {
            ctx.run_for(std::chrono::milliseconds(20));
        }
        stop.store(true);
        const auto drain_deadline = clock_t_::now() + std::chrono::seconds(5);
        while (finished.load() < kConns * kInflight
               && clock_t_::now() < drain_deadline) {
            ctx.run_for(std::chrono::milliseconds(20));
        }
        s.finalize(r);
    });

    client.join();
    stop.store(true);
    server.join();
    return r;
}

// ---- raw liburing implementation --------------------------------------------

struct raw_conn {
    int fd = -1;
    bool awaiting_send = false; // true: a send is in flight (next: recv)
    std::byte buf[kPayload];
};

struct raw_ping {
    int fd = -1;
    bool want_read = false;
    clock_t_::time_point t0;
    std::byte buf[kPayload];
};

struct raw_ctx {
    uring::ring ring;
    std::vector<raw_conn> connections;
};

void raw_submit_echo(uring::ring& ring, raw_conn& c) {
    io_uring_sqe* sqe = ring.next_sqe();
    if (sqe == nullptr) {
        ring.flush();
        sqe = ring.next_sqe();
    }
    ::io_uring_prep_recv(sqe, c.fd, c.buf, kPayload, 0);
    ::io_uring_sqe_set_data(sqe, &c);
}

result run_raw(int seconds) {
    std::atomic<bool> stop{false};
    int listen_fd = -1;
    const auto port = listen_on_ephemeral(listen_fd);

    std::thread server([&] {
        uring::ring ring{uring::ring_params{.entries = 256}};
        struct accept_slot {
            iox::op_base base; // reuse dispatch shape
            static void thunk(iox::op_base*, io_context&, std::int32_t, std::uint32_t) noexcept {}
            accept_slot() noexcept : base(&thunk) {}
        };
        std::vector<accept_slot> accepts(8);
        std::vector<raw_conn> connections;
        connections.reserve(64);

        auto arm_accept = [&](int i) {
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
            }
            ::io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            ::io_uring_sqe_set_data(sqe, &accepts[i]);
        };
        for (int i = 0; i < 8; ++i) {
            arm_accept(i);
        }

        while (!stop.load(std::memory_order_relaxed)) {
            ring.flush_and_wait(1);
            ring.for_each_cqe([&](io_uring_cqe* cqe) {
                void* ud = io_uring_cqe_get_data(cqe);
                if (ud >= accepts.data() && ud < accepts.data() + accepts.size()) {
                    if (cqe->res >= 0) {
                        connections.push_back(raw_conn{cqe->res, false, {}});
                        raw_submit_echo(ring, connections.back());
                        arm_accept(static_cast<int>(
                            static_cast<accept_slot*>(ud) - accepts.data()));
                    }
                } else if (auto* c = static_cast<raw_conn*>(ud); c != nullptr) {
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    if (c->awaiting_send) { // send done → await the next request
                        ::io_uring_prep_recv(sqe, c->fd, c->buf, kPayload, 0);
                        ::io_uring_sqe_set_data(sqe, c);
                        c->awaiting_send = false;
                    } else { // recv done → echo back (64B may still be
                        // partial in theory; with kPayload-sized echoes on
                        // loopback it is not — same assumption both sides)
                        ::io_uring_prep_send(sqe, c->fd, c->buf,
                                             static_cast<std::size_t>(cqe->res),
                                             MSG_NOSIGNAL);
                        ::io_uring_sqe_set_data(sqe, c);
                        c->awaiting_send = true;
                    }
                }
            });
        }
        // stop: close every conn so pending client recvs complete (EOF/RST)
        // and the client loop can wind down — otherwise its flush_and_wait
        // blocks forever on sockets nobody serves anymore.
        for (auto& c : connections) {
            ::close(c.fd);
        }
    });

    result r;
    std::thread client([&] {
        uring::ring ring{uring::ring_params{.entries = 256}};
        samples s;
        std::vector<raw_ping> pings(kConns * kInflight);
        std::atomic<int> finished{0};
        std::atomic<uint64_t> iters{0};

        for (auto& p : pings) {
            p.fd = connect_one(port);
            p.want_read = false;
            io_uring_sqe* sqe = ring.next_sqe();
            if (sqe == nullptr) {
                ring.flush();
                sqe = ring.next_sqe();
            }
            ::memset(p.buf, 'x', kPayload);
            p.t0 = clock_t_::now();
            ::io_uring_prep_send(sqe, p.fd, p.buf, kPayload, MSG_NOSIGNAL);
            ::io_uring_sqe_set_data(sqe, &p);
            p.want_read = true;
        }

        while (finished.load() < kConns * kInflight) {
            ring.flush_and_wait(1);
            ring.for_each_cqe([&](io_uring_cqe* cqe) {
                auto* p = static_cast<raw_ping*>(io_uring_cqe_get_data(cqe));
                if (p == nullptr || p->fd < 0) {
                    return;
                }
                if (p->want_read) { // send completed → recv
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    ::io_uring_prep_recv(sqe, p->fd, p->buf, kPayload, 0);
                    ::io_uring_sqe_set_data(sqe, p);
                    p->want_read = false;
                } else { // recv completed → record + send again
                    if (cqe->res == static_cast<std::int32_t>(kPayload)) {
                        s.add(clock_t_::now() - p->t0);
                    }
                    if (stop.load(std::memory_order_relaxed)) {
                        p->fd = -1;
                        finished.fetch_add(1);
                        return;
                    }
                    io_uring_sqe* sqe = ring.next_sqe();
                    if (sqe == nullptr) {
                        ring.flush();
                        sqe = ring.next_sqe();
                    }
                    p->t0 = clock_t_::now();
                    ::io_uring_prep_send(sqe, p->fd, p->buf, kPayload, MSG_NOSIGNAL);
                    ::io_uring_sqe_set_data(sqe, p);
                    p->want_read = true;
                }
            });
        }
        s.finalize(r);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread([seconds, &stop] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop.store(true);
    }).detach();

    client.join();
    server.join();
    ::close(listen_fd);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 2;
    std::printf("TCP echo, %d connections × %d in-flight × %zu B, %ds windows\n", kConns,
                kInflight, kPayload, seconds);

    const result iox_r = run_iox(seconds);
    const result raw_r = run_raw(seconds);

    auto line = [](const char* who, const result& r) {
        std::printf("  %-5s: %8.0f kpps   p50 %7.1f us   p99 %7.1f us\n", who,
                    static_cast<double>(r.round_trips) / 1e3,
                    r.p50_ns / 1000.0, r.p99_ns / 1000.0);
    };
    line("iox", iox_r);
    line("raw", raw_r);
    if (raw_r.round_trips != 0) {
        std::printf("  ratio : %.0f %% of raw round-trips\n",
                    100.0 * static_cast<double>(iox_r.round_trips) /
                        static_cast<double>(raw_r.round_trips));
    }
    return 0;
}
