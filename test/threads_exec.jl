# This file is a part of Julia. License is MIT: https://julialang.org/license

using Test
using Base.Threads
using Base.Threads: SpinLock, threadpoolsize
using LinearAlgebra: peakflops

# for cfunction_closure
include("testenv.jl")

function killjob(d)
    Core.print(Core.stderr, d)
    if Sys.islinux()
        SIGINFO = 10
    elseif Sys.isbsd()
        SIGINFO = 29
    end
    if @isdefined(SIGINFO)
        ccall(:uv_kill, Cint, (Cint, Cint), getpid(), SIGINFO)
        sleep(5) # Allow time for profile to collect and print before killing
    end
    ccall(:uv_kill, Cint, (Cint, Cint), getpid(), Base.SIGTERM)
    nothing
end

# set up a watchdog alarm for 20 minutes
# so that we can attempt to get a "friendly" backtrace if something gets stuck
# (expected test duration is about 18-180 seconds)
Timer(t -> killjob("KILLING BY THREAD TEST WATCHDOG\n"), 1200)

@testset """threads_exec.jl with JULIA_NUM_THREADS == $(ENV["JULIA_NUM_THREADS"])""" begin

@test Threads.threadid() == 1
@test 1 <= threadpoolsize() <= Threads.maxthreadid()

# basic lock check
if threadpoolsize() > 1
    let lk = SpinLock()
        c1 = Base.Event()
        c2 = Base.Event()
        @test trylock(lk)
        @test !trylock(lk)
        t1 = Threads.@spawn (notify(c1); lock(lk); unlock(lk); trylock(lk))
        t2 = Threads.@spawn (notify(c2); trylock(lk))
        Libc.systemsleep(0.1) # block our thread from scheduling for a bit
        wait(c1)
        wait(c2)
        @test !fetch(t2)
        @test istaskdone(t2)
        @test !istaskdone(t1)
        unlock(lk)
        @test fetch(t1)
        @test istaskdone(t1)
    end
end

# threading constructs

let a = zeros(Int, 2 * threadpoolsize())
    @threads for i = 1:length(a)
        @sync begin
            @async begin
                @async (Libc.systemsleep(1); a[i] += 1)
                yield()
                a[i] += 1
            end
            @async begin
                yield()
                @async (Libc.systemsleep(1); a[i] += 1)
                a[i] += 1
            end
        end
    end
    @test all(isequal(4), a)
end

# parallel loop with parallel atomic addition
function threaded_loop(a, r, x)
    counter = Threads.Atomic{Int}(min(threadpoolsize(), length(r)))
    @threads for i in r
        # synchronize the start given that each partition is started sequentially,
        # meaning that without the wait, if the loop is too fast the iteration can happen in order
        if counter[] != 0
            Threads.atomic_sub!(counter, 1)
            spins = 0
            while counter[] != 0
                GC.safepoint()
                ccall(:jl_cpu_pause, Cvoid, ())
                spins += 1
                if spins > 500_000_000  # about 10 seconds
                    @warn "Failed wait for all workers. Unfinished rogue tasks occupying worker threads?"
                    break
                end
            end
        end
        j = i - firstindex(r) + 1
        a[j] = 1 + atomic_add!(x, 1)
    end
end

function test_threaded_loop_and_atomic_add()
    for r in [1:10000, collect(1:10000), Base.IdentityUnitRange(-500:500), (1,2,3,4,5,6,7,8,9,10)]
        n = length(r)
        x = Atomic()
        a = zeros(Int, n)
        threaded_loop(a,r,x)
        found = zeros(Bool,n)
        for i=1:length(a)
            found[a[i]] = true
        end
        @test x[] == n
        # Next test checks that all loop iterations ran,
        # and were unique (via pigeon-hole principle).
        @test !(false in found)
    end
end

test_threaded_loop_and_atomic_add()

# Helper for test_threaded_atomic_minmax that verifies sequential consistency.
function check_minmax_consistency(old::Array{T,1}, m::T, start::T, o::Base.Ordering) where T
    for v in old
        if v != start
            # Check that atomic op that installed v reported consistent old value.
            @test Base.lt(o, old[v-m+1], v)
        end
    end
end

function test_threaded_atomic_minmax(m::T,n::T) where T
    mid = m + (n-m)>>1
    x = Atomic{T}(mid)
    y = Atomic{T}(mid)
    oldx = Vector{T}(undef, n-m+1)
    oldy = Vector{T}(undef, n-m+1)
    @threads for i = m:n
        oldx[i-m+1] = atomic_min!(x, T(i))
        oldy[i-m+1] = atomic_max!(y, T(i))
    end
    @test x[] == m
    @test y[] == n
    check_minmax_consistency(oldy,m,mid,Base.Forward)
    check_minmax_consistency(oldx,m,mid,Base.Reverse)
end

# The ranges below verify that the correct signed/unsigned comparison is used.
test_threaded_atomic_minmax(Int16(-5000),Int16(5000))
test_threaded_atomic_minmax(UInt16(27000),UInt16(37000))

function threaded_add_locked(::Type{LockT}, x, n) where LockT
    critical = LockT()
    @threads for i = 1:n
        @test lock(critical) === nothing
        @test islocked(critical)
        x = x + 1
        @test unlock(critical) === nothing
    end
    @test !islocked(critical)
    nentered = 0
    nfailed = Atomic()
    @threads for i = 1:n
        if trylock(critical)
            @test islocked(critical)
            nentered += 1
            @test unlock(critical) === nothing
        else
            atomic_add!(nfailed, 1)
        end
    end
    @test 0 < nentered <= n
    @test nentered + nfailed[] == n
    @test !islocked(critical)
    return x
end

