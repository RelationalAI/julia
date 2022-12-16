# This file is a part of Julia. License is MIT: https://julialang.org/license

"""
Support for sockets. Provides [`IPAddr`](@ref) and subtypes, [`TCPSocket`](@ref), and [`UDPSocket`](@ref).
"""
module Sockets

export
    accept,
    bind,
    connect,
    getaddrinfo,
    getalladdrinfo,
    getnameinfo,
    getipaddr,
    getipaddrs,
    islinklocaladdr,
    getpeername,
    getsockname,
    listen,
    listenany,
    recv,
    recvfrom,
    send,
    join_multicast_group,
    leave_multicast_group,
    TCPSocket,
    UDPSocket,
    @ip_str,
    IPAddr,
    IPv4,
    IPv6

import Base
import Base: isless, show, print, parse, bind, convert, isreadable, iswritable, alloc_buf_hook, _uv_hook_close

using Base
using Base: LibuvStream, LibuvServer, PipeEndpoint, @handle_as, uv_error, associate_julia_struct, uvfinalize,
    notify_error, uv_req_data, uv_req_set_data, preserve_handle, unpreserve_handle, _UVError, IOError,
    eventloop, StatusUninit, StatusInit, StatusConnecting, StatusOpen, StatusClosing, StatusClosed, StatusActive,
    preserve_handle, unpreserve_handle, iolock_begin, iolock_end,
    uv_status_string, check_open, OS_HANDLE, RawFD,
    UV_EINVAL, UV_ENOMEM, UV_ENOBUFS, UV_EAGAIN, UV_ECONNABORTED, UV_EADDRINUSE, UV_EACCES, UV_EADDRNOTAVAIL,
    UV_EAI_ADDRFAMILY, UV_EAI_AGAIN, UV_EAI_BADFLAGS,
    UV_EAI_BADHINTS, UV_EAI_CANCELED, UV_EAI_FAIL,
    UV_EAI_FAMILY, UV_EAI_NODATA, UV_EAI_NONAME,
    UV_EAI_OVERFLOW, UV_EAI_PROTOCOL, UV_EAI_SERVICE,
    UV_EAI_SOCKTYPE, UV_EAI_MEMORY, StatusEOF, StatusPaused

include("IPAddr.jl")
include("addrinfo.jl")

"""
    TCPSocket(; delay=true)

Open a TCP socket using libuv. If `delay` is true, libuv delays creation of the
socket's file descriptor till the first [`bind`](@ref) call. `TCPSocket` has various
fields to denote the state of the socket as well as its send/receive buffers.
"""
mutable struct TCPSocket <: LibuvStream
    handle::Ptr{Cvoid}
    status::Int
    buffer::IOBuffer
    cond::Base.ThreadSynchronizer
    readerror::Any
    sendbuf::Union{IOBuffer, Nothing}
    lock::ReentrantLock # advisory lock
    throttle::Int
    socklock::Ptr{Cvoid}

    function TCPSocket(handle::Ptr{Cvoid}, status)
        tcp = new(
                handle,
                status,
                PipeBuffer(),
                Base.ThreadSynchronizer(),
                nothing,
                nothing,
                ReentrantLock(),
                Base.DEFAULT_READ_BUFFER_SZ,
                Ptr{Nothing}())
        associate_julia_struct(tcp.handle, tcp)
        finalizer(uvfinalize, tcp)
        return tcp
    end
end

function Base.iolock_begin(s::TCPSocket)
    ccall(:jl_socklock_begin, Cvoid, (Ptr{Cvoid},), s.socklock)
    iolock_begin()
end

function Base.iolock_end(s::TCPSocket)
    iolock_end()
    ccall(:jl_socklock_end, Cvoid, (Ptr{Cvoid},), s.socklock)
end

function init_socklock(socklock::Ptr{Cvoid})
    ccall(:jl_init_socklock, Cvoid, (Ptr{Cvoid},), socklock)
end

function sizeof_socklock()
   return  ccall(:jl_sizeof_socklock, Int32, ())
end

# kw arg "delay": if true, libuv delays creation of the socket fd till the first bind call
function TCPSocket(; delay=true)
    tcp = TCPSocket(Libc.malloc(Base._sizeof_uv_tcp), StatusUninit)
    af_spec = delay ? 0 : 2   # AF_UNSPEC is 0, AF_INET is 2

    socklock = Libc.malloc(sizeof_socklock())
    init_socklock(socklock)
    tcp.socklock = socklock

    iolock_begin(tcp)
    err = ccall(:uv_tcp_init_ex, Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cuint),
                eventloop(), tcp.handle, af_spec)
    uv_error("failed to create tcp socket", err)
    tcp.status = StatusInit
    iolock_end(tcp)
    return tcp
end

function TCPSocket(fd::OS_HANDLE)
    tcp = TCPSocket()
    iolock_begin(tcp)
    err = ccall(:uv_tcp_open, Int32, (Ptr{Cvoid}, OS_HANDLE), tcp.handle, fd)
    uv_error("tcp_open", err)
    tcp.status = StatusOpen
    iolock_end(tcp)
    return tcp
end
if OS_HANDLE != RawFD
    TCPSocket(fd::RawFD) = TCPSocket(Libc._get_osfhandle(fd))
end


mutable struct TCPServer <: LibuvServer
    handle::Ptr{Cvoid}
    status::Int
    cond::Base.ThreadSynchronizer

    function TCPServer(handle::Ptr{Cvoid}, status)
        tcp = new(
            handle,
            status,
            Base.ThreadSynchronizer())
        associate_julia_struct(tcp.handle, tcp)
        finalizer(uvfinalize, tcp)
        return tcp
    end
end

# Keyword arg "delay": if true, libuv delays creation of socket fd till bind.
# It can be set to false if there is a need to set socket options before
# further calls to `bind` and `listen`, e.g. `SO_REUSEPORT`.
function TCPServer(; delay=true)
    tcp = TCPServer(Libc.malloc(Base._sizeof_uv_tcp), StatusUninit)
    af_spec = delay ? 0 : 2   # AF_UNSPEC is 0, AF_INET is 2
    iolock_begin()
    err = ccall(:uv_tcp_init_ex, Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cuint),
                eventloop(), tcp.handle, af_spec)
    uv_error("failed to create tcp server", err)
    tcp.status = StatusInit
    iolock_end()
    return tcp
