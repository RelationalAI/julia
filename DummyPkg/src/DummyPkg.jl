module DummyPkg

const Arr = Int[123, 456, 789]

const Small = 42

const Str = "Lorem ipsum"

const Big = BigInt(10)^123

struct MyType{T}
    a::NTuple{16,Int}
    b::Float64
    c::Float32
    d::Float16
end

const TN = Base.typename(MyType)

function foo(x)
    x
end

function foo(y::MyType)
    @inline sum(y.a) + y.b + y.c + y.d
end

function bar(y::MyType)
    sum(y.a) + y.b + y.c + y.d
end


@generated function gar(y::MyType)
    return y
end

gar(MyType{Float64}(ntuple(_->0,16),0,0,0))

precompile(Tuple{typeof(foo), Int})
precompile(Tuple{typeof(foo), BigFloat})
precompile(Tuple{typeof(foo), MyType{Any}})
precompile(Tuple{typeof(foo), MyType{Int}})
precompile(Tuple{typeof(foo), MyType{Float32}})
precompile(Tuple{typeof(bar), MyType{Any}})
precompile(Tuple{typeof(bar), MyType{Int}})
precompile(Tuple{typeof(bar), MyType{Float32}})
precompile(Tuple{typeof(gar), MyType{Any}})
precompile(Tuple{typeof(gar), MyType{Int}})
precompile(Tuple{typeof(gar), MyType{Float32}})

end # module DummyPkg