@test threaded_add_locked(SpinLock, 0, 10000) == 10000
@test threaded_add_locked(ReentrantLock, 0, 10000) == 10000

# Check if the recursive lock can be locked and unlocked correctly.
let critical = ReentrantLock()
    @test !islocked(critical)
    @test_throws ErrorException("unlock count must match lock count") unlock(critical)
    @test lock(critical) === nothing
    @test islocked(critical)
    @test lock(critical) === nothing
    @test trylock(critical) == true
    @test islocked(critical)
    @test unlock(critical) === nothing
    @test islocked(critical)
    @test unlock(critical) === nothing
    @test islocked(critical)
    @test unlock(critical) === nothing
    @test !islocked(critical)
    @test_throws ErrorException("unlock count must match lock count") unlock(critical)
    @test trylock(critical) == true
    @test islocked(critical)
    @test unlock(critical) === nothing
    @test !islocked(critical)
    @test_throws ErrorException("unlock count must match lock count") unlock(critical)
    @test !islocked(critical)
end

# Make sure doing a GC while holding a lock doesn't cause dead lock
# PR 14190. (This is only meaningful for threading)
function threaded_gc_locked(::Type{LockT}) where LockT
    critical = LockT()
    @threads for i = 1:20
        @test lock(critical) === nothing
        @test islocked(critical)
        GC.gc(false)
        @test unlock(critical) === nothing
    end
    @test !islocked(critical)
end

threaded_gc_locked(SpinLock)
threaded_gc_locked(ReentrantLock)

# Issue 33159
# Make sure that a Threads.Condition can't be used without being locked, on any thread.
@testset "Threads.Conditions must be locked" begin
    c = Threads.Condition()
    @test_throws Exception notify(c)
    @test_throws Exception wait(c)

    # If it's locked, but on the wrong thread, it should still throw an exception
    lock(c)
    @test_throws Exception fetch(@async notify(c))
    @test_throws Exception fetch(@async notify(c, all=false))
    @test_throws Exception fetch(@async wait(c))
    unlock(c)
end

# Issue 14726
# Make sure that eval'ing in a different module doesn't mess up other threads
orig_curmodule14726 = @__MODULE__
main_var14726 = 1
@eval Main module M14726
module_var14726 = 1
end

@threads for i in 1:100
    for j in 1:100
        @eval M14726 module_var14726 = $j
    end
end
@test @isdefined(orig_curmodule14726)
@test @isdefined(main_var14726)
@test @__MODULE__() == orig_curmodule14726

@threads for i in 1:100
    # Make sure current module is not null.
    # The @test might not be particularly meaningful currently since the
    # thread infrastructures swallows the error. (Same below)
    @test @__MODULE__() == orig_curmodule14726
end

@eval Main module M14726_2
using Test
using Base.Threads
@threads for i in 1:100
    # Make sure current module is the same as the one on the thread that
    # pushes the work onto the threads.
    # The @test might not be particularly meaningful currently since the
    # thread infrastructures swallows the error. (See also above)
    @test @__MODULE__() == M14726_2
end
end

# Ensure only LLVM-supported types can be atomic
@test_throws TypeError Atomic{BigInt}
@test_throws TypeError Atomic{ComplexF64}

if Sys.ARCH === :i686 || startswith(string(Sys.ARCH), "arm") ||
   Sys.ARCH === :powerpc64le || Sys.ARCH === :ppc64le

    @test_throws TypeError Atomic{Int128}()
    @test_throws TypeError Atomic{UInt128}()
end

if Sys.ARCH === :powerpc64le || Sys.ARCH === :ppc64le
    @test_throws TypeError Atomic{Float16}()
    @test_throws TypeError Atomic{Float32}()
    @test_throws TypeError Atomic{Float64}()
end

function test_atomic_bools()
    x = Atomic{Bool}(false)
    # Arithmetic functions are not defined.
    @test_throws MethodError atomic_add!(x, true)
    @test_throws MethodError atomic_sub!(x, true)
    # All the rest are:
    for v in [true, false]
        @test x[] == atomic_xchg!(x, v)
        @test v == atomic_cas!(x, v, !v)
    end
    x = Atomic{Bool}(false)
    @test false == atomic_max!(x, true); @test x[] == true
    x = Atomic{Bool}(true)
    @test true == atomic_and!(x, false); @test x[] == false
end

test_atomic_bools()

# Test atomic memory ordering with load/store
mutable struct CommBuf
    var1::Atomic{Int}
    var2::Atomic{Int}
    correct_write::Bool
    correct_read::Bool
    CommBuf() = new(Atomic{Int}(0), Atomic{Int}(0), false, false)
end
function test_atomic_write(commbuf::CommBuf, n::Int)
    for i in 1:n
        # The atomic stores guarantee that var1 >= var2
        commbuf.var1[] = i
        commbuf.var2[] = i
    end
    commbuf.correct_write = true
end
function test_atomic_read(commbuf::CommBuf, n::Int)
    correct = true
    while true
        # load var2 before var1
        var2 = commbuf.var2[]
        var1 = commbuf.var1[]
        correct &= var1 >= var2
        var1 == n && break
        # Temporary solution before we have gc transition support in codegen.
        ccall(:jl_gc_safepoint, Cvoid, ())
    end
    commbuf.correct_read = correct
end
function test_atomic()
    commbuf = CommBuf()
    count = 1_000_000
    @threads for i in 1:2
        if i==1
            test_atomic_write(commbuf, count)
        else
            test_atomic_read(commbuf, count)
        end
    end
    @test commbuf.correct_write == true
    @test commbuf.correct_read == true
end
test_atomic()