end

"""
    accept(server[, client])

Accepts a connection on the given server and returns a connection to the client. An
uninitialized client stream may be provided, in which case it will be used instead of
creating a new stream.
"""
accept(server::TCPServer) = accept(server, TCPSocket())

function accept(callback, server::LibuvServer)
    task = @async try
            while true
                client = accept(server)
                callback(client)
            end
        catch ex
            # accept below may explicitly throw UV_ECONNABORTED:
            # filter that out since we expect that error
            if !(ex isa IOError && ex.code == UV_ECONNABORTED) || isopen(server)
                rethrow()
            end
        end
    return task # caller is responsible for checking for errors
end


# UDP
"""
    UDPSocket()

Open a UDP socket using libuv. `UDPSocket` has various
fields to denote the state of the socket.
"""
mutable struct UDPSocket <: LibuvStream
    handle::Ptr{Cvoid}
    status::Int
    recvnotify::Base.ThreadSynchronizer
    cond::Base.ThreadSynchronizer

    function UDPSocket(handle::Ptr{Cvoid}, status)
        cond = Base.ThreadSynchronizer()
        udp = new(handle, status, Base.ThreadSynchronizer(cond.lock), cond)
        associate_julia_struct(udp.handle, udp)
        finalizer(uvfinalize, udp)
        return udp
    end
end
function UDPSocket()
    this = UDPSocket(Libc.malloc(Base._sizeof_uv_udp), StatusUninit)
    iolock_begin()
    err = ccall(:uv_udp_init, Cint, (Ptr{Cvoid}, Ptr{Cvoid}),
                eventloop(), this.handle)
    uv_error("failed to create udp socket", err)
    this.status = StatusInit
    iolock_end()
    return this
end

show(io::IO, stream::UDPSocket) = print(io, typeof(stream), "(", uv_status_string(stream), ")")

function _uv_hook_close(sock::UDPSocket)
    sock.handle = C_NULL
    lock(sock.cond)
    try
        sock.status = StatusClosed
        notify(sock.cond)
        notify_error(sock.recvnotify, EOFError())
    finally
        unlock(sock.cond)
    end
    nothing
end

# Disables dual stack mode.
const UV_TCP_IPV6ONLY = 1

# Disables dual stack mode. Only available when using ipv6 binf
const UV_UDP_IPV6ONLY = 1

# Indicates message was truncated because read buffer was too small. The
# remainder was discarded by the OS.
const UV_UDP_PARTIAL = 2

# Indicates if SO_REUSEADDR will be set when binding the handle in uv_udp_bind. This sets
# the SO_REUSEPORT socket flag on the BSDs and OS X. On other Unix platforms, it sets the
# SO_REUSEADDR flag. What that means is that multiple threads or processes can bind to the
# same address without error (provided they all set the flag) but only the last one to bind
# will receive any traffic, in effect "stealing" the port from the previous listener.
const UV_UDP_REUSEADDR = 4

##

function _bind(sock::Union{TCPServer, TCPSocket}, host::Union{IPv4, IPv6}, port::UInt16, flags::UInt32=UInt32(0))
    host_in = Ref(hton(host.host))
    return ccall(:jl_tcp_bind, Int32, (Ptr{Cvoid}, UInt16, Ptr{Cvoid}, Cuint, Cint),
            sock, hton(port), host_in, flags, host isa IPv6)
end

function _bind(sock::UDPSocket, host::Union{IPv4, IPv6}, port::UInt16, flags::UInt32=UInt32(0))
    host_in = Ref(hton(host.host))
    return ccall(:jl_udp_bind, Int32, (Ptr{Cvoid}, UInt16, Ptr{Cvoid}, Cuint, Cint),
            sock, hton(port), host_in, flags, host isa IPv6)
end

"""
    bind(socket::Union{TCPServer, UDPSocket, TCPSocket}, host::IPAddr, port::Integer; ipv6only=false, reuseaddr=false, kws...)

Bind `socket` to the given `host:port`. Note that `0.0.0.0` will listen on all devices.

* The `ipv6only` parameter disables dual stack mode. If `ipv6only=true`, only an IPv6 stack is created.
* If `reuseaddr=true`, multiple threads or processes can bind to the same address without error
  if they all set `reuseaddr=true`, but only the last to bind will receive any traffic.
"""
function bind(sock::Union{TCPServer, UDPSocket}, host::IPAddr, port::Integer; ipv6only = false, reuseaddr = false, kws...)
    if sock.status != StatusInit
        error("$(typeof(sock)) is not in initialization state")
    end
    flags = 0
    if isa(host, IPv6) && ipv6only
        flags |= isa(sock, UDPSocket) ? UV_UDP_IPV6ONLY : UV_TCP_IPV6ONLY
    end
    if isa(sock, UDPSocket) && reuseaddr
        flags |= UV_UDP_REUSEADDR
    end
    iolock_begin()
    err = _bind(sock, host, UInt16(port), UInt32(flags))
    if err < 0
        iolock_end()
        if err != UV_EADDRINUSE && err != UV_EACCES && err != UV_EADDRNOTAVAIL
            #TODO: this codepath is not currently tested
            throw(_UVError("bind", err))
        else
            return false
        end
    end
    if isa(sock, TCPServer) || isa(sock, UDPSocket)
        sock.status = StatusOpen
    end
    isa(sock, UDPSocket) && setopt(sock; kws...)
    iolock_end()
    return true
end

