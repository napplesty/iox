// iox — unified async IO for Linux
// include/iox/runtime/source_registry_impl.inc — the method bodies of source_registry,

inline source_registry::source_registry() noexcept {
    busy_tick_.registry = this;
}

inline void source_registry::source_watch::arm(io_context& ctx) noexcept {
    if (io_uring_sqe* sqe = ctx.acquire_sqe(*this)) {
        ::io_uring_prep_poll_add(sqe, source->completion_fd().v, POLLIN);
    } else {
        retiring = true;
        parked = true;
    }
}

inline void source_registry::source_watch::on_fired(op_base* self, io_context& ctx,
                                                    std::int32_t res,
                                                    std::uint32_t) noexcept {
    auto& w = *static_cast<source_watch*>(self);
    if (w.retiring || res == -ECANCELED || res == -512) {
        w.parked = true;
        return;
    }
    if (res > 0) {
        w.source->on_ready(ctx);
    }
    if (!w.retiring) {
        w.arm(ctx);
    } else {
        w.parked = true;
    }
}

inline void source_registry::busy_tick::on_fired(op_base* self, io_context& ctx,
                                                 std::int32_t res,
                                                 std::uint32_t) noexcept {
    auto& t = *static_cast<busy_tick*>(self);
    t.in_flight = false;
    if (res == -ECANCELED || res == -512) {
        return;
    }
    for (std::size_t i = 0; i < t.registry->busy_sources_.size(); ++i) {
        completion_source* s = t.registry->busy_sources_[i];
        if (s->has_work()) {
            s->on_ready(ctx);
        }
    }
    if (t.active) {
        arm(ctx, t);
    }
}

inline void source_registry::busy_tick::arm(io_context& ctx, busy_tick& t) noexcept {
    namespace chrono = std::chrono;
    if (t.in_flight) {
        return;
    }
    if (io_uring_sqe* sqe = ctx.acquire_sqe(t)) {
        const auto when = chrono::steady_clock::now() + chrono::milliseconds{1};
        t.ts.tv_sec = chrono::duration_cast<chrono::seconds>(when.time_since_epoch()).count();
        t.ts.tv_nsec = (chrono::duration_cast<chrono::nanoseconds>(when.time_since_epoch()) %
                        chrono::seconds{1}).count();
        ::io_uring_prep_timeout(sqe, &t.ts, 0, IORING_TIMEOUT_ABS);
        t.in_flight = true;
    }
}

inline void source_registry::arm_busy_tick(io_context& ctx) noexcept {
    busy_tick_.active = true;
    busy_tick::arm(ctx, busy_tick_);
}

inline void source_registry::stop_busy_tick(io_context& ctx) noexcept {
    if (busy_tick_.active) {
        busy_tick_.active = false;
        if (busy_tick_.in_flight) {
            ctx.submit_cancel(reinterpret_cast<std::uint64_t>(&busy_tick_));
        }
    }
}

inline void source_registry::sweep_retired() noexcept {
    std::erase_if(sources_, [](const source_reg& r) {
        return r.watch && r.watch->retiring && r.watch->parked;
    });
}

inline void source_registry::attach(io_context& ctx, completion_source& s) noexcept {
    sweep_retired();
    if (attached(s)) {
        return;
    }
    if (const iox::fd f = s.completion_fd(); f.valid()) {
        auto w = std::make_unique<source_watch>();
        w->source = &s;
        w->arm(ctx);
        sources_.push_back(source_reg{&s, std::move(w)});
    } else {
        busy_sources_.push_back(&s);
        arm_busy_tick(ctx);
    }
}

inline void source_registry::detach(io_context& ctx, completion_source& s) noexcept {
    sweep_retired();
    for (source_reg& r : sources_) {
        if (r.source == &s && r.watch && !r.watch->retiring) {
            r.watch->retiring = true;
            ctx.submit_cancel(reinterpret_cast<std::uint64_t>(r.watch.get()));
        }
    }
    std::erase_if(sources_, [&s](const source_reg& r) {
        return r.source == &s && r.watch && r.watch->retiring && r.watch->parked;
    });
    std::erase(busy_sources_, &s);
    if (busy_sources_.empty()) {
        stop_busy_tick(ctx);
    }
}

inline bool source_registry::attached(const completion_source& s) const noexcept {
    for (const source_reg& r : sources_) {
        if (r.source == &s && !(r.watch && r.watch->retiring)) {
            return true;
        }
    }
    for (const completion_source* b : busy_sources_) {
        if (b == &s) {
            return true;
        }
    }
    return false;
}
