# Ordering safe-crash-log records: `backtrace_id` and `backtrace_line`

Date: 2026-07-06

## Problem

When the heartbeat thread detects heartbeat loss, `check_heartbeats`
(`src/threading.c`) calls `jl_print_task_backtraces`
(`src/stackwalk.c:1228`).  That function emits its output one line at a time
through `jl_safe_printf` (`src/jl_uv.c:759`).  When a safe-crash-log file is
configured via `--safe-crash-log-file`, each `jl_safe_printf` call is wrapped
into a single-line JSON object by `write_to_safe_crash_log`
(`src/jl_uv.c:688`) and appended to the file:

```json
{"level":"Error", "timestamp":"2026-07-06T12:34:56.789", "message": "..."}
```

The `timestamp` has only millisecond precision.  A single backtrace dump emits
many records in far less than a millisecond, so successive records routinely
share an identical timestamp.  Observability platforms that sort by timestamp
may then present those records in an arbitrary order, which makes a dumped
backtrace very hard to read.

## Goal

Add two JSON attributes so a consumer can deterministically reconstruct the
order of the records that make up one backtrace dump, and can tell records of
one dump apart from records of another dump emitted at a different time:

- `backtrace_id` — identifies a single `jl_print_task_backtraces` invocation.
- `backtrace_line` — the ordinal of a record within that invocation.

## Scope

The two attributes are emitted **only** on records produced while a thread is
executing inside `jl_print_task_backtraces`.  All other safe-crash-log
records keep their current JSON shape unchanged, with neither attribute
present.  This includes:

- the `==== heartbeat loss (Ns) ====` message (emitted before the dump),
- signal-handler crash output,
- "waiting for the world" messages.

The framing lines that `jl_print_task_backtraces` itself prints (`++++ Task
backtraces`, `==== Thread N ...`, `++++ Done`, etc.) are part of the dump and
therefore are tagged.

## Design

### State (in `src/jl_uv.c`, next to `write_to_safe_crash_log`)

```c
// Global: yields a unique id (>= 1) for each backtrace dump.
static _Atomic(int64_t) bt_dump_counter;

// Per printing thread: id of the dump currently in progress on this
// thread; 0 means "not inside a dump". Line ordinal within that dump,
// reset at each dump start.
static __thread int64_t bt_dump_id;
static __thread int64_t bt_dump_line;
```

`__thread` is the established thread-local idiom in this tree
(`src/threading.c:244`, `src/julia_fasttls.h`).  Keeping `bt_dump_id` and
`bt_dump_line` thread-local (rather than global) means:

- Only the thread actually running the dump tags its records.
- Concurrent dumps on different threads each receive a distinct
  `backtrace_id` (from the atomic global counter) and maintain an independent
  `backtrace_line` sequence with no cross-thread contention.

The global counter is accessed with `jl_atomic_fetch_add_relaxed`, which is
lock-free and async-signal-safe, matching existing atomic usage in this file
(e.g., `src/jl_uv.c:122`).

### Dump lifecycle hooks (exported from `src/jl_uv.c`)

```c
JL_DLLEXPORT void jl_backtrace_dump_begin(void) JL_NOTSAFEPOINT
{
    bt_dump_id = jl_atomic_fetch_add_relaxed(&bt_dump_counter, 1) + 1;
    bt_dump_line = 0;
}

JL_DLLEXPORT void jl_backtrace_dump_end(void) JL_NOTSAFEPOINT
{
    bt_dump_id = 0;
}
```

`jl_print_task_backtraces` (`src/stackwalk.c:1228`) calls
`jl_backtrace_dump_begin()` at the top (after the existing heartbeat pause)
and `jl_backtrace_dump_end()` at the bottom (before the heartbeat resume),
with an `extern` declaration alongside the existing
`extern int jl_inside_heartbeat_thread(void);` at `src/stackwalk.c:1222`.

### Emission (in `write_to_safe_crash_log`)

When `bt_dump_id != 0`, emit the two attributes between `timestamp` and
`message`, then advance the line counter:

```json
{"level":"Error", "timestamp":"...", "backtrace_id": 3, "backtrace_line": 12, "message": "..."}
```

The integer fields are formatted with `sprintf`, matching the existing
millisecond-fraction formatting in the same function
(`src/jl_uv.c:712`).  `bt_dump_line` is incremented after the record is
composed.

### Truncation-budget fix

`write_to_safe_crash_log` currently computes the message budget from a
hardcoded constant:

```c
const int max_b = wbuflen - 70 - 3;
```

This assumes a fixed-length preamble.  With the optional integer attributes the
preamble length varies, so the message-truncation loop must derive its budget
from the **actual** preamble length after the (possibly present) attributes are
written, rather than from a constant.  The message loop reserves the fixed tail
(`"}\n` plus the terminating NUL, and room for the `...` ellipsis) against the
real remaining space in `wbuf`.  The 2048-byte `wbuf` is large enough to
absorb the worst-case attribute prefix (two `int64` fields plus their JSON
keys, roughly 80 bytes) while still leaving ample room for the message.

## Assumptions

- **SCL-1 (`write_to_safe_crash_log` / `jl_print_task_backtraces`):** a single
  backtrace dump is printed serially by one thread, so the framing and frame
  records of that dump are the only records that should carry `backtrace_id`
  and `backtrace_line`.  Records emitted by *another* thread's signal handler
  during a concurrent dump window are not tagged, because the current dump id
  and line ordinal live in the thread-local storage of the printing thread.

## Testing

`write_to_safe_crash_log` writes to a real file descriptor
(`jl_sig_fd`) and is only active from signal-handler / heartbeat /
waiting-for-the-world contexts, which are awkward to drive from a unit test.
Verification will therefore be an integration check:

1. Run a Julia process with `--safe-crash-log-file` pointed at a temp file and
   a heartbeat configuration that triggers `jl_print_task_backtraces`.
2. Assert that every record emitted during the dump carries both
   `backtrace_id` and `backtrace_line`, that `backtrace_line` is a gap-free
   `0..N-1` sequence, and that all records of one dump share a single
   `backtrace_id`.
3. Assert that non-dump records (e.g., the heartbeat-loss message) carry
   neither attribute.
4. Assert that a second dump uses a strictly greater `backtrace_id` and
   restarts `backtrace_line` at 0.

The exact test harness will be settled in the implementation plan.