function bind(sock::TCPSocket, host::IPAddr, port::Integer; ipv6only = false, reuseaddr = false, kws...)
    if sock.status != StatusInit
        error("$(typeof(sock)) is not in initialization state")
    end
    flags = 0
    if isa(host, IPv6) && ipv6only
        flags |= isa(sock, UDPSocket) ? UV_UDP_IPV6ONLY : UV_TCP_IPV6ONLY
    end
    if isa(sock, UDPSocket) && reuseaddr
        flags |= UV_UDP_REUSEADDR
    end
    iolock_begin(sock)
    err = _bind(sock, host, UInt16(port), UInt32(flags))
    if err < 0
        iolock_end(sock)
        if err != UV_EADDRINUSE && err != UV_EACCES && err != UV_EADDRNOTAVAIL
            #TODO: this codepath is not currently tested
            throw(_UVError("bind", err))
        else
            return false
        end
    end
    if isa(sock, TCPServer) || isa(sock, UDPSocket)
        sock.status = StatusOpen
    end
    isa(sock, UDPSocket) && setopt(sock; kws...)
    iolock_end(sock)
    return true
end

bind(sock::TCPServer, addr::InetAddr) = bind(sock, addr.host, addr.port)

"""
    setopt(sock::UDPSocket; multicast_loop=nothing, multicast_ttl=nothing, enable_broadcast=nothing, ttl=nothing)

Set UDP socket options.

* `multicast_loop`: loopback for multicast packets (default: `true`).
* `multicast_ttl`: TTL for multicast packets (default: `nothing`).
* `enable_broadcast`: flag must be set to `true` if socket will be used for broadcast
  messages, or else the UDP system will return an access error (default: `false`).
* `ttl`: Time-to-live of packets sent on the socket (default: `nothing`).
"""
function setopt(sock::UDPSocket; multicast_loop=nothing, multicast_ttl=nothing, enable_broadcast=nothing, ttl=nothing)
    iolock_begin()
    if sock.status == StatusUninit
        error("Cannot set options on uninitialized socket")
    end
    if multicast_loop !== nothing
        uv_error("multicast_loop", ccall(:uv_udp_set_multicast_loop, Cint, (Ptr{Cvoid}, Cint), sock.handle, multicast_loop) < 0)
    end
    if multicast_ttl !== nothing
        uv_error("multicast_ttl", ccall(:uv_udp_set_multicast_ttl, Cint, (Ptr{Cvoid}, Cint), sock.handle, multicast_ttl))
    end
    if enable_broadcast !== nothing
        uv_error("enable_broadcast", ccall(:uv_udp_set_broadcast, Cint, (Ptr{Cvoid}, Cint), sock.handle, enable_broadcast))
    end
    if ttl !== nothing
        uv_error("ttl", ccall(:uv_udp_set_ttl, Cint, (Ptr{Cvoid}, Cint), sock.handle, ttl))
    end
    iolock_end()
    nothing
end

"""
    recv(socket::UDPSocket)

Read a UDP packet from the specified socket, and return the bytes received. This call blocks.
"""
function recv(sock::UDPSocket)
    addr, data = recvfrom(sock)
    return data
end

function uv_recvcb end

"""
    recvfrom(socket::UDPSocket) -> (host_port, data)

Read a UDP packet from the specified socket, returning a tuple of `(host_port, data)`, where
`host_port` will be an InetAddr{IPv4} or InetAddr{IPv6}, as appropriate.

!!! compat "Julia 1.3"
    Prior to Julia version 1.3, the first returned value was an address (`IPAddr`).
    In version 1.3 it was changed to an `InetAddr`.
"""
function recvfrom(sock::UDPSocket)
    iolock_begin()
    # If the socket has not been bound, it will be bound implicitly to ::0 and a random port
    if sock.status != StatusInit && sock.status != StatusOpen && sock.status != StatusActive
        error("UDPSocket is not initialized and open")
    end
    if ccall(:uv_is_active, Cint, (Ptr{Cvoid},), sock.handle) == 0
        err = ccall(:uv_udp_recv_start, Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}),
                    sock,
                    @cfunction(Base.uv_alloc_buf, Cvoid, (Ptr{Cvoid}, Csize_t, Ptr{Cvoid})),
                    @cfunction(uv_recvcb, Cvoid, (Ptr{Cvoid}, Cssize_t, Ptr{Cvoid}, Ptr{Cvoid}, Cuint)))
        uv_error("recv_start", err)
    end
    sock.status = StatusActive
    lock(sock.recvnotify)
    iolock_end()
    try
        From = Union{InetAddr{IPv4}, InetAddr{IPv6}}
        Data = Vector{UInt8}
        from, data = wait(sock.recvnotify)::Tuple{From, Data}
        return (from, data)
    finally
        unlock(sock.recvnotify)
    end
end

alloc_buf_hook(sock::UDPSocket, size::UInt) = (Libc.malloc(size), Int(size)) # size is always 64k from libuv

function uv_recvcb(handle::Ptr{Cvoid}, nread::Cssize_t, buf::Ptr{Cvoid}, addr::Ptr{Cvoid}, flags::Cuint)
    sock = @handle_as handle UDPSocket
    lock(sock.recvnotify)
    try
        buf_addr = ccall(:jl_uv_buf_base, Ptr{UInt8}, (Ptr{Cvoid},), buf)
        if nread == 0 && addr == C_NULL
            Libc.free(buf_addr)
        elseif nread < 0
            Libc.free(buf_addr)
            notify_error(sock.recvnotify, _UVError("recv", nread))
        elseif flags & UV_UDP_PARTIAL > 0
            Libc.free(buf_addr)
            notify_error(sock.recvnotify, "Partial message received")
        else
            buf_size = Int(ccall(:jl_uv_buf_len, Csize_t, (Ptr{Cvoid},), buf))
            if buf_size - nread < 16384 # waste at most 16k (note: buf_size is currently always 64k)
                buf = unsafe_wrap(Array, buf_addr, nread, own=true)
            else
                buf = Vector{UInt8}(undef, nread)
                GC.@preserve buf unsafe_copyto!(pointer(buf), buf_addr, nread)
                Libc.free(buf_addr)
            end
            # need to check the address type in order to convert to a Julia IPAddr
            host = IPv4(0)
            port = UInt16(0)
            if ccall(:jl_sockaddr_is_ip4, Cint, (Ptr{Cvoid},), addr) == 1
                host = IPv4(ntoh(ccall(:jl_sockaddr_host4, UInt32, (Ptr{Cvoid},), addr)))
                port = ntoh(ccall(:jl_sockaddr_port4, UInt16, (Ptr{Cvoid},), addr))
            elseif ccall(:jl_sockaddr_is_ip6, Cint, (Ptr{Cvoid},), addr) == 1
                tmp = Ref{UInt128}(0)
                scope_id = ccall(:jl_sockaddr_host6, UInt32, (Ptr{Cvoid}, Ptr{UInt128}), addr, tmp)
                host = IPv6(ntoh(tmp[]))
                port = ntoh(ccall(:jl_sockaddr_port6, UInt16, (Ptr{Cvoid},), addr))
            end
            from = InetAddr(host, port)
            notify(sock.recvnotify, (from, buf), all=false)
        end
        if sock.status == StatusActive && isempty(sock.recvnotify)
            sock.status = StatusOpen
            ccall(:uv_udp_recv_stop, Cint, (Ptr{Cvoid},), sock)
        end
    finally
        unlock(sock.recvnotify)
    end
    nothing
