// iox — unified async IO for Linux
#include <iox/core/error.h>

namespace iox::detail {

const char* errno_name(int e) noexcept {
    switch (e) {
    case 0: return "OK";
    case EAGAIN: return "EAGAIN";
    case EBADF: return "EBADF";
    case EBUSY: return "EBUSY";
    case ECANCELED: return "ECANCELED";
    case EFAULT: return "EFAULT";
    case EINTR: return "EINTR";
    case EINVAL: return "EINVAL";
    case EIO: return "EIO";
    case ENOENT: return "ENOENT";
    case ENOMEM: return "ENOMEM";
    case ESRCH: return "ESRCH";
    case ECHILD: return "ECHILD";
    case EXDEV: return "EXDEV";
    case ENOTTY: return "ENOTTY";
    case EADDRINUSE: return "EADDRINUSE";
    case EISCONN: return "EISCONN";
    case ENOTSOCK: return "ENOTSOCK";
    case EPROTOTYPE: return "EPROTOTYPE";
    case ENODATA: return "ENODATA";
    case ENOMSG: return "ENOMSG";
    case ENOSPC: return "ENOSPC";
    case ENODEV: return "ENODEV";
    case EACCES: return "EACCES";
    case EDEADLK: return "EDEADLK";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case EOPNOTSUPP: return "EOPNOTSUPP";
    case EOVERFLOW: return "EOVERFLOW";
    case EPERM: return "EPERM";
    case EPIPE: return "EPIPE";
    case ECONNABORTED: return "ECONNABORTED";
    case ECONNREFUSED: return "ECONNREFUSED";
    case ECONNRESET: return "ECONNRESET";
    case EINPROGRESS: return "EINPROGRESS";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case ENOTCONN: return "ENOTCONN";
    case ETIMEDOUT: return "ETIMEDOUT";
    default: return "ERRNO"; // EWOULDBLOCK is EAGAIN on Linux
    }
}

}
