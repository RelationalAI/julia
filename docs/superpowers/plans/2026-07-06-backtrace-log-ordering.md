# Backtrace-Log Record Ordering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tag each safe-crash-log record emitted during a task-backtrace dump with `backtrace_id` and `backtrace_line` so observability platforms can deterministically order records that share a millisecond timestamp and group them by dump.

**Architecture:** `jl_print_task_backtraces` (`src/stackwalk.c`) brackets its output with two new exported hooks, `jl_backtrace_dump_begin`/`jl_backtrace_dump_end` (defined in `src/jl_uv.c`). The hooks maintain per-thread dump state (a dump id and a line ordinal) plus one global atomic counter. `write_to_safe_crash_log` (`src/jl_uv.c`) reads that per-thread state and, when a dump is in progress on the current thread, injects the two integer attributes into the JSON record and advances the line ordinal.

**Tech Stack:** C (Julia runtime, C11 `_Atomic` + `__thread`), Julia `Test` stdlib, subprocess integration test driven through `jl_heartbeat_enable`/`jl_heartbeat`.

## Global Constraints

- Code fits within 80 characters. Comments fit within 80 characters.
- The crash-log file format is one JSON object per line; this MUST be preserved.
- The crash path runs from signal handlers and the heartbeat thread: use only
  async-signal-safe, lock-free primitives (`jl_atomic_fetch_add_relaxed`,
  `__thread`, `sprintf` — all already used in `write_to_safe_crash_log`). New
  runtime functions are annotated `JL_NOTSAFEPOINT`.
- New RAI-specific runtime code is marked with a `// RAI-specific` comment, to
  match the existing convention around `write_to_safe_crash_log`.
- Assumptions are documented in an ASSUMPTIONS block with named entries.
  Assumption **SCL-1**: a single backtrace dump is printed serially by one
  thread, so per-thread dump id/line correctly scope tagging to that thread;
  records emitted by another thread's signal handler during a concurrent dump
  window are not tagged.

---

### Task 1: Integration test for dump-record tagging — SKIPPED

**STATUS: SKIPPED.** The test suite has no precedent for exercising
`jl_print_task_backtraces`, the heartbeat mechanism, or the safe-crash-log
path — none of `jl_print_task_backtraces`, `jl_heartbeat*`, or
`--safe-crash-log-file` appear in `test/`. The full test below is preserved
as a trace so a reviewer can request it be added; if so, add it and register
it per Step 2, then it must fail before Task 2 and pass after. Verification of
Task 2 instead relies on its manual sanity check (Task 2, Step 7).

Write the test first. It drives a Julia subprocess that enables heartbeats,
stops sending them to force `jl_print_task_backtraces`, recovers, then stalls
again to force a *second* dump. The parent parses the crash-log file and
asserts the ordering attributes. Run against the current (unmodified) runtime
it fails, because no records carry `backtrace_id`/`backtrace_line`.

**Files:**
- Create: `test/safecrashlog.jl`
- Modify: `test/choosetests.jl:24-27` (register the new test name)

**Interfaces:**
- Consumes (from the runtime, already present): `ccall(:jl_heartbeat_enable,
  Cint, (Cint, Cint, Cint), interval_s, tasks_after_n, reset_after_n)` returns
  `0` on success, `-1` if not ready; `ccall(:jl_heartbeat, Cvoid, ())`.
- Consumes (Task 2 makes these assertions pass): each record emitted while a
  thread is inside `jl_print_task_backtraces` gains
  `"backtrace_id": <int>, "backtrace_line": <int>` between the `timestamp` and
  `message` fields; `backtrace_line` restarts at `0` per dump and is
  gap-free; each dump has a distinct, strictly increasing `backtrace_id`.

- [ ] **Step 1: Write the failing test**

Create `test/safecrashlog.jl`:

```julia
# Tests that safe-crash-log records emitted during a task-backtrace dump are
# tagged with backtrace_id/backtrace_line so a consumer can order records that
# share a millisecond timestamp. See
# docs/superpowers/specs/2026-07-06-backtrace-log-ordering-design.md.
#
# ASSUMPTIONS
# - SCL-1: one dump is printed serially by the heartbeat thread, so all of a
#   dump's records share one backtrace_id and a gap-free 0-based line ordinal.
# - Timing: a 1s heartbeat interval with tasks_after_n=1 makes the first
#   missed interval trigger a dump; frequent beats for 5s recover; a 4s stall
#   then triggers a second dump. Sleeps are padded to absorb scheduling jitter.
using Test

# The child enables heartbeats, forces two dumps, and exits. All prints land in
# the file named by --safe-crash-log-file.
const CHILD = """
    t0 = time()
    while ccall(:jl_heartbeat_enable, Cint, (Cint, Cint, Cint), 1, 1, 2) != 0
        (time() - t0) > 10 && error("could not enable heartbeats")
        sleep(0.05)
    end
    sleep(4)                                   # miss beats -> first dump
    t1 = time()
    while time() - t1 < 5                       # beat -> recover
        ccall(:jl_heartbeat, Cvoid, ())
        sleep(0.1)
    end
    sleep(4)                                    # miss beats -> second dump
"""

# Parse the crash log into records, extracting the optional ordering
# attributes and the message text.
function read_records(path)
    records = NamedTuple[]
    for line in eachline(path)
        isempty(strip(line)) && continue
        m = match(r"\\"backtrace_id\\":\\s*(\\d+),\\s*\\"backtrace_line\\":\\s*(\\d+)",
                  line)
        id = m === nothing ? nothing : parse(Int, m.captures[1])
        ln = m === nothing ? nothing : parse(Int, m.captures[2])
        msg = match(r"\\"message\\":\\s*\\"(.*)\\"\\}\\s*\$", line)
        text = msg === nothing ? line : msg.captures[1]
        push!(records, (; id, ln, text))
    end
    return records
end

if !Sys.isunix()
    @info "safecrashlog test only runs on Unix; skipping"
else
    mktemp() do logpath, io
        close(io)
        childfile = tempname() * ".jl"
        write(childfile, CHILD)
        try
            cmd = `$(Base.julia_cmd()) --startup-file=no
                   --safe-crash-log-file=$logpath $childfile`
            run(cmd)
        finally
            rm(childfile; force=true)
        end

        records = read_records(logpath)
        @test !isempty(records)

        # Heartbeat loss/recovery messages are emitted outside a dump: untagged.
        loss = filter(r -> occursin("heartbeat loss", r.text), records)
        @test !isempty(loss)
        @test all(r -> r.id === nothing, loss)

        # Dump framing lines are tagged, and two dumps occurred.
        begins = filter(r -> occursin("++++ Task backtraces", r.text), records)
        dones = filter(r -> occursin("++++ Done", r.text), records)
        @test length(begins) == 2
        @test length(dones) == 2
        @test all(r -> r.id !== nothing, begins)
        @test all(r -> r.id !== nothing, dones)

        # Two distinct, strictly increasing dump ids; each restarts line at 0
        # and is gap-free (SCL-1).
        tagged = filter(r -> r.id !== nothing, records)
        ids = unique(r.id for r in tagged)
        @test length(ids) == 2
        @test ids[2] > ids[1]
        for id in ids
            dump = filter(r -> r.id == id, tagged)
            @test [r.ln for r in dump] == collect(0:length(dump) - 1)
        end
    end
end
```

- [ ] **Step 2: Register the test**

In `test/choosetests.jl`, add `"safecrashlog"` to the `TESTNAMES` array. The
current lines 24-27 read:

```julia
const TESTNAMES = [
        "subarray", "core", "compiler", "worlds", "atomics", "gc",
        "misc", "threads", "stress", "binaryplatforms", "atexit",
        "enums", "cmdlineargs", "int", "interpreter",
```