# Test ordering with fences using Peterson's algorithm
# Example adapted from <https://en.wikipedia.org/wiki/Peterson%27s_algorithm>
mutable struct Peterson
    # State for Peterson's algorithm
    flag::Vector{Atomic{Int}}
    turn::Atomic{Int}
    # Collision detection
    critical::Vector{Atomic{Int}}
    correct::Vector{Bool}
    Peterson() =
        new([Atomic{Int}(0), Atomic{Int}(0)],
            Atomic{Int}(0),
            [Atomic{Int}(0), Atomic{Int}(0)],
            [false, false])
end
function test_fence(p::Peterson, id::Int, n::Int)
    @assert id == mod1(id,2)
    correct = true
    otherid = mod1(id+1,2)
    for i in 1:n
        p.flag[id][] = 1
        p.turn[] = otherid
        atomic_fence()
        while p.flag[otherid][] != 0 && p.turn[] == otherid
            # busy wait
            # Temporary solution before we have gc transition support in codegen.
            ccall(:jl_gc_safepoint, Cvoid, ())
        end
        # critical section
        p.critical[id][] = 1
        correct &= p.critical[otherid][] == 0
        p.critical[id][] = 0
        # end of critical section
        p.flag[id][] = 0
    end
    p.correct[id] = correct
end
function test_fence()
    commbuf = Peterson()
    count = 1_000_000
    @threads for i in 1:2
        test_fence(commbuf, i, count)
    end
    @test commbuf.correct[1] == true
    @test commbuf.correct[2] == true
end
test_fence()

# Test load / store with various types
let atomictypes = intersect((Int8, Int16, Int32, Int64, Int128,
                             UInt8, UInt16, UInt32, UInt64, UInt128,
                             Float16, Float32, Float64),
                            Base.Threads.atomictypes)
    for T in atomictypes
        var = Atomic{T}()
        var[] = 42
        @test var[] === T(42)
        old = atomic_xchg!(var, T(13))
        @test old === T(42)
        @test var[] === T(13)
        old = atomic_cas!(var, T(13), T(14))   # this will succeed
        @test old === T(13)
        @test var[] === T(14)
        old = atomic_cas!(var, T(13), T(15))   # this will fail
        @test old === T(14)
        @test var[] === T(14)
    end
end

# Test atomic_cas! and atomic_xchg!
function test_atomic_cas!(var::Atomic{T}, range::StepRange{Int,Int}) where T
    for i in range
        while true
            old = atomic_cas!(var, T(i-1), T(i))
            old == T(i-1) && break
            # Temporary solution before we have gc transition support in codegen.
            ccall(:jl_gc_safepoint, Cvoid, ())
        end
    end
end
for T in intersect((Int32, Int64, Float32, Float64), Base.Threads.atomictypes)
    var = Atomic{T}()
    nloops = 1000
    di = threadpoolsize()
    @threads for i in 1:di
        test_atomic_cas!(var, i:di:nloops)
    end
    @test var[] === T(nloops)
end

function test_atomic_xchg!(var::Atomic{T}, i::Int, accum::Atomic{Int}) where T
    old = atomic_xchg!(var, T(i))
    atomic_add!(accum, Int(old))
end
for T in intersect((Int32, Int64, Float32, Float64), Base.Threads.atomictypes)
    accum = Atomic{Int}()
    var = Atomic{T}()
    nloops = 1000
    @threads for i in 1:nloops
        test_atomic_xchg!(var, i, accum)
    end
    @test accum[] + Int(var[]) === sum(0:nloops)
end

function test_atomic_float(varadd::Atomic{T}, varmax::Atomic{T}, varmin::Atomic{T}, i::Int) where T
    atomic_add!(varadd, T(i))
    atomic_max!(varmax, T(i))
    atomic_min!(varmin, T(i))
end
for T in intersect((Int32, Int64, Float16, Float32, Float64), Base.Threads.atomictypes)
    varadd = Atomic{T}()
    varmax = Atomic{T}()
    varmin = Atomic{T}()
    nloops = 1000
    @threads for i in 1:nloops
        test_atomic_float(varadd, varmax, varmin, i)
    end
    @test varadd[] === T(sum(1:nloops))
    @test varmax[] === T(maximum(1:nloops))
    @test varmin[] === T(0)
    @test atomic_add!(Atomic{T}(1), T(2)) == 1
    @test atomic_sub!(Atomic{T}(2), T(3)) == 2
    @test atomic_min!(Atomic{T}(4), T(3)) == 4
    @test atomic_max!(Atomic{T}(5), T(6)) == 5
end

using Dates
for period in (0.06, Dates.Millisecond(60))
    let async = Base.AsyncCondition(), t
        c = Condition()
        task = schedule(Task(function()
            notify(c)
            wait(c)
            t = Timer(period)
            wait(t)
            ccall(:uv_async_send, Cvoid, (Ptr{Cvoid},), async)
            ccall(:uv_async_send, Cvoid, (Ptr{Cvoid},), async)
            wait(c)
            sleep(period)
            ccall(:uv_async_send, Cvoid, (Ptr{Cvoid},), async)
            ccall(:uv_async_send, Cvoid, (Ptr{Cvoid},), async)
        end))
        wait(c)
        notify(c)
        delay1 = @elapsed wait(async)
        notify(c)
        delay2 = @elapsed wait(async)
        @test istaskdone(task)
        @test delay1 > 0.05
        @test delay2 > 0.05
        @test isopen(async)
        @test !isopen(t)
        close(t)
        close(async)
        @test_throws EOFError wait(async)
        @test !isopen(async)
        @test_throws EOFError wait(t)
        @test_throws EOFError wait(async)
    end
end

