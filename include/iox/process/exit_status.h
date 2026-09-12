// iox — unified async IO for Linux
// process/exit_status.h — how a child ended. A small parsed view over the
// waitid siginfo: raw kernel encoding stays out of call sites.
#pragma once

#include <signal.h>
#include <sys/wait.h>

namespace iox::process {

struct exit_status {
    /// Exit code (valid when exited) or terminating signal (when signaled).
    int code = -1;
    bool exited = false;
    bool signaled = false;
    bool dumped_core = false;

    static exit_status from_siginfo(const ::siginfo_t& si) noexcept {
        exit_status s;
        s.exited = (si.si_code == CLD_EXITED);
        s.signaled = (si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED);
        s.dumped_core = (si.si_code == CLD_DUMPED);
        s.code = si.si_status;
        return s;
    }

    /// The usual question: did it run to completion successfully?
    bool success() const noexcept { return exited && code == 0; }
};

} // namespace iox::process
