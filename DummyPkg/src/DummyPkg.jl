module DummyPkg

const Arr = Int[123, 456, 789]

const Small = 42

const Str = "Lorem ipsum"

const Big = BigInt(10)^123

struct MyType
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
    sum(y.a) + y.b + y.c + y.d
end

precompile(Tuple{typeof(foo), Int})
precompile(Tuple{typeof(foo), BigFloat})
precompile(Tuple{typeof(foo), MyType})

end # module DummyPkg