function test_thread_cfunction()
    # ensure a runtime call to `get_trampoline` will be created
    fs = [ Core.Box() for i in 1:1000 ]
    @noinline cf(f) = @cfunction $f Float64 ()
    cfs = Vector{Base.CFunction}(undef, length(fs))
    cf1 = cf(fs[1])
    @threads for i in 1:1000
        cfs[i] = cf(fs[i])
    end
    @test cfs[1] == cf1
    @test cfs[2] == cf(fs[2])
    @test length(unique(cfs)) == 1000
    ok = zeros(Int, threadpoolsize())
    @threads :static for i in 1:10000
        i = mod1(i, 1000)
        fi = fs[i]
        cfi = cf(fi)
        GC.@preserve cfi begin
            ok[threadid()] += (cfi === cfs[i])
        end
    end
    @test sum(ok) == 10000
end
if cfunction_closure
    test_thread_cfunction()
end

function test_thread_range()
    a = zeros(Int, threadpoolsize())
    @threads for i in 1:threadid()
        a[i] = 1
    end
    for i in 1:threadid()
        @test a[i] == 1
    end
    for i in (threadid() + 1):threadpoolsize()
        @test a[i] == 0
    end
end
test_thread_range()

# Thread safety of `jl_load_and_lookup`.
function test_load_and_lookup_18020(n)
    @threads for i in 1:n
        try
            ccall(:jl_load_and_lookup,
                  Ptr{Cvoid}, (Cstring, Cstring, Ref{Ptr{Cvoid}}),
                  "$i", :f, C_NULL)
        catch ex
            ex isa ErrorException || rethrow()
            startswith(ex.msg, "could not load library") || rethrow()
        end
    end
end
test_load_and_lookup_18020(10000)

# Nested threaded loops
# This may not be efficient/fully supported but should work without crashing.....
function test_nested_loops()
    a = zeros(Int, 100, 100)
    @threads for i in 1:100
        @threads for j in 1:100
            a[j, i] = i + j
        end
    end
    for i in 1:100
        for j in 1:100
            @test a[j, i] == i + j
        end
    end
end
test_nested_loops()

function test_thread_too_few_iters()
    x = Atomic()
    a = zeros(Int, threadpoolsize()+2)
    threaded_loop(a, 1:threadpoolsize()-1, x)
    found = zeros(Bool, threadpoolsize()+2)
    for i=1:threadpoolsize()-1
        found[a[i]] = true
    end
    @test x[] == threadpoolsize()-1
    # Next test checks that all loop iterations ran,
    # and were unique (via pigeon-hole principle).
    @test !(false in found[1:threadpoolsize()-1])
    @test !(true in found[threadpoolsize():end])
end
test_thread_too_few_iters()

@testset "IntrusiveLinkedList" begin
    @test eltype(Base.IntrusiveLinkedList{Integer}) == Integer
    @test eltype(Base.LinkedList{Integer}) == Integer
    @test eltype(Base.IntrusiveLinkedList{<:Integer}) == Any
    @test eltype(Base.LinkedList{<:Integer}) == Any
    @test eltype(Base.IntrusiveLinkedList{<:Base.LinkedListItem{Integer}}) == Any

    t = Base.LinkedList{Integer}()
    @test eltype(t) == Integer
    @test isempty(t)
    @test length(t) == 0
    @test isempty(collect(t)::Vector{Integer})
    @test pushfirst!(t, 2) === t
    @test !isempty(t)
    @test length(t) == 1
    @test pushfirst!(t, 1) === t
    @test !isempty(t)
    @test length(t) == 2
    @test collect(t) == [1, 2]
    @test pop!(t) == 2
    @test !isempty(t)
    @test length(t) == 1
    @test collect(t) == [1]
    @test pop!(t) == 1
    @test isempty(t)
    @test length(t) == 0
    @test collect(t) == []

    @test push!(t, 1) === t
    @test !isempty(t)
    @test length(t) == 1
    @test push!(t, 2) === t
    @test !isempty(t)
    @test length(t) == 2
    @test collect(t) == [1, 2]
    @test popfirst!(t) == 1
    @test popfirst!(t) == 2
    @test isempty(collect(t)::Vector{Integer})

    @test push!(t, 5) === t
    @test push!(t, 6) === t
    @test push!(t, 7) === t
    @test length(t) === 3
    @test Base.list_deletefirst!(t, 1) === t
    @test length(t) === 3
    @test Base.list_deletefirst!(t, 6) === t
    @test length(t) === 2
    @test collect(t) == [5, 7]
    @test Base.list_deletefirst!(t, 6) === t
    @test length(t) === 2
    @test Base.list_deletefirst!(t, 7) === t
    @test length(t) === 1
    @test collect(t) == [5]
    @test Base.list_deletefirst!(t, 5) === t
    @test length(t) === 0
    @test collect(t) == []
    @test isempty(t)

    t2 = Base.LinkedList{Integer}()
    @test push!(t, 5) === t
    @test push!(t, 6) === t
    @test push!(t, 7) === t
    @test push!(t2, 2) === t2
    @test push!(t2, 3) === t2
    @test push!(t2, 4) === t2
    @test Base.list_append!!(t, t2) === t
    @test isempty(t2)
    @test isempty(collect(t2)::Vector{Integer})
    @test collect(t) == [5, 6, 7, 2, 3, 4]
    @test Base.list_append!!(t, t2) === t
    @test collect(t) == [5, 6, 7, 2, 3, 4]
    @test Base.list_append!!(t2, t) === t2
    @test isempty(t)
    @test collect(t2) == [5, 6, 7, 2, 3, 4]
    @test push!(t, 1) === t
    @test collect(t) == [1]
    @test Base.list_append!!(t2, t) === t2
    @test isempty(t)
    @test collect(t2) == [5, 6, 7, 2, 3, 4, 1]
end

