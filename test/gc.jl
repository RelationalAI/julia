# This file is a part of Julia. License is MIT: https://julialang.org/license

using Test

function run_gctest(file)
    let cmd = `$(Base.julia_cmd()) --depwarn=error --rr-detach --startup-file=no $file`
        @testset for test_nthreads in (1, 2, 4)
            @testset for concurrent_sweep in (0, 1)
                new_env = copy(ENV)
                new_env["JULIA_NUM_THREADS"] = string(test_nthreads)
                new_env["JULIA_NUM_GC_THREADS"] = "$(test_nthreads),$(concurrent_sweep)"
                @test success(run(pipeline(setenv(cmd, new_env), stdout = stdout, stderr = stderr)))
            end
        end
    end
end

@time run_gctest("gc/binarytree.jl")
@time run_gctest("gc/linkedlist.jl")
@time run_gctest("gc/objarray.jl")