end

function _send_async(sock::UDPSocket, ipaddr::Union{IPv4, IPv6}, port::UInt16, buf)
    req = Libc.malloc(Base._sizeof_uv_udp_send)
    uv_req_set_data(req, C_NULL) # in case we get interrupted before arriving at the wait call
    host_in = Ref(hton(ipaddr.host))
    err = ccall(:jl_udp_send, Cint, (Ptr{Cvoid}, Ptr{Cvoid}, UInt16, Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Cvoid}, Cint),
                req, sock, hton(port), host_in, buf, sizeof(buf),
                @cfunction(Base.uv_writecb_task, Cvoid, (Ptr{Cvoid}, Cint)),
                ipaddr isa IPv6)
    if err < 0
        Libc.free(req)
        uv_error("send", err)
    end
    return req
end

"""
    send(socket::UDPSocket, host::IPAddr, port::Integer, msg)

Send `msg` over `socket` to `host:port`.
"""
function send(sock::UDPSocket, ipaddr::IPAddr, port::Integer, msg)
    # If the socket has not been bound, it will be bound implicitly to ::0 and a random port
    iolock_begin()
    if sock.status != StatusInit && sock.status != StatusOpen && sock.status != StatusActive
        error("UDPSocket is not initialized and open")
    end
    uvw = _send_async(sock, ipaddr, UInt16(port), msg)
    ct = current_task()
    preserve_handle(ct)
    Base.sigatomic_begin()
    uv_req_set_data(uvw, ct)
    iolock_end()
    status = try
        Base.sigatomic_end()
        wait()::Cint
    finally
        Base.sigatomic_end()
        iolock_begin()
        ct.queue === nothing || list_deletefirst!(ct.queue, ct)
        if uv_req_data(uvw) != C_NULL
            # uvw is still alive,
            # so make sure we won't get spurious notifications later
            uv_req_set_data(uvw, C_NULL)
        else
            # done with uvw
            Libc.free(uvw)
        end
        iolock_end()
        unpreserve_handle(ct)
    end
    uv_error("send", status)
    nothing
end


#from `connect`
function uv_connectcb(conn::Ptr{Cvoid}, status::Cint)
    hand = ccall(:jl_uv_connect_handle, Ptr{Cvoid}, (Ptr{Cvoid},), conn)
    sock = @handle_as hand LibuvStream
    lock(sock.cond)
    try
        if status >= 0 # success
            if !(sock.status == StatusClosed || sock.status == StatusClosing)
                sock.status = StatusOpen
            end
        else
            sock.readerror = _UVError("connect", status) # TODO: perhaps we should not reuse readerror for this
            if !(sock.status == StatusClosed || sock.status == StatusClosing)
                ccall(:jl_forceclose_uv, Cvoid, (Ptr{Cvoid},), hand)
                sock.status = StatusClosing
            end
        end
        notify(sock.cond)
    finally
        unlock(sock.cond)
    end
    Libc.free(conn)
    nothing
end

function connect!(sock::TCPSocket, host::Union{IPv4, IPv6}, port::Integer)
    iolock_begin(sock)
    if sock.status != StatusInit
        error("TCPSocket is not in initialization state")
    end
    if !(0 <= port <= typemax(UInt16))
        throw(ArgumentError("port out of range, must be 0 ≤ port ≤ 65535, got $port"))
    end
    host_in = Ref(hton(host.host))
    uv_error("connect", ccall(:jl_tcp_connect, Int32, (Ptr{Cvoid}, Ptr{Cvoid}, UInt16, Ptr{Cvoid}, Cint),
                              sock, host_in, hton(UInt16(port)), @cfunction(uv_connectcb, Cvoid, (Ptr{Cvoid}, Cint)),
                              host isa IPv6))
    sock.status = StatusConnecting
    iolock_end(sock)
    nothing
end

connect!(sock::TCPSocket, addr::InetAddr) = connect!(sock, addr.host, addr.port)

function wait_connected(x::LibuvStream)
    iolock_begin()
    check_open(x)
    isopen(x) || x.readerror === nothing || throw(x.readerror)
    preserve_handle(x)
    lock(x.cond)
    try
        while x.status == StatusConnecting
            iolock_end()
            wait(x.cond)
            unlock(x.cond)
            iolock_begin()
            lock(x.cond)
        end
        isopen(x) || x.readerror === nothing || throw(x.readerror)
    finally
        unlock(x.cond)
        unpreserve_handle(x)
    end
    iolock_end()
    nothing
end

function wait_connected(x::TCPSocket)
    iolock_begin(x)
    check_open(x)
    isopen(x) || x.readerror === nothing || throw(x.readerror)
    preserve_handle(x)
    lock(x.cond)
    try
        while x.status == StatusConnecting
            iolock_end(x)
            wait(x.cond)
            unlock(x.cond)
            iolock_begin(x)
            lock(x.cond)
        end
        isopen(x) || x.readerror === nothing || throw(x.readerror)
    finally
        unlock(x.cond)
        unpreserve_handle(x)
    end
    iolock_end(x)
    nothing
end

# Default Host to localhost

