// iox — unified async IO for Linux
// include/iox/iox.h — the one-include umbrella for the whole public API.
#pragma once

#include "iox/runtime/io_context.h"
#include "iox/runtime/blocking_pool.h"
#include "iox/core/exec.h"

#include "iox/ops.h"
#include "iox/compose/detach.h"
#include "iox/compose/loop.h"
#include "iox/compose/pump.h"
#include "iox/compose/write_all.h"

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

#include "iox/driver/capabilities.h"
#include "iox/driver/completion_source.h"
#include "iox/driver/registry.h"