let t = Timer(identity, 0.025, interval=0.025)
    out = stdout
    rd, wr = redirect_stdout()
    @async while isopen(rd)
        readline(rd)
    end
    try
        for i in 1:10000
            Threads.@threads for j in 1:1000
            end
            @show i
        end
    finally
        redirect_stdout(out)
        close(t)
    end
end

# shared workqueue

function pfib(n::Int)
    if n <= 1
        return n
    end
    t = Threads.@spawn pfib(n-2)
    return pfib(n-1) + fetch(t)::Int
end
@test pfib(20) == 6765


# scheduling wake/sleep test (#32511)
let t = Timer(t -> killjob("KILLING BY QUICK KILL WATCHDOG\n"), 600) # this test should take about 1-10 seconds
    for _ = 1:10^5
        @threads for idx in 1:1024; #=nothing=# end
    end
    close(t) # stop the fast watchdog
end

# issue #32575
let ch = Channel{Char}(0), t
    t = Task(()->for v in "hello" put!(ch, v) end)
    t.sticky = false
    bind(ch, t)
    schedule(t)
    @test String(collect(ch)) == "hello"
end

# errors inside @threads :static
function _atthreads_with_error(a, err)
    Threads.@threads :static for i in eachindex(a)
        if err
            error("failed")
        end
        a[i] = Threads.threadid()
    end
    a
end
@test_throws CompositeException _atthreads_with_error(zeros(threadpoolsize()), true)
let a = zeros(threadpoolsize())
    _atthreads_with_error(a, false)
    @test a == [1:threadpoolsize();]
end

# static schedule
function _atthreads_static_schedule(n)
    ids = zeros(Int, n)
    Threads.@threads :static for i = 1:n
        ids[i] = Threads.threadid()
    end
    return ids
end
@test _atthreads_static_schedule(threadpoolsize()) == 1:threadpoolsize()
@test _atthreads_static_schedule(1) == [1;]
@test_throws(
    "`@threads :static` cannot be used concurrently or nested",
    @threads(for i = 1:1; _atthreads_static_schedule(threadpoolsize()); end),
)

# dynamic schedule
function _atthreads_dynamic_schedule(n)
    inc = Threads.Atomic{Int}(0)
    flags = zeros(Int, n)
    Threads.@threads :dynamic for i = 1:n
        Threads.atomic_add!(inc, 1)
        flags[i] = 1
    end
    return inc[], flags
end
@test _atthreads_dynamic_schedule(threadpoolsize()) == (threadpoolsize(), ones(threadpoolsize()))
@test _atthreads_dynamic_schedule(1) == (1, ones(1))
@test _atthreads_dynamic_schedule(10) == (10, ones(10))
@test _atthreads_dynamic_schedule(threadpoolsize() * 2) == (threadpoolsize() * 2, ones(threadpoolsize() * 2))

# nested dynamic schedule
function _atthreads_dynamic_dynamic_schedule()
    inc = Threads.Atomic{Int}(0)
    Threads.@threads :dynamic for _ = 1:threadpoolsize()
        Threads.@threads :dynamic for _ = 1:threadpoolsize()
            Threads.atomic_add!(inc, 1)
        end
    end
    return inc[]
end
@test _atthreads_dynamic_dynamic_schedule() == threadpoolsize() * threadpoolsize()

function _atthreads_static_dynamic_schedule()
    ids = zeros(Int, threadpoolsize())
    inc = Threads.Atomic{Int}(0)
    Threads.@threads :static for i = 1:threadpoolsize()
        ids[i] = Threads.threadid()
        Threads.@threads :dynamic for _ = 1:threadpoolsize()
            Threads.atomic_add!(inc, 1)
        end
    end
    return ids, inc[]
end
@test _atthreads_static_dynamic_schedule() == (1:threadpoolsize(), threadpoolsize() * threadpoolsize())

# errors inside @threads :dynamic
function _atthreads_dynamic_with_error(a)
    Threads.@threads :dynamic for i in eachindex(a)
        error("user error in the loop body")
    end
    a
end
@test_throws "user error in the loop body" _atthreads_dynamic_with_error(zeros(threadpoolsize()))

try
    @macroexpand @threads(for i = 1:10, j = 1:10; end)
catch ex
    @test ex isa ArgumentError
end

@testset "@spawn interpolation" begin
    # Issue #30896: evaluating arguments immediately
    begin
        outs = zeros(5)
        # Use interpolation to fill outs with the values of `i`
        @sync begin
            local i = 1
            while i <= 5
                Threads.@spawn setindex!(outs, $i, $i)
                i += 1
            end
        end
        @test outs == 1:5
    end

    # Test macro parsing for interpolating into Args
    @test fetch(Threads.@spawn 2+$2) == 4
    @test fetch(Threads.@spawn Int($(2.0))) == 2
    a = 2
    @test fetch(Threads.@spawn *($a,$a)) == a^2
    # Test macro parsing for interpolating into kwargs
    @test fetch(Threads.@spawn sort($([3 2; 1 0]), dims=2)) == [2 3; 0 1]
    @test fetch(Threads.@spawn sort([3 $2; 1 $0]; dims=$2)) == [2 3; 0 1]

    # Test macro parsing supports multiple levels of interpolation
    @testset "spawn macro multiple levels of interpolation" begin
        # Use `ch` to synchronize within the tests to run after the local variables are
        # updated, showcasing the problem and the solution.
        ch = Channel()   # (This synchronization fixes test failure reported in #34141.)

        @test fetch(Threads.@spawn "$($a)") == "$a"
        let a = 1
            # Interpolate the current value of `a` vs the value of `a` in the closure
            t = Threads.@spawn (take!(ch); :(+($$a, $a, a)))
            a = 2  # update `a` after spawning, before `t` runs
            put!(ch, nothing)  # now run t
            @test fetch(t) == Expr(:call, :+, 1, 2, :a)
        end

        # Test the difference between different levels of interpolation
        # Without interpolation, each spawned task sees the last value of `i` (6);
        # with interpolation, each spawned task has the value of `i` at time of `@spawn`.
        let
            oneinterp  = Vector{Any}(undef, 5)
            twointerps = Vector{Any}(undef, 5)
            @sync begin
               local i = 1
               while i <= 5
                   Threads.@spawn (take!(ch); setindex!(oneinterp, :($i), $i))
                   Threads.@spawn (take!(ch); setindex!(twointerps, :($($i)), $i))
                   i += 1
               end
               for _ in 1:10; put!(ch, nothing); end # Now run all the tasks.
            end
            # The first definition _didn't_ interpolate i
            @test oneinterp == fill(6, 5)
            # The second definition _did_ interpolate i
            @test twointerps == 1:5
        end
    end