"""
    connect([host], port::Integer) -> TCPSocket

Connect to the host `host` on port `port`.
"""
connect(sock::TCPSocket, port::Integer) = connect(sock, localhost, port)
connect(port::Integer) = connect(localhost, port)

# Valid connect signatures for TCP
connect(host::AbstractString, port::Integer) = connect(TCPSocket(), host, port)
connect(addr::IPAddr, port::Integer) = connect(TCPSocket(), addr, port)
connect(addr::InetAddr) = connect(TCPSocket(), addr)

function connect!(sock::TCPSocket, host::AbstractString, port::Integer)
    if sock.status != StatusInit
        error("TCPSocket is not in initialization state")
    end
    ipaddr = getaddrinfo(host)
    connect!(sock, ipaddr, port)
    return sock
end

function connect(sock::LibuvStream, args...)
    connect!(sock, args...)
    wait_connected(sock)
    return sock
end

"""
    nagle(socket::Union{TCPServer, TCPSocket}, enable::Bool)

Enables or disables Nagle's algorithm on a given TCP server or socket.

!!! compat "Julia 1.3"
    This function requires Julia 1.3 or later.
"""
function nagle(sock::Union{TCPServer, TCPSocket}, enable::Bool)
    # disable or enable Nagle's algorithm on all OSes
    iolock_begin(sock)
    check_open(sock)
    err = ccall(:uv_tcp_nodelay, Cint, (Ptr{Cvoid}, Cint), sock.handle, Cint(!enable))
    # TODO: check err
    iolock_end(sock)
    return err
end

"""
    quickack(socket::Union{TCPServer, TCPSocket}, enable::Bool)

On Linux systems, the TCP_QUICKACK is disabled or enabled on `socket`.
"""
function quickack(sock::TCPSocket, enable::Bool)
    iolock_begin(sock)
    check_open(sock)
    @static if Sys.islinux()
        # tcp_quickack is a linux only option
        if ccall(:jl_tcp_quickack, Cint, (Ptr{Cvoid}, Cint), sock.handle, Cint(enable)) < 0
            @warn "Networking unoptimized ( Error enabling TCP_QUICKACK : $(Libc.strerror(Libc.errno())) )" maxlog=1
        end
    end
    iolock_end(sock)
    nothing
end

function quickack(sock::TCPServer, enable::Bool)
    iolock_begin()
    check_open(sock)
    @static if Sys.islinux()
        # tcp_quickack is a linux only option
        if ccall(:jl_tcp_quickack, Cint, (Ptr{Cvoid}, Cint), sock.handle, Cint(enable)) < 0
            @warn "Networking unoptimized ( Error enabling TCP_QUICKACK : $(Libc.strerror(Libc.errno())) )" maxlog=1
        end
    end
    iolock_end()
    nothing
end


##

const BACKLOG_DEFAULT = 511

"""
    listen([addr, ]port::Integer; backlog::Integer=BACKLOG_DEFAULT) -> TCPServer

Listen on port on the address specified by `addr`.
By default this listens on `localhost` only.
To listen on all interfaces pass `IPv4(0)` or `IPv6(0)` as appropriate.
`backlog` determines how many connections can be pending (not having
called [`accept`](@ref)) before the server will begin to
reject them. The default value of `backlog` is 511.
"""
function listen(addr; backlog::Integer=BACKLOG_DEFAULT)
    sock = TCPServer()
    bind(sock, addr) || error("cannot bind to port; may already be in use or access denied")
    listen(sock; backlog=backlog)
    return sock
end
listen(port::Integer; backlog::Integer=BACKLOG_DEFAULT) = listen(localhost, port; backlog=backlog)
listen(host::IPAddr, port::Integer; backlog::Integer=BACKLOG_DEFAULT) = listen(InetAddr(host, port); backlog=backlog)

function listen(sock::LibuvServer; backlog::Integer=BACKLOG_DEFAULT)
    uv_error("listen", trylisten(sock; backlog=backlog))
    return sock
end

# from `listen`
function uv_connectioncb(stream::Ptr{Cvoid}, status::Cint)
    sock = @handle_as stream LibuvServer
    lock(sock.cond)
    try
        if status >= 0
            notify(sock.cond)
        else
            notify_error(sock.cond, _UVError("connection", status))
        end
    finally
        unlock(sock.cond)
    end
    nothing
end

function trylisten(sock::LibuvServer; backlog::Integer=BACKLOG_DEFAULT)
    iolock_begin()
    check_open(sock)
    err = ccall(:uv_listen, Cint, (Ptr{Cvoid}, Cint, Ptr{Cvoid}),
                sock, backlog, @cfunction(uv_connectioncb, Cvoid, (Ptr{Cvoid}, Cint)))
    sock.status = StatusActive
    iolock_end()
    return err
end

##

function accept_nonblock(server::TCPServer, client::TCPSocket)
    iolock_begin()
    if client.status != StatusInit
        error("client TCPSocket is not in initialization state")
    end
    err = ccall(:uv_accept, Int32, (Ptr{Cvoid}, Ptr{Cvoid}), server.handle, client.handle)
    if err == 0
        client.status = StatusOpen
    end
    iolock_end()
    return err
end

function accept_nonblock(server::TCPServer)
    client = TCPSocket()
    uv_error("accept", accept_nonblock(server, client))
    return client
end

function accept(server::LibuvServer, client::LibuvStream)
    iolock_begin()
    if server.status != StatusActive && server.status != StatusClosing && server.status != StatusClosed
        throw(ArgumentError("server not connected, make sure \"listen\" has been called"))
    end
    while isopen(server)
        err = accept_nonblock(server, client)
        if err == 0
            iolock_end()
            return client
        elseif err != UV_EAGAIN
            uv_error("accept", err)
        end
        preserve_handle(server)
        lock(server.cond)
        iolock_end()
        try
            wait(server.cond)
        finally
            unlock(server.cond)
            unpreserve_handle(server)
        end
        iolock_begin()
    end
    uv_error("accept", UV_ECONNABORTED)
    nothing
end

## Utility functions

const localhost = ip"127.0.0.1"