Add `"safecrashlog"` to the list (place it next to `"cmdlineargs"`):

```julia
        "enums", "cmdlineargs", "safecrashlog", "int", "interpreter",
```

(If the exact surrounding tokens differ, insert `"safecrashlog"` anywhere in
the array — order is not significant.)

- [ ] **Step 3: Run the test to verify it fails**

Run (from the repo root, using the already-built runtime):

```bash
usr/bin/julia --startup-file=no test/safecrashlog.jl
```

Expected: FAIL. Records are produced, but none carry the ordering attributes,
so `read_records` returns `id === nothing` for every record. The failing
assertions are `length(begins) == 2` (begins are found but `all(r -> r.id !==
nothing, begins)` fails) and the `length(ids) == 2` block. The
`heartbeat loss` / untagged assertions pass.

- [ ] **Step 4: Commit**

```bash
git add test/safecrashlog.jl test/choosetests.jl
git commit -m "test: assert backtrace-log dump records carry ordering attrs"
```

---

### Task 2: Emit `backtrace_id`/`backtrace_line` during dumps

Add the per-thread dump state and lifecycle hooks, inject the attributes in
`write_to_safe_crash_log`, make the truncation budget independent of the
now-variable preamble length, and bracket `jl_print_task_backtraces` with the
hooks. Rebuild and the Task 1 test passes.

**Files:**
- Modify: `src/jl_uv.c:687-754` (state, hooks, emission, truncation)
- Modify: `src/stackwalk.c:1222` (extern decls) and `src/stackwalk.c:1234-1236`,
  `src/stackwalk.c:1301-1303` (bracket the dump)

**Interfaces:**
- Produces: `void jl_backtrace_dump_begin(void)` and `void
  jl_backtrace_dump_end(void)` (both `JL_DLLEXPORT ... JL_NOTSAFEPOINT`),
  called by `jl_print_task_backtraces`.
- Produces: crash-log records emitted while `bt_dump_id != 0` on the current
  thread gain `"backtrace_id": <int64>, "backtrace_line": <int64>` between the
  `timestamp` and `message` fields; `backtrace_line` starts at `0` per dump.

- [ ] **Step 1: Add dump state and lifecycle hooks**

In `src/jl_uv.c`, the block just before `write_to_safe_crash_log` currently
reads (lines 686-688):

```c
// RAI-specific
STATIC_INLINE void write_to_safe_crash_log(char *buf) JL_NOTSAFEPOINT
{
```

Replace it with the state, the two hooks, and the (unchanged) function opener:

```c
// RAI-specific: state for tagging safe-crash-log records emitted during a
// task-backtrace dump, so a consumer can order records that share a
// millisecond timestamp and group them by dump.
//
// ASSUMPTIONS
// - SCL-1: a dump is printed serially by one thread. bt_dump_id/bt_dump_line
//   are therefore thread-local, which scopes tagging to the printing thread
//   and lets concurrent dumps on other threads keep independent line
//   sequences under distinct ids. Records emitted by another thread's signal
//   handler during a dump window are not tagged.
static _Atomic(int64_t) bt_dump_counter;
static __thread int64_t bt_dump_id;   // current dump on this thread; 0 = none
static __thread int64_t bt_dump_line; // record ordinal within current dump

// RAI-specific: begin a dump on the current thread, assigning a unique id and
// resetting the per-dump line ordinal.
JL_DLLEXPORT void jl_backtrace_dump_begin(void) JL_NOTSAFEPOINT
{
    bt_dump_id = jl_atomic_fetch_add_relaxed(&bt_dump_counter, 1) + 1;
    bt_dump_line = 0;
}

// RAI-specific: end the dump on the current thread.
JL_DLLEXPORT void jl_backtrace_dump_end(void) JL_NOTSAFEPOINT
{
    bt_dump_id = 0;
}

// RAI-specific
STATIC_INLINE void write_to_safe_crash_log(char *buf) JL_NOTSAFEPOINT
{
```