end

@testset "@async interpolation" begin
    # Args
    @test fetch(@async 2+$2) == 4
    @test fetch(@async Int($(2.0))) == 2
    a = 2
    @test fetch(@async *($a,$a)) == a^2
    # kwargs
    @test fetch(@async sort($([3 2; 1 0]), dims=2)) == [2 3; 0 1]
    @test fetch(@async sort([3 $2; 1 $0]; dims=$2)) == [2 3; 0 1]

    # Supports multiple levels of interpolation
    @test fetch(@async :($a)) == a
    @test fetch(@async :($($a))) == a
    @test fetch(@async "$($a)") == "$a"
end

# Issue #34138
@testset "spawn interpolation: macrocalls" begin
    x = [reshape(1:4, 2, 2);]
    @test fetch(Threads.@spawn @. $exp(x)) == @. $exp(x)
    x = 2
    @test @eval(fetch(@async 2+$x)) == 4
end

# issue #34666
fib34666(x) =
    @sync begin
        function f(x)
            x in (0, 1) && return x
            a = Threads.@spawn f(x - 2)
            b = Threads.@spawn f(x - 1)
            return fetch(a) + fetch(b)
        end
        f(x)
    end
@test fib34666(25) == 75025

# issue #41324
@testset "Co-schedule" begin
    parent = Threads.@spawn begin
        @test current_task().sticky == false
        child = @async begin end
        @test current_task().sticky == true
        @test Threads.threadid() == Threads.threadid(child)
        wait(child)
    end
    wait(parent)
    @test parent.sticky == true
end

function jitter_channel(f, k, delay, ntasks, schedule)
    x = Channel(ch -> foreach(i -> put!(ch, i), 1:k), 1)
    y = Channel(k) do ch
        g = i -> begin
            iseven(i) && sleep(delay)
            put!(ch, f(i))
        end
        Threads.foreach(g, x; schedule=schedule, ntasks=ntasks)
    end
    return y
end

@testset "Threads.foreach(f, ::Channel)" begin
    k = 50
    delay = 0.01
    expected = sin.(1:k)
    ordered_fair = collect(jitter_channel(sin, k, delay, 1, Threads.FairSchedule()))
    ordered_static = collect(jitter_channel(sin, k, delay, 1, Threads.StaticSchedule()))
    @test expected == ordered_fair
    @test expected == ordered_static

    unordered_fair = collect(jitter_channel(sin, k, delay, 10, Threads.FairSchedule()))
    unordered_static = collect(jitter_channel(sin, k, delay, 10, Threads.StaticSchedule()))
    @test expected != unordered_fair
    @test expected != unordered_static
    @test Set(expected) == Set(unordered_fair)
    @test Set(expected) == Set(unordered_static)

    ys = Channel() do ys
        inner = Channel(xs -> foreach(i -> put!(xs, i), 1:3))
        Threads.foreach(x -> put!(ys, x), inner)
    end
    @test sort!(collect(ys)) == 1:3
end

# reproducible multi-threaded rand()

using Random

function reproducible_rand(r, i)
    if i == 0
        return UInt64(0)
    end
    r1 = rand(r, UInt64)*hash(i)
    t1 = Threads.@spawn reproducible_rand(r, i-1)
    t2 = Threads.@spawn reproducible_rand(r, i-1)
    r2 = rand(r, UInt64)
    return r1 + r2 + fetch(t1) + fetch(t2)
end

@testset "Task-local random" begin
    r = Random.TaskLocalRNG()
    Random.seed!(r, 23)
    val = reproducible_rand(r, 10)
    for i = 1:4
        Random.seed!(r, 23)
        @test reproducible_rand(r, 10) == val
    end
end

# @spawn racying with sync_end

hidden_spawn(f) = Threads.@spawn f()

function sync_end_race()
    y = Ref(:notset)
    local t
    @sync begin
        for _ in 1:6  # tweaked to maximize `nerror` below
            Threads.@spawn nothing
        end
        t = hidden_spawn() do
            Threads.@spawn y[] = :completed
        end
    end
    try
        wait(t)
    catch
        return :notscheduled
    end
    return y[]
end

function check_sync_end_race()
    @sync begin
        done = Threads.Atomic{Bool}(false)
        try
            # `Threads.@spawn` must fail to be scheduled or complete its execution:
            ncompleted = 0
            nnotscheduled = 0
            nerror = 0
            for i in 1:1000
                y = try
                    yield()
                    sync_end_race()
                catch err
                    if err isa CompositeException
                        if err.exceptions[1] isa Base.ScheduledAfterSyncException
                            nerror += 1
                            continue
                        end
                    end
                    rethrow()
                end
                y in (:completed, :notscheduled) || return (; i, y)
                ncompleted += y === :completed
                nnotscheduled += y === :notscheduled
            end
            # Useful for tuning the test:
            @debug "`check_sync_end_race` done" threadpoolsize() ncompleted nnotscheduled nerror
        finally
            done[] = true
        end
    end
    return nothing