"""
    listenany([host::IPAddr,] port_hint) -> (UInt16, TCPServer)

Create a `TCPServer` on any port, using hint as a starting point. Returns a tuple of the
actual port that the server was created on and the server itself.
"""
function listenany(host::IPAddr, default_port)
    addr = InetAddr(host, default_port)
    while true
        sock = TCPServer()
        if bind(sock, addr) && trylisten(sock) == 0
            if default_port == 0
                _addr, port = getsockname(sock)
                return (port, sock)
            end
            return (addr.port, sock)
        end
        close(sock)
        addr = InetAddr(addr.host, addr.port + 1)
        if addr.port == default_port
            error("no ports available")
        end
    end
end

listenany(default_port) = listenany(localhost, default_port)

function udp_set_membership(sock::UDPSocket, group_addr::String,
                            interface_addr::Union{Nothing, String}, operation)
    if interface_addr === nothing
        interface_addr = C_NULL
    end
    r = ccall(:uv_udp_set_membership, Cint,
              (Ptr{Cvoid}, Cstring, Cstring, Cint),
              sock.handle, group_addr, interface_addr, operation)
    uv_error("uv_udp_set_membership", r)
    return
end

"""
    join_multicast_group(sock::UDPSocket, group_addr, interface_addr = nothing)

Join a socket to a particular multicast group defined by `group_addr`.
If `interface_addr` is given, specifies a particular interface for multi-homed
systems.  Use `leave_multicast_group()` to disable reception of a group.
"""
function join_multicast_group(sock::UDPSocket, group_addr::String,
                              interface_addr::Union{Nothing, String} = nothing)
    return udp_set_membership(sock, group_addr, interface_addr, 1)
end
function join_multicast_group(sock::UDPSocket, group_addr::IPAddr,
                              interface_addr::Union{Nothing, IPAddr} = nothing)
    if interface_addr !== nothing
        interface_addr = string(interface_addr)
    end
    return join_multicast_group(sock, string(group_addr), interface_addr)
end

"""
    leave_multicast_group(sock::UDPSocket, group_addr, interface_addr = nothing)

Remove a socket from  a particular multicast group defined by `group_addr`.
If `interface_addr` is given, specifies a particular interface for multi-homed
systems.  Use `join_multicast_group()` to enable reception of a group.
"""
function leave_multicast_group(sock::UDPSocket, group_addr::String,
                               interface_addr::Union{Nothing, String} = nothing)
    return udp_set_membership(sock, group_addr, interface_addr, 0)
end
function leave_multicast_group(sock::UDPSocket, group_addr::IPAddr,
                               interface_addr::Union{Nothing, IPAddr} = nothing)
    if interface_addr !== nothing
        interface_addr = string(interface_addr)
    end
    return leave_multicast_group(sock, string(group_addr), interface_addr)
end

"""
    getsockname(sock::Union{TCPServer, TCPSocket}) -> (IPAddr, UInt16)

Get the IP address and port that the given socket is bound to.
"""
getsockname(sock::Union{TCPSocket, TCPServer}) = _sockname(sock, true)


"""
    getpeername(sock::TCPSocket) -> (IPAddr, UInt16)

Get the IP address and port of the remote endpoint that the given
socket is connected to. Valid only for connected TCP sockets.
"""
getpeername(sock::TCPSocket) = _sockname(sock, false)

function _sockname(sock, self=true)
    sock.status == StatusInit || check_open(sock)
    rport = Ref{Cushort}(0)
    raddress = zeros(UInt8, 16)
    rfamily = Ref{Cuint}(0)

    iolock_begin(sock)
    if self
        r = ccall(:jl_tcp_getsockname, Int32,
                (Ptr{Cvoid}, Ref{Cushort}, Ptr{Cvoid}, Ref{Cuint}),
                sock.handle, rport, raddress, rfamily)
    else
        r = ccall(:jl_tcp_getpeername, Int32,
                (Ptr{Cvoid}, Ref{Cushort}, Ptr{Cvoid}, Ref{Cuint}),
                sock.handle, rport, raddress, rfamily)
    end
    iolock_end(sock)
    uv_error("cannot obtain socket name", r)
    port = ntoh(rport[])
    af_inet6 = @static if Sys.iswindows() # AF_INET6 in <sys/socket.h>
        23
    elseif Sys.isapple()
        30
    elseif Sys.KERNEL ∈ (:FreeBSD, :DragonFly)
        28
    elseif Sys.KERNEL ∈ (:NetBSD, :OpenBSD)
        24
    else
        10
    end

    if rfamily[] == 2 # AF_INET
        addrv4 = raddress[1:4]
        naddr = ntoh(unsafe_load(Ptr{Cuint}(pointer(addrv4)), 1))
        addr = IPv4(naddr)
    elseif rfamily[] == af_inet6
        naddr = ntoh(unsafe_load(Ptr{UInt128}(pointer(raddress)), 1))
        addr = IPv6(naddr)
    else
        error(string("unsupported address family: ", rfamily[]))
    end
    return addr, port
end

# Overrides.

function Base.wait_readnb(x::TCPSocket, nb::Int)
    # fast path before iolock acquire
    bytesavailable(x.buffer) >= nb && return
    open = isopen(x) && x.status != StatusEOF # must precede readerror check
    x.readerror === nothing || throw(x.readerror)
    open || return
    iolock_begin(x)
    # repeat fast path after iolock acquire, before other expensive work
    bytesavailable(x.buffer) >= nb && (iolock_end(x); return)
    open = isopen(x) && x.status != StatusEOF
    x.readerror === nothing || throw(x.readerror)
    open || (iolock_end(x); return)
    # now do the "real" work
    oldthrottle = x.throttle
    preserve_handle(x)
    lock(x.cond)
    try
        while bytesavailable(x.buffer) < nb
            x.readerror === nothing || throw(x.readerror)
            isopen(x) || break
            x.status == StatusEOF && break
            x.throttle = max(nb, x.throttle)
            start_reading(x) # ensure we are reading
            iolock_end(x)
            wait(x.cond)
            unlock(x.cond)
            iolock_begin(x)
            lock(x.cond)
        end
    finally
        if isempty(x.cond)
            stop_reading(x) # stop reading iff there are currently no other read clients of the stream
        end
        if oldthrottle <= x.throttle <= nb
            # if we're interleaving readers, we might not get back to the "original" throttle
            # but we consider that an acceptable "risk", since we can't be quite sure what the intended value is now
            x.throttle = oldthrottle
        end
        unpreserve_handle(x)
        unlock(x.cond)
    end
    iolock_end(x)
    nothing