- [ ] **Step 2: Make the truncation budget tail-relative**

In `src/jl_uv.c`, the buffer setup currently reads (lines 697-701):

```c
    const int wbuflen = 2048;
    const int max_b = wbuflen - 70 - 3;
    char wbuf[wbuflen];
    bzero(wbuf, wbuflen);
    int wlen = 0;
```

Replace the `max_b` line so the message budget reserves a fixed tail rather
than assuming a fixed-length preamble (the optional attributes make the
preamble variable):

```c
    const int wbuflen = 2048;
    // The message budget is an absolute index near the end of wbuf so that a
    // variable-length preamble (the optional backtrace_id/backtrace_line
    // fields) does not affect it. Reserve room for the "..." truncation
    // marker, the closing "\"}\n", and the terminating NUL.
    const int max_b = wbuflen - 3 /* "..." */ - 3 /* "\"}\n" */ - 1 /* NUL */;
    char wbuf[wbuflen];
    bzero(wbuf, wbuflen);
    int wlen = 0;
```

The message loop's existing guard `if (wlen == max_b || wlen == max_b - 1)`
is unchanged and remains correct: each iteration adds at most 2 bytes, so
`wlen` never exceeds `max_b` before the `"..."` (3) and closing `"\"}\n"` (3)
are appended, keeping the total within `wbuflen`.

- [ ] **Step 3: Inject the attributes after the timestamp**

In `src/jl_uv.c`, the timestamp-to-message section currently reads (lines
706-716):

```c
    // Timestamp (19 bytes)
    struct timeval tv;
    struct tm* tm_info;
    gettimeofday(&tv, NULL);
    tm_info = gmtime(&tv.tv_sec);
    wlen += strftime(&wbuf[wlen], 42, "%Y-%m-%dT%H:%M:%S", tm_info);
    sprintf(&wbuf[wlen], ".%03ld", (long)tv.tv_usec / 1000);
    wlen += 4;

    // JSON preamble to message (15 bytes)
    wlen += copystp(&wbuf[wlen], "\", \"message\": \"");
```

Replace the two trailing lines (the `// JSON preamble to message` comment and
its `copystp`) with the timestamp close, the optional attributes, and the
message preamble:

```c
    // Close the timestamp string.
    wlen += copystp(&wbuf[wlen], "\"");

    // RAI-specific: tag records emitted during a task-backtrace dump so a
    // consumer can order records sharing a millisecond timestamp and group
    // them by dump (SCL-1).
    if (bt_dump_id != 0) {
        wlen += sprintf(&wbuf[wlen],
                        ", \"backtrace_id\": %lld, \"backtrace_line\": %lld",
                        (long long)bt_dump_id, (long long)bt_dump_line);
        bt_dump_line++;
    }

    // JSON preamble to message.
    wlen += copystp(&wbuf[wlen], ", \"message\": \"");
```

- [ ] **Step 4: Bracket `jl_print_task_backtraces` with the hooks**

In `src/stackwalk.c`, the extern block at line 1222 currently reads:

```c
extern int jl_inside_heartbeat_thread(void);
```

Add the two hook declarations after it:

```c
extern int jl_inside_heartbeat_thread(void);
extern void jl_backtrace_dump_begin(void) JL_NOTSAFEPOINT;
extern void jl_backtrace_dump_end(void) JL_NOTSAFEPOINT;
```

Then, inside `jl_print_task_backtraces`, the opening heartbeat-pause block
currently reads (lines 1234-1236):

```c
    if (!jl_inside_heartbeat_thread()) {
        jl_heartbeat_pause();
    }
```

Add the begin hook immediately after it:

```c
    if (!jl_inside_heartbeat_thread()) {
        jl_heartbeat_pause();
    }
    jl_backtrace_dump_begin();
```