end

@testset "Racy `@spawn`" begin
    @test check_sync_end_race() === nothing
end

# issue #41546, thread-safe package loading
@testset "package loading" begin
    ch = Channel{Bool}(threadpoolsize())
    barrier = Base.Event()
    old_act_proj = Base.ACTIVE_PROJECT[]
    try
        pushfirst!(LOAD_PATH, "@")
        Base.ACTIVE_PROJECT[] = joinpath(@__DIR__, "TestPkg")
        @sync begin
            for _ in 1:threadpoolsize()
                Threads.@spawn begin
                    put!(ch, true)
                    wait(barrier)
                    @eval using TestPkg
                end
            end
            for _ in 1:threadpoolsize()
                take!(ch)
            end
            notify(barrier)
        end
        @test Base.root_module(@__MODULE__, :TestPkg) isa Module
    finally
        Base.ACTIVE_PROJECT[] = old_act_proj
        popfirst!(LOAD_PATH)
    end
end

# issue #49746, thread safety in `atexit(f)`
@testset "atexit thread safety" begin
    f = () -> nothing
    before_len = length(Base.atexit_hooks)
    @sync begin
        for _ in 1:1_000_000
            Threads.@spawn begin
                atexit(f)
            end
        end
    end
    @test length(Base.atexit_hooks) == before_len + 1_000_000
    @test all(hook -> hook === f, Base.atexit_hooks[1 : 1_000_000])

    # cleanup
    Base.@lock Base._atexit_hooks_lock begin
        deleteat!(Base.atexit_hooks, 1:1_000_000)
    end
end

@testset "Base.Experimental.task_metrics" begin
    t = Task(() -> nothing)
    @test_throws "const field" t.metrics_enabled = true
    is_task_metrics_enabled() = fetch(Threads.@spawn current_task().metrics_enabled)
    @test !is_task_metrics_enabled()
    try
        @testset "once" begin
            Base.Experimental.task_metrics(true)
            @test is_task_metrics_enabled()
            Base.Experimental.task_metrics(false)
            @test !is_task_metrics_enabled()
        end
        @testset "multiple" begin
            Base.Experimental.task_metrics(true)  # 1
            Base.Experimental.task_metrics(true)  # 2
            Base.Experimental.task_metrics(true)  # 3
            @test is_task_metrics_enabled()
            Base.Experimental.task_metrics(false) # 2
            @test is_task_metrics_enabled()
            Base.Experimental.task_metrics(false) # 1
            @test is_task_metrics_enabled()
            @sync for i in 1:5                    # 0 (not negative)
                Threads.@spawn Base.Experimental.task_metrics(false)
            end
            @test !is_task_metrics_enabled()
            Base.Experimental.task_metrics(true)  # 1
            @test is_task_metrics_enabled()
        end
    finally
        while is_task_metrics_enabled()
            Base.Experimental.task_metrics(false)
        end
    end
end

@testset "task time counters" begin
    @testset "enabled" begin
        try
            Base.Experimental.task_metrics(true)
            start_time = time_ns()
            t = Threads.@spawn peakflops()
            wait(t)
            end_time = time_ns()
            wall_time_delta = end_time - start_time
            @test t.metrics_enabled
            @test Base.Experimental.task_running_time_ns(t) > 0
            @test Base.Experimental.task_wall_time_ns(t) > 0
            @test Base.Experimental.task_wall_time_ns(t) >= Base.Experimental.task_running_time_ns(t)
            @test wall_time_delta > Base.Experimental.task_wall_time_ns(t)
        finally
            Base.Experimental.task_metrics(false)
        end
    end
    @testset "disabled" begin
        t = Threads.@spawn peakflops()
        wait(t)
        @test !t.metrics_enabled
        @test isnothing(Base.Experimental.task_running_time_ns(t))
        @test isnothing(Base.Experimental.task_wall_time_ns(t))
    end
    @testset "task not run" begin
        t1 = Task(() -> nothing)
        @test !t1.metrics_enabled
        @test isnothing(Base.Experimental.task_running_time_ns(t1))
        @test isnothing(Base.Experimental.task_wall_time_ns(t1))
        try
            Base.Experimental.task_metrics(true)
            t2 = Task(() -> nothing)
            @test t2.metrics_enabled
            @test Base.Experimental.task_running_time_ns(t2) == 0
            @test Base.Experimental.task_wall_time_ns(t2) == 0
        finally
            Base.Experimental.task_metrics(false)
        end
    end
    @testset "task failure" begin
        try
            Base.Experimental.task_metrics(true)
            t = Threads.@spawn error("this task failed")
            @test_throws "this task failed" wait(t)
            @test Base.Experimental.task_running_time_ns(t) > 0
            @test Base.Experimental.task_wall_time_ns(t) > 0
            @test Base.Experimental.task_wall_time_ns(t) >= Base.Experimental.task_running_time_ns(t)
        finally
            Base.Experimental.task_metrics(false)
        end
    end
    @testset "direct yield(t)" begin
        try
            Base.Experimental.task_metrics(true)
            start = time_ns()
            t_outer = Threads.@spawn begin
                t_inner = Task(() -> peakflops())
                t_inner.sticky = false
                # directly yield to `t_inner` rather calling `schedule(t_inner)`
                yield(t_inner)
                wait(t_inner)
                @test Base.Experimental.task_running_time_ns(t_inner) > 0
                @test Base.Experimental.task_wall_time_ns(t_inner) > 0
                @test Base.Experimental.task_wall_time_ns(t_inner) >= Base.Experimental.task_running_time_ns(t_inner)
            end
            wait(t_outer)
            delta = time_ns() - start
            @test Base.Experimental.task_running_time_ns(t_outer) > 0
            @test Base.Experimental.task_wall_time_ns(t_outer) > 0
            @test Base.Experimental.task_wall_time_ns(t_outer) >= Base.Experimental.task_running_time_ns(t_outer)
            @test Base.Experimental.task_wall_time_ns(t_outer) < delta
        finally
            Base.Experimental.task_metrics(false)
        end
    end
    @testset "bad schedule" begin
        try
            Base.Experimental.task_metrics(true)
            t1 = Task((x) -> 1)
            schedule(t1) # MethodError
            yield()
            @assert istaskfailed(t1)
            @test Base.Experimental.task_running_time_ns(t1) > 0
            @test Base.Experimental.task_wall_time_ns(t1) > 0
            foo(a, b) = a + b
            t2 = Task(() -> (peakflops(); foo(wait())))
            schedule(t2)
            yield()
            @assert istaskstarted(t1) && !istaskdone(t2)
            schedule(t2, 1)
            yield()
            @assert istaskfailed(t2)
            @test Base.Experimental.task_running_time_ns(t2) > 0
            @test Base.Experimental.task_wall_time_ns(t2) > 0
        finally
            Base.Experimental.task_metrics(false)
        end
    end
    @testset "continuously update until task done" begin
        try
            Base.Experimental.task_metrics(true)
            last_running_time = Ref(typemax(Int))
            last_wall_time = Ref(typemax(Int))
            t = Threads.@spawn begin
                running_time = Base.Experimental.task_running_time_ns()
                wall_time = Base.Experimental.task_wall_time_ns()
                for _ in 1:5
                    x = time_ns()
                    while time_ns() < x + 100
                    end
                    new_running_time = Base.Experimental.task_running_time_ns()
                    new_wall_time = Base.Experimental.task_wall_time_ns()
                    @test new_running_time > running_time
                    @test new_wall_time > wall_time
                    running_time = new_running_time
                    wall_time = new_wall_time
                end
                last_running_time[] = running_time
                last_wall_time[] = wall_time
            end
            wait(t)
            final_running_time = Base.Experimental.task_running_time_ns(t)
            final_wall_time = Base.Experimental.task_wall_time_ns(t)
            @test last_running_time[] < final_running_time
            @test last_wall_time[] < final_wall_time
            # ensure many more tasks are run to make sure the counters are
            # not being updated after a task is done e.g. only when a new task is found
            @sync for _ in 1:Threads.nthreads()
                Threads.@spawn rand()
            end
            @test final_running_time == Base.Experimental.task_running_time_ns(t)
            @test final_wall_time == Base.Experimental.task_wall_time_ns(t)
        finally
            Base.Experimental.task_metrics(false)
        end
    end