end

function Base.closewrite(s::TCPSocket)
    iolock_begin(s)
    check_open(s)
    req = Libc.malloc(_sizeof_uv_shutdown)
    uv_req_set_data(req, C_NULL) # in case we get interrupted before arriving at the wait call
    err = ccall(:uv_shutdown, Int32, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}),
                req, s, @cfunction(Base.uv_shutdowncb_task, Cvoid, (Ptr{Cvoid}, Cint)))
    if err < 0
        Libc.free(req)
        uv_error("shutdown", err)
    end
    ct = current_task()
    preserve_handle(ct)
    Base.sigatomic_begin()
    uv_req_set_data(req, ct)
    iolock_end(s)
    status = try
        Base.sigatomic_end()
        wait()::Cint
    finally
        # try-finally unwinds the sigatomic level, so need to repeat sigatomic_end
        Base.sigatomic_end()
        iolock_begin(s)
        ct.queue === nothing || list_deletefirst!(ct.queue, ct)
        if uv_req_data(req) != C_NULL
            # req is still alive,
            # so make sure we won't get spurious notifications later
            uv_req_set_data(req, C_NULL)
        else
            # done with req
            Libc.free(req)
        end
        iolock_end(s)
        unpreserve_handle(ct)
    end
    if isopen(s)
        if status < 0 || ccall(:uv_is_readable, Cint, (Ptr{Cvoid},), s.handle) == 0
            close(s)
        end
    end
    if status < 0
        throw(_UVError("shutdown", status))
    end
    nothing
end

function Base.close(stream::TCPSocket)
    iolock_begin(stream)
    should_wait = false
    if stream.status == StatusInit
        ccall(:jl_forceclose_uv, Cvoid, (Ptr{Cvoid},), stream.handle)
        stream.status = StatusClosing
    elseif isopen(stream)
        should_wait = Base.uv_handle_data(stream) != C_NULL
        if stream.status != StatusClosing
            ccall(:jl_close_uv, Cvoid, (Ptr{Cvoid},), stream.handle)
            stream.status = StatusClosing
        end
    end
    iolock_end(stream)
    should_wait && wait_close(stream)
    nothing
end


function Base.uvfinalize(uv::TCPSocket)
    uv.handle == C_NULL && return
    iolock_begin(uv)
    if uv.handle != C_NULL
        Base.disassociate_julia_struct(uv.handle) # not going to call the usual close hooks
        if uv.status != StatusUninit
            close(uv)
        else
            Libc.free(uv.handle)
        end
        uv.status = StatusClosed
        uv.handle = C_NULL
    end
    iolock_end(uv)
    nothing
end

function start_reading(stream::TCPSocket)
    iolock_begin(stream)
    if stream.status == StatusOpen
        if !isreadable(stream)
            error("tried to read a stream that is not readable")
        end
        # libuv may call the alloc callback immediately
        # for a TTY on Windows, so ensure the status is set first
        stream.status = StatusActive
        ret = ccall(:uv_read_start, Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}),
                    stream, @cfunction(Base.uv_alloc_buf, Cvoid, (Ptr{Cvoid}, Csize_t, Ptr{Cvoid})),
                    @cfunction(Base.uv_readcb, Cvoid, (Ptr{Cvoid}, Cssize_t, Ptr{Cvoid})))
    elseif stream.status == StatusPaused
        stream.status = StatusActive
        ret = Int32(0)
    elseif stream.status == StatusActive
        ret = Int32(0)
    else
        ret = Int32(-1)
    end
    iolock_end(stream)
    return ret
end


if Sys.iswindows()
    function Base.stop_reading(stream::TCPSocket)
        iolock_begin(stream)
        if stream.status == StatusActive
            stream.status = StatusOpen
            ccall(:uv_read_stop, Cint, (Ptr{Cvoid},), stream)
        end
        iolock_end(stream)
        nothing
    end
else
    function Base.stop_reading(stream::TCPSocket)
        iolock_begin(stream)
        if stream.status == StatusActive
            stream.status = StatusPaused
        end
        iolock_end(stream)
        nothing
    end
end


function Base.readbytes!(s::TCPSocket, a::Vector{UInt8}, nb::Int)
    iolock_begin(s)
    sbuf = s.buffer
    @assert sbuf.seekable == false
    @assert sbuf.maxsize >= nb

    function wait_locked(s, buf, nb)
        while bytesavailable(buf) < nb
            s.readerror === nothing || throw(s.readerror)
            isopen(s) || break
            s.status != StatusEOF || break
            iolock_end(s)
            wait_readnb(s, nb)
            iolock_begin(s)
        end
    end

    if nb <= SZ_UNBUFFERED_IO # Under this limit we are OK with copying the array from the stream's buffer
        wait_locked(s, sbuf, nb)
    end
    if bytesavailable(sbuf) >= nb
        nread = readbytes!(sbuf, a, nb)
    else
        newbuf = PipeBuffer(a, maxsize=nb)
        newbuf.size = 0 # reset the write pointer to the beginning
        nread = try
            s.buffer = newbuf
            write(newbuf, sbuf)
            wait_locked(s, newbuf, nb)
            bytesavailable(newbuf)
        finally
            s.buffer = sbuf
        end
        compact(newbuf)
    end
    iolock_end(s)
    return nread
end

function Base.read(stream::TCPSocket)
    wait_readnb(stream, typemax(Int))
    iolock_begin(stream)
    bytes = take!(stream.buffer)
    iolock_end(stream)
    return bytes
end