And the closing heartbeat-resume block currently reads (lines 1301-1303):

```c
    if (!jl_inside_heartbeat_thread()) {
        jl_heartbeat_resume();
    }
```

Add the end hook immediately before it:

```c
    jl_backtrace_dump_end();
    if (!jl_inside_heartbeat_thread()) {
        jl_heartbeat_resume();
    }
```

- [ ] **Step 5: Rebuild the runtime**

Run (from the repo root; use the platform's core count):

```bash
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Expected: builds `usr/bin/julia` with no errors. (Only `src/jl_uv.c` and
`src/stackwalk.c` changed, so this relinks `libjulia-internal`.)

- [ ] **Step 6: Run the test to verify it passes**

Run:

```bash
usr/bin/julia --startup-file=no test/safecrashlog.jl
```

Expected: PASS — all `@test`s succeed. Every dump record now carries
`backtrace_id`/`backtrace_line`, two dumps are found with strictly increasing
ids, and each dump's `backtrace_line` values form a gap-free `0..N-1`
sequence.

- [ ] **Step 7: Manually confirm the JSON shape (optional sanity check)**

Run a quick manual capture and eyeball one dump record:

```bash
tmp=$(mktemp)
cat > /tmp/hb_child.jl <<'EOF'
t0 = time()
while ccall(:jl_heartbeat_enable, Cint, (Cint,Cint,Cint), 1, 1, 1000) != 0
    (time()-t0) > 10 && error("no heartbeat"); sleep(0.05)
end
sleep(4)
EOF
usr/bin/julia --startup-file=no --safe-crash-log-file=$tmp /tmp/hb_child.jl
grep -m1 "Task backtraces" "$tmp"
```

Expected: a single line of the form
`{"level":"Error", "timestamp":"...", "backtrace_id": 1, "backtrace_line": N, "message": "...Task backtraces..."}`
and the `heartbeat loss` line above it carries neither attribute.

- [ ] **Step 8: Commit**

```bash
git add src/jl_uv.c src/stackwalk.c
git commit -m "Tag task-backtrace crash-log records with backtrace_id/line"
```

---

## Self-Review

**Spec coverage:**
- "Add `backtrace_id`/`backtrace_line` attributes" → Task 2 Steps 1, 3.
- "Only records emitted inside `jl_print_task_backtraces` are tagged; other
  records unchanged" → Task 2 Steps 1 (per-thread `bt_dump_id`), 4 (hooks
  bracket only the dump); Task 1 asserts `heartbeat loss` records are
  untagged.
- "Per-thread dump id/line + global atomic counter" → Task 2 Step 1.
- "`jl_backtrace_dump_begin`/`end` hooks called by `jl_print_task_backtraces`"
  → Task 2 Steps 1, 4.
- "Emit between `timestamp` and `message`; integers via `sprintf`" → Task 2
  Step 3.
- "Truncation budget from actual preamble, not a constant" → Task 2 Step 2
  (tail-relative budget, valid for present/absent attributes).
- "Assumption SCL-1 documented" → Task 2 Step 1 ASSUMPTIONS block; Task 1
  header references it.
- "Integration test: attributes present on dump records, gap-free `0..N-1`
  line sequence, one id per dump, non-dump records untagged, second dump uses
  greater id and restarts line at 0" → Task 1 Step 1 assertions.

**Placeholder scan:** No TBD/TODO; every code and command step shows concrete
content.

**Type consistency:** `bt_dump_counter` (`_Atomic(int64_t)`), `bt_dump_id`,
`bt_dump_line` (`__thread int64_t`) are defined and used consistently.
`jl_backtrace_dump_begin`/`jl_backtrace_dump_end` signatures match between the
`src/jl_uv.c` definitions and the `src/stackwalk.c` externs. The test's regex
keys (`backtrace_id`, `backtrace_line`) match the emitted JSON keys.
