// iox — unified async IO for Linux
// ops.h — the unified vocabulary, one include for every operation
// (design §二 L3).
//
// Each operation is a minimal unit header under iox/ops/ (one CPO + its
// policy per file, plus fd_sender.h for the shared skeleton); include
// those directly when you want a narrower dependency. Combinators built on
// the vocabulary (loop / detach / write_all) live in iox/compose/.
#pragma once

#include "iox/ops/accept.h"
#include "iox/ops/close.h"
#include "iox/ops/connect.h"
#include "iox/ops/fsync.h"
#include "iox/ops/open.h"
#include "iox/ops/poll.h"
#include "iox/ops/read.h"
#include "iox/ops/read_at.h"
#include "iox/ops/recv_from.h"
#include "iox/ops/schedule.h"
#include "iox/ops/send_to.h"
#include "iox/ops/signal.h"
#include "iox/ops/splice.h"
#include "iox/ops/tee.h"
#include "iox/ops/timer.h"
#include "iox/ops/wait_pid.h"
#include "iox/ops/write.h"
#include "iox/ops/write_at.h"
