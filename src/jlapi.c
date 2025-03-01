// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  jlapi.c
  miscellaneous functions for users of libjulia.so, to handle initialization
  and the style of use where julia is not in control most of the time.
*/
#include "platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "julia.h"
#include "options.h"
#include "julia_assert.h"
#include "julia_internal.h"

#ifdef USE_TRACY
#include "tracy/TracyC.h"
#endif

#ifdef __cplusplus
#include <cfenv>
extern "C" {
#else
#include <fenv.h>
#endif

JL_DLLEXPORT int jl_is_initialized(void)
{
    return jl_main_module != NULL;
}

JL_DLLEXPORT void jl_set_ARGS(int argc, char **argv)
{
    if (jl_core_module != NULL) {
        jl_array_t *args = (jl_array_t*)jl_get_global(jl_core_module, jl_symbol("ARGS"));
        if (args == NULL) {
            args = jl_alloc_vec_any(0);
            JL_GC_PUSH1(&args);
            jl_set_const(jl_core_module, jl_symbol("ARGS"), (jl_value_t*)args);
            JL_GC_POP();
        }
        assert(jl_array_len(args) == 0);
        jl_array_grow_end(args, argc);
        int i;
        for (i = 0; i < argc; i++) {
            jl_value_t *s = (jl_value_t*)jl_cstr_to_string(argv[i]);
            jl_arrayset(args, s, i);
        }
    }
}

// First argument is the usr/bin directory where the julia binary is, or NULL to guess.
// Second argument is the path of a system image file (*.so).
// A non-absolute path is interpreted as relative to the first argument path, or
// relative to the default julia home dir.
// The default is something like ../lib/julia/sys.so
JL_DLLEXPORT void jl_init_with_image(const char *julia_bindir,
                                     const char *image_path)
{
    if (jl_is_initialized())
        return;
    libsupport_init();
    jl_options.julia_bindir = julia_bindir;
    if (image_path != NULL)
        jl_options.image_file = image_path;
    else
        jl_options.image_file = jl_get_default_sysimg_path();
    julia_init(JL_IMAGE_JULIA_HOME);
    jl_exception_clear();
}

JL_DLLEXPORT void jl_init(void)
{
    char *libbindir = NULL;
#ifdef _OS_WINDOWS_
    libbindir = strdup(jl_get_libdir());
#else
    (void)asprintf(&libbindir, "%s" PATHSEPSTRING ".." PATHSEPSTRING "%s", jl_get_libdir(), "bin");
#endif
    if (!libbindir) {
        printf("jl_init unable to find libjulia!\n");
        abort();
    }
    jl_init_with_image(libbindir, jl_get_default_sysimg_path());
    free(libbindir);
}

// HACK: remove this for Julia 1.8 (see <https://github.com/JuliaLang/julia/issues/40730>)
JL_DLLEXPORT void jl_init__threading(void)
{
    jl_init();
}

// HACK: remove this for Julia 1.8 (see <https://github.com/JuliaLang/julia/issues/40730>)
JL_DLLEXPORT void jl_init_with_image__threading(const char *julia_bindir,
                                     const char *image_relative_path)
{
    jl_init_with_image(julia_bindir, image_relative_path);
}

static void _jl_exception_clear(jl_task_t *ct) JL_NOTSAFEPOINT
{
    ct->ptls->previous_exception = NULL;
}

JL_DLLEXPORT jl_value_t *jl_eval_string(const char *str)
{
    jl_value_t *r;
    jl_task_t *ct = jl_current_task;
    JL_TRY {
        const char filename[] = "none";
        jl_value_t *ast = jl_parse_all(str, strlen(str),
                filename, strlen(filename), 1);
        JL_GC_PUSH1(&ast);
        r = jl_toplevel_eval_in(jl_main_module, ast);
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        r = NULL;
    }
    return r;
}

JL_DLLEXPORT jl_value_t *jl_current_exception(void) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT
{
    jl_excstack_t *s = jl_current_task->excstack;
    return s && s->top != 0 ? jl_excstack_exception(s, s->top) : jl_nothing;
}

JL_DLLEXPORT jl_value_t *jl_exception_occurred(void)
{
    return jl_current_task->ptls->previous_exception;
}

JL_DLLEXPORT void jl_exception_clear(void)
{
    _jl_exception_clear(jl_current_task);
}

// get the name of a type as a string
JL_DLLEXPORT const char *jl_typename_str(jl_value_t *v)
{
    if (!jl_is_datatype(v))
        return NULL;
    return jl_symbol_name(((jl_datatype_t*)v)->name->name);
}

// get the name of typeof(v) as a string
JL_DLLEXPORT const char *jl_typeof_str(jl_value_t *v)
{
    return jl_typename_str((jl_value_t*)jl_typeof(v));
}

JL_DLLEXPORT void *jl_array_eltype(jl_value_t *a)
{
    return jl_tparam0(jl_typeof(a));
}

JL_DLLEXPORT int jl_array_rank(jl_value_t *a)
{
    return jl_array_ndims(a);
}

JL_DLLEXPORT size_t jl_array_size(jl_value_t *a, int d)
{
    return jl_array_dim(a, d);
}

JL_DLLEXPORT const char *jl_string_ptr(jl_value_t *s)
{
    return jl_string_data(s);
}

JL_DLLEXPORT jl_value_t *jl_call(jl_function_t *f, jl_value_t **args, uint32_t nargs)
{
    jl_value_t *v;
    jl_task_t *ct = jl_current_task;
    nargs++; // add f to args
    JL_TRY {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        argv[0] = (jl_value_t*)f;
        for (int i = 1; i < nargs; i++)
            argv[i] = args[i - 1];
        size_t last_age = ct->world_age;
        ct->world_age = jl_get_world_counter();
        v = jl_apply(argv, nargs);
        ct->world_age = last_age;
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT jl_value_t *jl_call0(jl_function_t *f)
{
    jl_value_t *v;
    jl_task_t *ct = jl_current_task;
    JL_TRY {
        JL_GC_PUSH1(&f);
        size_t last_age = ct->world_age;
        ct->world_age = jl_get_world_counter();
        v = jl_apply_generic(f, NULL, 0);
        ct->world_age = last_age;
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT jl_value_t *jl_call1(jl_function_t *f, jl_value_t *a)
{
    jl_value_t *v;
    jl_task_t *ct = jl_current_task;
    JL_TRY {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, 2);
        argv[0] = f;
        argv[1] = a;
        size_t last_age = ct->world_age;
        ct->world_age = jl_get_world_counter();
        v = jl_apply(argv, 2);
        ct->world_age = last_age;
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT jl_value_t *jl_call2(jl_function_t *f, jl_value_t *a, jl_value_t *b)
{
    jl_value_t *v;
    jl_task_t *ct = jl_current_task;
    JL_TRY {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, 3);
        argv[0] = f;
        argv[1] = a;
        argv[2] = b;
        size_t last_age = ct->world_age;
        ct->world_age = jl_get_world_counter();
        v = jl_apply(argv, 3);
        ct->world_age = last_age;
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT jl_value_t *jl_call3(jl_function_t *f, jl_value_t *a,
                                  jl_value_t *b, jl_value_t *c)
{
    jl_value_t *v;
    jl_task_t *ct = jl_current_task;
    JL_TRY {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, 4);
        argv[0] = f;
        argv[1] = a;
        argv[2] = b;
        argv[3] = c;
        size_t last_age = ct->world_age;
        ct->world_age = jl_get_world_counter();
        v = jl_apply(argv, 4);
        ct->world_age = last_age;
        JL_GC_POP();
        _jl_exception_clear(ct);
    }
    JL_CATCH {
        ct->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT void jl_yield(void)
{
    static jl_function_t *yieldfunc = NULL;
    if (yieldfunc == NULL)
        yieldfunc = (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("yield"));
    if (yieldfunc != NULL)
        jl_call0(yieldfunc);
}

JL_DLLEXPORT jl_value_t *jl_get_field(jl_value_t *o, const char *fld)
{
    jl_value_t *v;
    JL_TRY {
        jl_value_t *s = (jl_value_t*)jl_symbol(fld);
        int i = jl_field_index((jl_datatype_t*)jl_typeof(o), (jl_sym_t*)s, 1);
        v = jl_get_nth_field(o, i);
        jl_exception_clear();
    }
    JL_CATCH {
        jl_current_task->ptls->previous_exception = jl_current_exception();
        v = NULL;
    }
    return v;
}

JL_DLLEXPORT void jl_sigatomic_begin(void)
{
    JL_SIGATOMIC_BEGIN();
}

JL_DLLEXPORT void jl_sigatomic_end(void)
{
    jl_task_t *ct = jl_current_task;
    if (ct->ptls->defer_signal == 0)
        jl_error("sigatomic_end called in non-sigatomic region");
    JL_SIGATOMIC_END();
}

JL_DLLEXPORT int jl_is_debugbuild(void) JL_NOTSAFEPOINT
{
#ifdef JL_DEBUG_BUILD
    return 1;
#else
    return 0;
#endif
}

JL_DLLEXPORT int8_t jl_is_memdebug(void) JL_NOTSAFEPOINT {
#ifdef MEMDEBUG
    return 1;
#else
    return 0;
#endif
}

JL_DLLEXPORT jl_value_t *jl_get_julia_bindir(void)
{
    return jl_cstr_to_string(jl_options.julia_bindir);
}

JL_DLLEXPORT jl_value_t *jl_get_julia_bin(void)
{
    return jl_cstr_to_string(jl_options.julia_bin);
}

JL_DLLEXPORT jl_value_t *jl_get_image_file(void)
{
    return jl_cstr_to_string(jl_options.image_file);
}

JL_DLLEXPORT int jl_ver_major(void)
{
    return JULIA_VERSION_MAJOR;
}

JL_DLLEXPORT int jl_ver_minor(void)
{
    return JULIA_VERSION_MINOR;
}

JL_DLLEXPORT int jl_ver_patch(void)
{
    return JULIA_VERSION_PATCH;
}

JL_DLLEXPORT int jl_ver_is_release(void)
{
    return JULIA_VERSION_IS_RELEASE;
}

JL_DLLEXPORT const char *jl_ver_string(void)
{
   return JULIA_VERSION_STRING;
}

// return char* from String field in Base.GIT_VERSION_INFO
static const char *git_info_string(const char *fld)
{
    static jl_value_t *GIT_VERSION_INFO = NULL;
    if (!GIT_VERSION_INFO)
        GIT_VERSION_INFO = jl_get_global(jl_base_module, jl_symbol("GIT_VERSION_INFO"));
    jl_value_t *f = jl_get_field(GIT_VERSION_INFO, fld);
    assert(jl_is_string(f));
    return jl_string_data(f);
}

JL_DLLEXPORT const char *jl_git_branch(void)
{
    static const char *branch = NULL;
    if (!branch) branch = git_info_string("branch");
    return branch;
}

JL_DLLEXPORT const char *jl_git_commit(void)
{
    static const char *commit = NULL;
    if (!commit) commit = git_info_string("commit");
    return commit;
}

// Create function versions of some useful macros for GDB or FFI use
JL_DLLEXPORT jl_taggedvalue_t *(jl_astaggedvalue)(jl_value_t *v)
{
    return jl_astaggedvalue(v);
}

JL_DLLEXPORT jl_value_t *(jl_valueof)(jl_taggedvalue_t *v)
{
    return jl_valueof(v);
}

JL_DLLEXPORT jl_value_t *(jl_typeof)(jl_value_t *v)
{
    return jl_typeof(v);
}

JL_DLLEXPORT jl_value_t *(jl_get_fieldtypes)(jl_value_t *v)
{
    return (jl_value_t*)jl_get_fieldtypes((jl_datatype_t*)v);
}

JL_DLLEXPORT int ijl_egal(jl_value_t *a, jl_value_t *b)
{
    return jl_egal(a, b);
}


#ifndef __clang_gcanalyzer__
JL_DLLEXPORT int8_t (jl_gc_unsafe_enter)(void)
{
    jl_task_t *ct = jl_current_task;
    return jl_gc_unsafe_enter(ct->ptls);
}

JL_DLLEXPORT void (jl_gc_unsafe_leave)(int8_t state)
{
    jl_task_t *ct = jl_current_task;
    jl_gc_unsafe_leave(ct->ptls, state);
}

JL_DLLEXPORT int8_t (jl_gc_safe_enter)(void)
{
    jl_task_t *ct = jl_current_task;
    return jl_gc_safe_enter(ct->ptls);
}

JL_DLLEXPORT void (jl_gc_safe_leave)(int8_t state)
{
    jl_task_t *ct = jl_current_task;
    jl_gc_safe_leave(ct->ptls, state);
}
#endif

JL_DLLEXPORT void jl_gc_safepoint(void)
{
    jl_task_t *ct = jl_current_task;
    jl_gc_safepoint_(ct->ptls);
}

JL_DLLEXPORT void (jl_cpu_pause)(void)
{
    jl_cpu_pause();
}

JL_DLLEXPORT void (jl_cpu_suspend)(void)
{
    jl_cpu_suspend();
}

JL_DLLEXPORT void (jl_cpu_wake)(void)
{
    jl_cpu_wake();
}

JL_DLLEXPORT void jl_cumulative_compile_timing_enable(void)
{
    // Increment the flag to allow reentrant callers to `@time`.
    jl_atomic_fetch_add(&jl_measure_compile_time_enabled, 1);
}

JL_DLLEXPORT void jl_cumulative_compile_timing_disable(void)
{
    // Decrement the flag when done measuring, allowing other callers to continue measuring.
    jl_atomic_fetch_add(&jl_measure_compile_time_enabled, -1);
}

JL_DLLEXPORT uint64_t jl_cumulative_compile_time_ns(void)
{
    return jl_atomic_load_relaxed(&jl_cumulative_compile_time);
}

JL_DLLEXPORT uint64_t jl_cumulative_recompile_time_ns(void)
{
    return jl_atomic_load_relaxed(&jl_cumulative_recompile_time);
}

/**
 * @brief Enable per-task timing.
 */
JL_DLLEXPORT void jl_task_metrics_enable(void)
{
    // Increment the flag to allow reentrant callers.
    jl_atomic_fetch_add(&jl_task_metrics_enabled, 1);
}

/**
 * @brief Disable per-task timing.
 */
JL_DLLEXPORT void jl_task_metrics_disable(void)
{
    // Prevent decrementing the counter below zero
    uint8_t enabled = jl_atomic_load_relaxed(&jl_task_metrics_enabled);
    while (enabled > 0) {
        if (jl_atomic_cmpswap(&jl_task_metrics_enabled, &enabled, enabled-1))
            break;
    }
}

/**
 * @brief Retrieve floating-point environment constants.
 *
 * Populates an array with constants related to the floating-point environment,
 * such as rounding modes and exception flags.
 *
 * @param ret An array of integers to be populated with floating-point environment constants.
 */
JL_DLLEXPORT void jl_get_fenv_consts(int *ret)
{
    ret[0] = FE_INEXACT;
    ret[1] = FE_UNDERFLOW;
    ret[2] = FE_OVERFLOW;
    ret[3] = FE_DIVBYZERO;
    ret[4] = FE_INVALID;
    ret[5] = FE_TONEAREST;
    ret[6] = FE_UPWARD;
    ret[7] = FE_DOWNWARD;
    ret[8] = FE_TOWARDZERO;
}

// TODO: Windows binaries currently load msvcrt which doesn't have these C99 functions.
//       the mingw compiler ships additional definitions, but only for use in C code.
//       remove this when we switch to ucrt, make the version in openlibm portable,
//       or figure out how to reexport the defs from libmingwex (see JuliaLang/julia#38466).
JL_DLLEXPORT int jl_get_fenv_rounding(void)
{
    return fegetround();
}
JL_DLLEXPORT int jl_set_fenv_rounding(int i)
{
    return fesetround(i);
}

static int exec_program(char *program)
{
    JL_TRY {
        jl_load(jl_main_module, program);
    }
    JL_CATCH {
        // TODO: It is possible for this output to be mangled due to `jl_print_backtrace`
        //       printing directly to STDERR_FILENO.
        int shown_err = 0;
        jl_printf(JL_STDERR, "error during bootstrap:\n");
        jl_value_t *exc = jl_current_exception();
        jl_value_t *showf = jl_base_module ? jl_get_function(jl_base_module, "show") : NULL;
        if (showf) {
            jl_value_t *errs = jl_stderr_obj();
            if (errs) {
                if (jl_call2(showf, errs, exc)) {
                    jl_printf(JL_STDERR, "\n");
                    shown_err = 1;
                }
            }
        }
        if (!shown_err) {
            jl_static_show((JL_STREAM*)STDERR_FILENO, exc);
            jl_printf((JL_STREAM*)STDERR_FILENO, "\n");
        }
        jl_print_backtrace(); // written to STDERR_FILENO
        jl_printf((JL_STREAM*)STDERR_FILENO, "\n");
        return 1;
    }
    return 0;
}

static NOINLINE int true_main(int argc, char *argv[])
{
    jl_set_ARGS(argc, argv);

    jl_function_t *start_client = jl_base_module ?
        (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("_start")) : NULL;

    if (start_client) {
        jl_task_t *ct = jl_current_task;
        JL_TRY {
            size_t last_age = ct->world_age;
            ct->world_age = jl_get_world_counter();
            jl_apply(&start_client, 1);
            ct->world_age = last_age;
        }
        JL_CATCH {
            jl_no_exc_handler(jl_current_exception(), ct);
        }
        return 0;
    }

    // run program if specified, otherwise enter REPL
    if (argc > 0) {
        if (strcmp(argv[0], "-")) {
            return exec_program(argv[0]);
        }
    }

    jl_printf(JL_STDOUT, "WARNING: Base._start not defined, falling back to economy mode repl.\n");
    if (!jl_errorexception_type)
        jl_printf(JL_STDOUT, "WARNING: jl_errorexception_type not defined; any errors will be fatal.\n");

    while (!ios_eof(ios_stdin)) {
        char *volatile line = NULL;
        JL_TRY {
            ios_puts("\njulia> ", ios_stdout);
            ios_flush(ios_stdout);
            line = ios_readline(ios_stdin);
            jl_value_t *val = (jl_value_t*)jl_eval_string(line);
            JL_GC_PUSH1(&val);
            if (jl_exception_occurred()) {
                jl_printf(JL_STDERR, "error during run:\n");
                jl_static_show(JL_STDERR, jl_exception_occurred());
                jl_exception_clear();
            }
            else if (val) {
                jl_static_show(JL_STDOUT, val);
            }
            JL_GC_POP();
            jl_printf(JL_STDOUT, "\n");
            free(line);
            line = NULL;
            jl_process_events();
        }
        JL_CATCH {
            if (line) {
                free(line);
                line = NULL;
            }
            jl_printf((JL_STREAM*)STDERR_FILENO, "\nparser error:\n");
            jl_static_show((JL_STREAM*)STDERR_FILENO, jl_current_exception());
            jl_printf((JL_STREAM*)STDERR_FILENO, "\n");
            jl_print_backtrace(); // written to STDERR_FILENO
        }
    }
    return 0;
}

static void lock_low32(void)
{
#if defined(_OS_WINDOWS_) && defined(_P64) && defined(JL_DEBUG_BUILD)
    // Prevent usage of the 32-bit address space on Win64, to catch pointer cast errors.
    char *const max32addr = (char*)0xffffffffL;
    SYSTEM_INFO info;
    MEMORY_BASIC_INFORMATION meminfo;
    GetNativeSystemInfo(&info);
    memset(&meminfo, 0, sizeof(meminfo));
    meminfo.BaseAddress = info.lpMinimumApplicationAddress;
    while ((char*)meminfo.BaseAddress < max32addr) {
        size_t nbytes = VirtualQuery(meminfo.BaseAddress, &meminfo, sizeof(meminfo));
        assert(nbytes == sizeof(meminfo));
        if (meminfo.State == MEM_FREE) { // reserve all free pages in the first 4GB of memory
            char *first = (char*)meminfo.BaseAddress;
            char *last = first + meminfo.RegionSize;
            if (last > max32addr)
                last = max32addr;
            // adjust first up to the first allocation granularity boundary
            // adjust last down to the last allocation granularity boundary
            first = (char*)(((long long)first + info.dwAllocationGranularity - 1) & ~(info.dwAllocationGranularity - 1));
            last = (char*)((long long)last & ~(info.dwAllocationGranularity - 1));
            if (last != first) {
                void *p = VirtualAlloc(first, last - first, MEM_RESERVE, PAGE_NOACCESS); // reserve all memory in between
                if ((char*)p != first)
                    // Wine and Windows10 seem to have issues with reporting memory access information correctly
                    // so we sometimes end up with unexpected results - this is just ignore those and continue
                    // this is just a debugging aid to help find accidental pointer truncation anyways,
                    // so it is not critical
                    VirtualFree(p, 0, MEM_RELEASE);
            }
        }
        meminfo.BaseAddress = (void*)((char*)meminfo.BaseAddress + meminfo.RegionSize);
    }
#endif
    return;
}

// Actual definition in `ast.c`
void jl_lisp_prompt(void);

#ifdef _OS_LINUX_
static void rr_detach_teleport(void) {
#define RR_CALL_BASE 1000
#define SYS_rrcall_detach_teleport (RR_CALL_BASE + 9)
    int err = syscall(SYS_rrcall_detach_teleport, 0, 0, 0, 0, 0, 0);
    if (err < 0 || jl_running_under_rr(1)) {
        jl_error("Failed to detach from rr session");
    }
}
#endif

JL_DLLEXPORT int jl_repl_entrypoint(int argc, char *argv[])
{
#ifdef USE_TRACY
    if (getenv("JULIA_WAIT_FOR_TRACY"))
        while (!TracyCIsConnected) jl_cpu_pause(); // Wait for connection
#endif

    // no-op on Windows, note that the caller must have already converted
    // from `wchar_t` to `UTF-8` already if we're running on Windows.
    uv_setup_args(argc, argv);

    // No-op on non-windows
    lock_low32();

    libsupport_init();
    int lisp_prompt = (argc >= 2 && strcmp((char*)argv[1],"--lisp") == 0);
    if (lisp_prompt) {
        memmove(&argv[1], &argv[2], (argc-2)*sizeof(void*));
        argc--;
    }
    char **new_argv = argv;
    jl_parse_opts(&argc, (char***)&new_argv);

    // The parent process requested that we detach from the rr session.
    // N.B.: In a perfect world, we would only do this for the portion of
    // the execution where we actually need to exclude rr (e.g. because we're
    // testing for the absence of a memory-model-dependent bug).
    if (jl_options.rr_detach && jl_running_under_rr(0)) {
#ifdef _OS_LINUX_
        rr_detach_teleport();
        execv("/proc/self/exe", argv);
#endif
        jl_error("Failed to self-execute");
    }

    julia_init(jl_options.image_file_specified ? JL_IMAGE_CWD : JL_IMAGE_JULIA_HOME);
    if (lisp_prompt) {
        jl_current_task->world_age = jl_get_world_counter();
        jl_lisp_prompt();
        return 0;
    }
    int ret = true_main(argc, (char**)new_argv);
    jl_atexit_hook(ret);
    return ret;
}

#ifdef __cplusplus
}
#endif