function Base.unsafe_read(s::TCPSocket, p::Ptr{UInt8}, nb::UInt)
    iolock_begin(s)
    sbuf = s.buffer
    @assert sbuf.seekable == false
    @assert sbuf.maxsize >= nb

    function wait_locked(s, buf, nb)
        while bytesavailable(buf) < nb
            s.readerror === nothing || throw(s.readerror)
            isopen(s) || throw(EOFError())
            s.status != StatusEOF || throw(EOFError())
            iolock_end(s)
            wait_readnb(s, nb)
            iolock_begin(s)
        end
    end

    if nb <= SZ_UNBUFFERED_IO # Under this limit we are OK with copying the array from the stream's buffer
        wait_locked(s, sbuf, Int(nb))
    end
    if bytesavailable(sbuf) >= nb
        unsafe_read(sbuf, p, nb)
    else
        newbuf = PipeBuffer(unsafe_wrap(Array, p, nb), maxsize=Int(nb))
        newbuf.size = 0 # reset the write pointer to the beginning
        try
            s.buffer = newbuf
            write(newbuf, sbuf)
            wait_locked(s, newbuf, Int(nb))
        finally
            s.buffer = sbuf
        end
    end
    iolock_end(s)
    nothing
end


function Base.read(this::TCPSocket, ::Type{UInt8})
    iolock_begin(this)
    sbuf = this.buffer
    @assert sbuf.seekable == false
    while bytesavailable(sbuf) < 1
        iolock_end(this)
        eof(this) && throw(EOFError())
        iolock_begin(this)
    end
    c = read(sbuf, UInt8)
    iolock_end(this)
    return c
end

function Base.readavailable(this::TCPSocket)
    wait_readnb(this, 1) # unlike the other `read` family of functions, this one doesn't guarantee error reporting
    iolock_begin(this)
    buf = this.buffer
    @assert buf.seekable == false
    bytes = take!(buf)
    iolock_end(this)
    return bytes
end

function Base.readuntil(x::TCPSocket, c::UInt8; keep::Bool=false)
    iolock_begin(x)
    buf = x.buffer
    @assert buf.seekable == false
    if !occursin(c, buf) # fast path checks first
        x.readerror === nothing || throw(x.readerror)
        if isopen(x) && x.status != StatusEOF
            preserve_handle(x)
            lock(x.cond)
            try
                while !occursin(c, x.buffer)
                    x.readerror === nothing || throw(x.readerror)
                    isopen(x) || break
                    x.status != StatusEOF || break
                    start_reading(x) # ensure we are reading
                    iolock_end(x)
                    wait(x.cond)
                    unlock(x.cond)
                    iolock_begin(x)
                    lock(x.cond)
                end
            finally
                if isempty(x.cond)
                    stop_reading(x) # stop reading iff there are currently no other read clients of the stream
                end
                unlock(x.cond)
                unpreserve_handle(x)
            end
        end
    end
    bytes = readuntil(buf, c, keep=keep)
    iolock_end(x)
    return bytes
end

uv_write(s::TCPSocket, p::Vector{UInt8}) = GC.@preserve p uv_write(s, pointer(p), UInt(sizeof(p)))

function uv_write(s::TCPSocket, p::Ptr{UInt8}, n::UInt)
    uvw = Base.uv_write_async(s, p, n)
    ct = current_task()
    preserve_handle(ct)
    Base.sigatomic_begin()
    uv_req_set_data(uvw, ct)
    iolock_end(s)
    status = try
        Base.sigatomic_end()
        # wait for the last chunk to complete (or error)
        # assume that any errors would be sticky,
        # (so we don't need to monitor the error status of the intermediate writes)
        wait()::Cint
    finally
        # try-finally unwinds the sigatomic level, so need to repeat sigatomic_end
        Base.sigatomic_end()
        iolock_begin(s)
        ct.queue === nothing || list_deletefirst!(ct.queue, ct)
        if uv_req_data(uvw) != C_NULL
            # uvw is still alive,
            # so make sure we won't get spurious notifications later
            uv_req_set_data(uvw, C_NULL)
        else
            # done with uvw
            Libc.free(uvw)
        end
        iolock_end(s)
        unpreserve_handle(ct)
    end
    if status < 0
        throw(_UVError("write", status))
    end
    return Int(n)
end

function Base.unsafe_write(s::TCPSocket, p::Ptr{UInt8}, n::UInt)
    while true
        # try to add to the send buffer
        iolock_begin(s)
        buf = s.sendbuf
        buf === nothing && break
        totb = bytesavailable(buf) + n
        if totb < buf.maxsize
            nb = unsafe_write(buf, p, n)
            iolock_end(s)
            return nb
        end
        bytesavailable(buf) == 0 && break
        # perform flush(s)
        arr = take!(buf)
        uv_write(s, arr)
    end
    # perform the output to the kernel
    return uv_write(s, p, n)
end

function Base.flush(s::TCPSocket)
    iolock_begin(s)
    buf = s.sendbuf
    if buf !== nothing
        if bytesavailable(buf) > 0
            arr = take!(buf)
            uv_write(s, arr)
            return
        end
    end # TODO @vustef: eventloop() was a problem in `vs-182-eventloop-per-socket`, and similar.
    uv_write(s, Ptr{UInt8}(Base.eventloop()), UInt(0)) # zero write from a random pointer to flush current queue
    return
end

function Base.buffer_writes(s::TCPSocket, bufsize)
    sendbuf = PipeBuffer(bufsize)
    iolock_begin(s)
    s.sendbuf = sendbuf
    iolock_end(s)
    return s
end

function Base.write(s::TCPSocket, b::UInt8)
    buf = s.sendbuf
    if buf !== nothing
        iolock_begin(s)
        if bytesavailable(buf) + 1 < buf.maxsize
            n = write(buf, b)
            iolock_end(s)
            return n
        end
        iolock_end(s)
    end
    return write(s, Ref{UInt8}(b))
end

function Base.wait(s::TCPSocket)
    GC.safepoint()
    W = Workqueues[Threads.threadid()]
    poptask(W)
    result = try_yieldto(ensure_rescheduled)
    process_events(s)
    # return when we come out of the queue
    return result
end

# domain sockets

include("PipeServer.jl")

end
