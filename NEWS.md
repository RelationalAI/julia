Julia v1.10 Release Notes
=========================

New language features
---------------------

* JuliaSyntax.jl is now used as the default parser, providing better diagnostics and faster
  parsing. Set environment variable `JULIA_USE_FLISP_PARSER` to `1` to switch back to the old
  parser if necessary (and if you find this necessary, please file an issue) ([#46372]).
* `⥺` (U+297A, `\leftarrowsubset`) and `⥷` (U+2977, `\leftarrowless`) may now be used as
  binary operators with arrow precedence ([#45962]).

Language changes
----------------

* When a task forks a child, the parent task's task-local RNG (random number generator) is no longer affected. The seeding of child based on the parent task also takes a more disciplined approach to collision resistance, using a design based on the SplitMix and DotMix splittable RNG schemes ([#49110]).
* A new more-specific rule for methods resolves ambiguities containing Union{} in favor of
  the method defined explicitly to handle the Union{} argument. This makes it possible to
  define methods to explicitly handle Union{} without the ambiguities that commonly would
  result previously. This also lets the runtime optimize certain method lookups in a way
  that significantly improves load and inference times for heavily overloaded methods that
  dispatch on Types (such as traits and constructors). ([#49349])
* The "h bar" `ℏ` (`\hslash` U+210F) character is now treated as equivalent to `ħ` (`\hbar` U+0127).
* The `@simd` macro now has more limited and clearer semantics: it only enables reordering and contraction
  of floating-point operations, instead of turning on all "fastmath" optimizations.
  If you observe performance regressions due to this change, you can recover previous behavior with `@fastmath @simd`,
  if you are OK with all the optimizations enabled by the `@fastmath` macro ([#49405]).
* When a method with keyword arguments is displayed in the stack trace view, the textual
  representation of the keyword arguments' type is simplified using the new
  `@Kwargs{key1::Type1, ...}` macro syntax ([#49959]).

Compiler/Runtime improvements
-----------------------------

* The mark phase of the garbage collector is now multi-threaded ([#48600]).
* [JITLink](https://llvm.org/docs/JITLink.html) is enabled by default on Linux aarch64 when Julia is linked to LLVM 15 or later versions ([#49745]).
  This should resolve many segmentation faults previously observed on this platform.
* The precompilation process now uses pidfile locks and orchestrates multiple julia processes to only have one process
  spend effort precompiling while the others wait. Previously all would do the work and race to overwrite the cache files.
  ([#49052])

Command-line option changes
---------------------------

* New option `--gcthreads` to set how many threads will be used by the garbage collector ([#48600]).
  The default is `N/2` where `N` is the number of worker threads (`--threads`) used by Julia.

Build system changes
--------------------

* SparseArrays and SuiteSparse are no longer included in the default system image, so the core
  language no longer contains GPL libraries. However, these libraries are still included
  alongside the language in the standard binary distribution ([#44247], [#48979], [#49266]).

New library functions
---------------------

* `tanpi` is now defined. It computes tan(π*x) more accurately than `tan(pi*x)` ([#48575]).
* `fourthroot(x)` is now defined in `Base.Math` and can be used to compute the fourth root of `x`.
   It can also be accessed using the unicode character `∜`, which can be typed by `\fourthroot<tab>` ([#48899]).
* `Libc.memmove`, `Libc.memset`, and `Libc.memcpy` are now defined, whose functionality matches that of their respective C calls.
* `Base.isprecompiled(pkg::PkgId)` has been added, to identify whether a package has already been precompiled ([#50218]).

New library features
--------------------

* `binomial(x, k)` now supports non-integer `x` ([#48124]).
* A `CartesianIndex` is now treated as a "scalar" for broadcasting ([#47044]).
* `printstyled` now supports italic output ([#45164]).
* `parent` and `parentindices` support `SubString`s.
* `replace(string, pattern...)` now supports an optional `IO` argument to
  write the output to a stream rather than returning a string ([#48625]).
* `startswith` now supports seekable `IO` streams ([#43055]).

Standard library changes
------------------------

* The `initialized=true` keyword assignment for `sortperm!` and `partialsortperm!`
  is now a no-op ([#47979]). It previously exposed unsafe behavior ([#47977]).
* Printing integral `Rational`s will skip the denominator in `Rational`-typed IO context (e.g. in arrays) ([#45396]).

#### Package Manager

* `Pkg.precompile` now accepts `timing` as a keyword argument which displays per package timing information for precompilation (e.g. `Pkg.precompile(timing=true)`).

#### LinearAlgebra

* `AbstractQ` no longer subtypes `AbstractMatrix`. Moreover, `adjoint(Q::AbstractQ)`
  no longer wraps `Q` in an `Adjoint` type, but instead in an `AdjointQ`, that itself
  subtypes `AbstractQ`. This change accounts for the fact that typically `AbstractQ`
  instances behave like function-based, matrix-backed linear operators, and hence don't
  allow for efficient indexing. Also, many `AbstractQ` types can act on vectors/matrices
  of different size, acting like a matrix with context-dependent size. With this change,
  `AbstractQ` has a well-defined API that is described in detail in the
  [Julia documentation](https://docs.julialang.org/en/v1/stdlib/LinearAlgebra/#man-linalg-abstractq)
  ([#46196]).
* Adjoints and transposes of `Factorization` objects are no longer wrapped in `Adjoint`
  and `Transpose` wrappers, respectively. Instead, they are wrapped in
  `AdjointFactorization` and `TranposeFactorization` types, which themselves subtype
  `Factorization` ([#46874]).
* New functions `hermitianpart` and `hermitianpart!` for extracting the Hermitian
  (real symmetric) part of a matrix ([#31836]).
* The `norm` of the adjoint or transpose of an `AbstractMatrix` now returns the norm of the
  parent matrix by default, matching the current behaviour for `AbstractVector`s ([#49020]).
* `eigen(A, B)` and `eigvals(A, B)`, where one of `A` or `B` is symmetric or Hermitian,
  are now fully supported ([#49533]).
* `eigvals/eigen(A, cholesky(B))` now computes the generalized eigenvalues (`eigen`: and eigenvectors)
  of `A` and `B` via Cholesky decomposition for positive definite `B`. Note: The second argument is
  the output of `cholesky`.

#### Printf

* Format specifiers now support dynamic width and precision, e.g. `%*s` and `%*.*g` ([#40105]).

#### REPL

* When stack traces are printed, the printed depth of types in function signatures will be limited
  to avoid overly verbose output ([#49795]).

#### Test

* The `@test_broken` macro (or `@test` with `broken=true`) now complains if the test expression returns a
  non-boolean value in the same way as a non-broken test ([#47804]).
* When a call to `@test` fails or errors inside a function, a larger stacktrace is now printed such that the location of the  test within a `@testset` can be retrieved ([#49451]).

#### InteractiveUtils

* `code_native` and `@code_native` now default to intel syntax instead of AT&T.
* `@time_imports` now shows the timing of any module `__init__()`s that are run ([#49529]).

Deprecated or removed
---------------------

* The `@pure` macro is now deprecated. Use `Base.@assume_effects :foldable` instead ([#48682]).

- A wall-time profiler is now available for users who need a sampling profiler that captures tasks regardless of their scheduling or running state. This type of profiler enables profiling of I/O-heavy tasks and helps detect areas of heavy contention in the system ([#55889]).

<!--- generated by NEWS-update.jl: -->
[#31836]: https://github.com/JuliaLang/julia/issues/31836
[#40105]: https://github.com/JuliaLang/julia/issues/40105
[#43055]: https://github.com/JuliaLang/julia/issues/43055
[#44247]: https://github.com/JuliaLang/julia/issues/44247
[#45164]: https://github.com/JuliaLang/julia/issues/45164
[#45396]: https://github.com/JuliaLang/julia/issues/45396
[#45962]: https://github.com/JuliaLang/julia/issues/45962
[#46196]: https://github.com/JuliaLang/julia/issues/46196
[#46372]: https://github.com/JuliaLang/julia/issues/46372
[#46874]: https://github.com/JuliaLang/julia/issues/46874
[#47044]: https://github.com/JuliaLang/julia/issues/47044
[#47804]: https://github.com/JuliaLang/julia/issues/47804
[#47977]: https://github.com/JuliaLang/julia/issues/47977
[#47979]: https://github.com/JuliaLang/julia/issues/47979
[#48124]: https://github.com/JuliaLang/julia/issues/48124
[#48575]: https://github.com/JuliaLang/julia/issues/48575
[#48600]: https://github.com/JuliaLang/julia/issues/48600
[#48625]: https://github.com/JuliaLang/julia/issues/48625
[#48682]: https://github.com/JuliaLang/julia/issues/48682
[#48899]: https://github.com/JuliaLang/julia/issues/48899
[#48979]: https://github.com/JuliaLang/julia/issues/48979
[#49020]: https://github.com/JuliaLang/julia/issues/49020
[#49052]: https://github.com/JuliaLang/julia/issues/49052
[#49110]: https://github.com/JuliaLang/julia/issues/49110
[#49266]: https://github.com/JuliaLang/julia/issues/49266
[#49405]: https://github.com/JuliaLang/julia/issues/49405
[#49451]: https://github.com/JuliaLang/julia/issues/49451
[#49529]: https://github.com/JuliaLang/julia/issues/49529
[#49533]: https://github.com/JuliaLang/julia/issues/49533
[#49745]: https://github.com/JuliaLang/julia/issues/49745
[#49795]: https://github.com/JuliaLang/julia/issues/49795
[#49959]: https://github.com/JuliaLang/julia/issues/49959
[#50218]: https://github.com/JuliaLang/julia/issues/50218
