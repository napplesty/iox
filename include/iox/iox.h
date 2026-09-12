// iox — unified async IO for Linux
// iox.h — the one-include umbrella for the whole public API.
//
// Design §二: L4 domain handles + L3 vocabulary + L2 io_context, all in one
// header. Prefer this unless you need a narrow dependency slice (e.g. only
// iox/ops/read.h for the CPO); every sub-include is self-contained.
//
// Deliberately NOT included here: the reference drivers (iox/nvme/,
// iox/xdp/) pull kernel uapi headers and hardware expectations with them —
// include those explicitly when the driver is actually used.
#pragma once

// L2 runtime + execution layer
#include "iox/runtime/io_context.h"
#include "iox/runtime/blocking_pool.h"
#include "iox/core/exec.h"

// L3 unified vocabulary + combinators
#include "iox/ops.h"
#include "iox/compose/detach.h"
#include "iox/compose/loop.h"
#include "iox/compose/pump.h"
#include "iox/compose/write_all.h"

// L4 domain handles
#include "iox/fs/file.h"
#include "iox/fs/watcher.h"
#include "iox/net/endpoint.h"
#include "iox/net/tcp.h"
#include "iox/net/udp.h"
#include "iox/net/unix.h"
#include "iox/net/dns.h"
#include "iox/pipe/channel.h"
#include "iox/signal/set.h"
#include "iox/signal/watcher.h"
#include "iox/process/process.h"
#include "iox/stdio/stream.h"

// Driver SPI (for extending, not for using the library)
#include "iox/driver/capabilities.h"
#include "iox/driver/completion_source.h"
#include "iox/driver/registry.h"