end

@testset "task time counters: lots of spawns" begin
    using Dates
    try
        Base.Experimental.task_metrics(true)
        # create more tasks than we have threads.
        # - all tasks must have: cpu time <= wall time
        # - some tasks must have: cpu time < wall time
        # - summing across all tasks we must have: total cpu time <= available cpu time
        n_tasks = 2 * Threads.nthreads(:default)
        cpu_times = Vector{UInt64}(undef, n_tasks)
        wall_times = Vector{UInt64}(undef, n_tasks)
        start_time = time_ns()
        @sync begin
            for i in 1:n_tasks
                start_time_i = time_ns()
                task_i = Threads.@spawn peakflops()
                Threads.@spawn begin
                    wait(task_i)
                    end_time_i = time_ns()
                    wall_time_delta_i = end_time_i - start_time_i
                    cpu_times[$i] = cpu_time_i = Base.Experimental.task_running_time_ns(task_i)
                    wall_times[$i] = wall_time_i = Base.Experimental.task_wall_time_ns(task_i)
                    # task should have recorded some cpu-time and some wall-time
                    @test cpu_time_i > 0
                    @test wall_time_i > 0
                    # task cpu-time cannot be greater than its wall-time
                    @test wall_time_i >= cpu_time_i
                    # task wall-time must be less than our manually measured wall-time
                    # between calling `@spawn` and returning from `wait`.
                    @test wall_time_delta_i > wall_time_i
                end
            end
        end
        end_time = time_ns()
        wall_time_delta = (end_time - start_time)
        available_cpu_time = wall_time_delta * Threads.nthreads(:default)
        summed_cpu_time = sum(cpu_times)
        # total CPU time from all tasks can't exceed what was actually available.
        @test available_cpu_time > summed_cpu_time
        # some tasks must have cpu-time less than their wall-time, because we had more tasks
        # than threads.
        summed_wall_time = sum(wall_times)
        @test summed_wall_time > summed_cpu_time
    finally
        Base.Experimental.task_metrics(false)
    end
end

@testset "--timeout-for-safepoint-straggler command-line flag" begin
    program = "
        function main()
            t = Threads.@spawn begin
                ccall(:uv_sleep, Cvoid, (Cuint,), 20_000)
            end
            # Force a GC
            ccall(:uv_sleep, Cvoid, (Cuint,), 1_000)
            GC.gc()
            wait(t)
        end
        main()
    "
    for timeout in ("1", "4", "16")
        tmp_output_filename = tempname()
        tmp_output_file = open(tmp_output_filename, "w")
        if isnothing(tmp_output_file)
            error("Failed to open file $tmp_output_filename")
        end
        run(pipeline(`$(Base.julia_cmd()) --threads=4 --timeout-for-safepoint-straggler=$(timeout) -e $program`, stderr=tmp_output_file))
        # Check whether we printed the straggler's backtrace
        @test !isempty(read(tmp_output_filename, String))
        close(tmp_output_file)
        rm(tmp_output_filename)
    end
end

end # main testset
