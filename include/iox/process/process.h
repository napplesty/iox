// iox — process/process.h: a spawned child: pid + pidfd.
#pragma once

#include <cstdint>
#include <sys/wait.h>

#include <spawn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <csignal>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "iox/core/error.h"
#include "iox/core/fd.h"
#include "iox/pipe/channel.h"

namespace iox::process {

class process {
public:
    struct io_plan {
        std::optional<pipe::read_end> in;
        std::optional<pipe::write_end> out;
        std::optional<pipe::write_end> err;
    };

    static std::expected<process, error> spawn(std::initializer_list<std::string_view> argv,
                                               io_plan io = {}) noexcept {
        if (argv.size() == 0) {
            return std::unexpected(error::from_errno(EINVAL));
        }
        std::vector<const char*> raw;
        raw.reserve(argv.size() + 1);
        for (std::string_view a : argv) {
            raw.push_back(a.data());
        }
        raw.push_back(nullptr);

        ::posix_spawn_file_actions_t file_actions;
        if (::posix_spawn_file_actions_init(&file_actions) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        const int wire[][2] = {{io.in ? io.in->read_handle().v : -1, 0},
                               {io.out ? io.out->write_handle().v : -1, 1},
                               {io.err ? io.err->write_handle().v : -1, 2}};
        for (auto [end, slot] : wire) {
            if (end >= 0) {
                ::posix_spawn_file_actions_adddup2(&file_actions, end, slot);
                ::posix_spawn_file_actions_addclose(&file_actions, end);
            }
        }

        ::pid_t pid = -1;
        const int result = ::posix_spawnp(&pid, raw[0], &file_actions, nullptr,
                                      const_cast<char**>(raw.data()), environ);
        ::posix_spawn_file_actions_destroy(&file_actions);
        io = {};
        if (result != 0) {
            return std::unexpected(error::from_errno(result));
        }

        const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd < 0) {
            const int e = errno;
            ::kill(pid, SIGKILL); // don't leak a child we cannot track
            int status = 0;
            (void)!::waitpid(pid, &status, 0);
            return std::unexpected(error::from_errno(e));
        }
        return process{pid, pidfd};
    }

    process() noexcept = default;

    process(process&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)), pidfd_(std::move(other.pidfd_)) {}
    process& operator=(process&& other) noexcept {
        if (this != &other) {
            reset();
            pid_ = std::exchange(other.pid_, -1);
            pidfd_ = std::move(other.pidfd_);
        }
        return *this;
    }
    process(const process&) = delete;
    process& operator=(const process&) = delete;

    bool valid() const noexcept { return pidfd_.valid(); }

    ::pid_t pid() const noexcept { return pid_; }
    iox::fd pidfd() const noexcept { return pidfd_.get(); }

    // Signal the child through the pidfd — race-free by construction.
    std::expected<void, error> kill(int sig) const noexcept {
        if (::syscall(SYS_pidfd_send_signal, pidfd_.get().v, sig, nullptr, 0) != 0) {
            return std::unexpected(error::from_errno(errno));
        }
        return {};
    }

    void reset() noexcept {
        pidfd_.reset();
        pid_ = -1;
    }

private:
    process(::pid_t pid, int raw_fd) noexcept : pid_(pid), pidfd_(raw_fd) {}

    ::pid_t pid_ = -1;
    iox::unique_fd pidfd_{};
};

static_assert(std::is_nothrow_move_constructible_v<process> &&
              !std::is_copy_constructible_v<process>);

}
