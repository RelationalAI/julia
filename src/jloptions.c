// This file is a part of Julia. License is MIT: https://julialang.org/license

#include <limits.h>
#include <errno.h>

#include "julia.h"
#include "julia_internal.h"

#include <unistd.h>
#include <getopt.h>
#include "julia_assert.h"

#ifdef _OS_WINDOWS_
char *shlib_ext = ".dll";
#elif defined(_OS_DARWIN_)
char *shlib_ext = ".dylib";
#else
char *shlib_ext = ".so";
#endif

static const char system_image_path[256] = "\0" JL_SYSTEM_IMAGE_PATH;
JL_DLLEXPORT const char *jl_get_default_sysimg_path(void)
{
    return &system_image_path[1];
}

static int jl_options_initialized = 0;

JL_DLLEXPORT void jl_init_options(void)
{
    if (jl_options_initialized)
        return;
    jl_options =
        (jl_options_t){ 0,    // quiet
                        -1,   // banner
                        NULL, // julia_bindir
                        NULL, // julia_bin
                        NULL, // cmds
                        NULL, // image_file (will be filled in below)
                        NULL, // cpu_target ("native", "core2", etc...)
                        0,    // nthreadpools
                        0,    // nthreads
                        0,    // nmarkthreads
                        0,    // nsweepthreads
                        NULL, // nthreads_per_pool
                        0,    // nprocs
                        NULL, // machine_file
                        NULL, // project
                        0,    // isinteractive
                        0,    // color
                        JL_OPTIONS_HISTORYFILE_ON, // history file
                        0,    // startup file
                        JL_OPTIONS_COMPILE_DEFAULT, // compile_enabled
                        0,    // code_coverage
                        0,    // malloc_log
                        NULL, // tracked_path
                        2,    // opt_level
                        0,    // opt_level_min
#ifdef JL_DEBUG_BUILD
                        2,    // debug_level [debug build]
#else
                        1,    // debug_level [release build]
#endif
                        JL_OPTIONS_CHECK_BOUNDS_DEFAULT, // check_bounds
                        JL_OPTIONS_DEPWARN_OFF,    // deprecation warning
                        0,    // method overwrite warning
                        1,    // can_inline
                        JL_OPTIONS_POLLY_ON, // polly
                        NULL, // trace_compile
                        NULL, // trace_dispatch
                        JL_OPTIONS_FAST_MATH_DEFAULT,
                        0,    // worker
                        NULL, // cookie
                        JL_OPTIONS_HANDLE_SIGNALS_ON,
                        JL_OPTIONS_USE_SYSIMAGE_NATIVE_CODE_YES,
                        JL_OPTIONS_USE_COMPILED_MODULES_YES,
                        JL_OPTIONS_USE_PKGIMAGES_YES,
                        NULL, // bind-to
                        NULL, // output-bc
                        NULL, // output-unopt-bc
                        NULL, // output-o
                        NULL, // output-asm
                        NULL, // output-ji
                        NULL, // output-code_coverage
                        0, // incremental
                        0, // image_file_specified
                        JL_OPTIONS_WARN_SCOPE_ON,  // ambiguous scope warning
                        0, // image-codegen
                        0, // rr-detach
                        0, // strip-metadata
                        0, // strip-ir
                        0, // permalloc_pkgimg
                        0, // heap-size-hint
                        0, // trace_compile_timing
                        NULL, // safe_crash_log_file
                        0, // task_metrics
                        25, // timeout_for_safepoint_straggler_s
    };
    jl_options_initialized = 1;
}

static const char usage[] = "\n    julia [switches] -- [programfile] [args...]\n\n";
static const char opts[]  =
    "Switches (a '*' marks the default value, if applicable; settings marked '($)' may trigger package precompilation):\n\n"
    " -v, --version              Display version information\n"
    " -h, --help                 Print this message (--help-hidden for more)\n"
    " --help-hidden              Uncommon options not shown by `-h`\n\n"

    // startup options
    " --project[={<dir>|@.}]     Set <dir> as the home project/environment\n"
    " -J, --sysimage <file>      Start up with the given system image file\n"
    " -H, --home <dir>           Set location of `julia` executable\n"
    " --startup-file={yes*|no}   Load `JULIA_DEPOT_PATH/config/startup.jl`; if `JULIA_DEPOT_PATH`\n"
    "                            environment variable is unset, load `~/.julia/config/startup.jl`\n"
    " --handle-signals={yes*|no} Enable or disable Julia's default signal handlers\n"
    " --sysimage-native-code={yes*|no}\n"
    "                            Use native code from system image if available\n"
    " --compiled-modules={yes*|no}\n"
    "                            Enable or disable incremental precompilation of modules\n"
    " --pkgimages={yes*|no}\n"
    "                            Enable or disable usage of native code caching in the form of pkgimages ($)\n\n"

    // actions
    " -e, --eval <expr>          Evaluate <expr>\n"
    " -E, --print <expr>         Evaluate <expr> and display the result\n"
    " -L, --load <file>          Load <file> immediately on all processors\n\n"

    // parallel options
    " -t, --threads {auto|N[,auto|M]}\n"
    "                           Enable N[+M] threads; N threads are assigned to the `default`\n"
    "                           threadpool, and if M is specified, M threads are assigned to the\n"
    "                           `interactive` threadpool; \"auto\" tries to infer a useful\n"
    "                           default number of threads to use but the exact behavior might change\n"
    "                           in the future. Currently sets N to the number of CPUs assigned to\n"
    "                           this Julia process based on the OS-specific affinity assignment\n"
    "                           interface if supported (Linux and Windows) or to the number of CPU\n"
    "                           threads if not supported (MacOS) or if process affinity is not\n"
    "                           configured, and sets M to 1.\n"
    " --gcthreads=N[,M]         Use N threads for the mark phase of GC and M (0 or 1) threads for the concurrent sweeping phase of GC.\n"
    "                           N is set to half of the number of compute threads and M is set to 0 if unspecified.\n"
    " -p, --procs {N|auto}      Integer value N launches N additional local worker processes\n"
    "                           \"auto\" launches as many workers as the number of local CPU threads (logical cores)\n"
    " --machine-file <file>     Run processes on hosts listed in <file>\n\n"

    // interactive options
    " -i, --interactive          Interactive mode; REPL runs and `isinteractive()` is true\n"
    " -q, --quiet                Quiet startup: no banner, suppress REPL warnings\n"
    " --banner={yes|no|auto*}    Enable or disable startup banner\n"
    " --color={yes|no|auto*}     Enable or disable color text\n"
    " --history-file={yes*|no}   Load or save history\n\n"

    // error and warning options
    " --depwarn={yes|no*|error}  Enable or disable syntax and method deprecation warnings (`error` turns warnings into errors)\n"
    " --warn-overwrite={yes|no*} Enable or disable method overwrite warnings\n"
    " --warn-scope={yes*|no}     Enable or disable warning for ambiguous top-level scope\n\n"

    // code generation options
    " -C, --cpu-target <target>  Limit usage of CPU features up to <target>; set to `help` to see the available options\n"
    " -O, --optimize={0,1,2*,3}  Set the optimization level (level 3 if `-O` is used without a level) ($)\n"
    " --min-optlevel={0*,1,2,3}  Set a lower bound on the optimization level\n"
#ifdef JL_DEBUG_BUILD
        " -g, --debug-info=[{0,1,2*}] Set the level of debug info generation in the julia-debug build ($)\n"
#else
        " -g, --debug-info=[{0,1*,2}] Set the level of debug info generation (level 2 if `-g` is used without a level) ($)\n"
#endif
    " --inline={yes*|no}         Control whether inlining is permitted, including overriding @inline declarations\n"
    " --check-bounds={yes|no|auto*}\n"
    "                            Emit bounds checks always, never, or respect @inbounds declarations ($)\n"
#ifdef USE_POLLY
    " --polly={yes*|no}          Enable or disable the polyhedral optimizer Polly (overrides @polly declaration)\n"
#endif

    // instrumentation options
    " --code-coverage[={none*|user|all}]\n"
    "                            Count executions of source lines (omitting setting is equivalent to `user`)\n"
    " --code-coverage=@<path>\n"
    "                            Count executions but only in files that fall under the given file path/directory.\n"
    "                            The `@` prefix is required to select this option. A `@` with no path will track the\n"
    "                            current directory.\n"

    " --code-coverage=tracefile.info\n"
    "                            Append coverage information to the LCOV tracefile (filename supports format tokens)\n"
// TODO: These TOKENS are defined in `runtime_ccall.cpp`. A more verbose `--help` should include that list here.
    " --track-allocation[={none*|user|all}]\n"
    "                            Count bytes allocated by each source line (omitting setting is equivalent to `user`)\n"
    " --track-allocation=@<path>\n"
    "                            Count bytes but only in files that fall under the given file path/directory.\n"
    "                            The `@` prefix is required to select this option. A `@` with no path will track the\n"
    "                            current directory.\n"
    " --bug-report=KIND          Launch a bug report session. It can be used to start a REPL, run a script, or evaluate\n"
    "                            expressions. It first tries to use BugReporting.jl installed in current environment and\n"
    "                            fallbacks to the latest compatible BugReporting.jl if not. For more information, see\n"
    "                            --bug-report=help.\n\n"

    " --heap-size-hint=<size>    Forces garbage collection if memory usage is higher than that value.\n"
    "                            The memory hint might be specified in megabytes(500M) or gigabytes(1G)\n\n"
;

static const char opts_hidden[]  =
    "Switches (a '*' marks the default value, if applicable):\n\n"
    // code generation options
    " --compile={yes*|no|all|min}\n"
    "                          Enable or disable JIT compiler, or request exhaustive or minimal compilation\n\n"

    // compiler output options
    " --output-o <name>        Generate an object file (including system image data)\n"
    " --output-ji <name>       Generate a system image data file (.ji)\n"
    " --strip-metadata         Remove docstrings and source location info from system image\n"
    " --strip-ir               Remove IR (intermediate representation) of compiled functions\n\n"

    // compiler debugging and experimental (see the devdocs for tips on using these options)
    " --experimental                                Enable the use of experimental (alpha) features\n"
    " --output-unopt-bc <name>                      Generate unoptimized LLVM bitcode (.bc)\n"
    " --output-bc <name>                            Generate LLVM bitcode (.bc)\n"
    " --output-asm <name>                           Generate an assembly file (.s)\n"
    " --output-incremental={yes|no*}                Generate an incremental output file (rather than\n"
    "                                               complete)\n"
    " --timeout-for-safepoint-straggler <seconds>   If this value is set, then we will dump the backtrace for a thread\n"
    "                                               that fails to reach a safepoint within the specified time\n"
    " --trace-compile={stderr|name}                 Print precompile statements for methods compiled\n"
    "                                               during execution or save to stderr or a path. Methods that\n"
    "                                               were recompiled are printed in yellow or with a trailing\n"
    "                                               comment if color is not supported\n"
    " --trace-compile-timing                        If --trace-compile is enabled show how long each took to\n"
    "                                               compile in ms\n"
    " --task-metrics={yes|no*}                      Enable collection of per-task timing data.\n"
    " --image-codegen                               Force generate code in imaging mode\n"
    " --permalloc-pkgimg={yes|no*}                  Copy the data section of package images into memory\n"
;

JL_DLLEXPORT void jl_parse_opts(int *argcp, char ***argvp)
{
    enum { opt_machinefile = 300,
           opt_color,
           opt_history_file,
           opt_startup_file,
           opt_compile,
           opt_code_coverage,
           opt_track_allocation,
           opt_check_bounds,
           opt_output_unopt_bc,
           opt_output_bc,
           opt_depwarn,
           opt_warn_overwrite,
           opt_warn_scope,
           opt_inline,
           opt_polly,
           opt_timeout_for_safepoint_straggler,
           opt_trace_compile,
           opt_trace_compile_timing,
           opt_trace_dispatch,
           opt_task_metrics,
           opt_math_mode,
           opt_worker,
           opt_bind_to,
           opt_handle_signals,
           opt_optlevel_min,
           opt_output_o,
           opt_output_asm,
           opt_output_ji,
           opt_use_precompiled,
           opt_use_compilecache,
           opt_incremental,
           opt_help_hidden,
           opt_banner,
           opt_sysimage_native_code,
           opt_compiled_modules,
           opt_pkgimages,
           opt_machine_file,
           opt_project,
           opt_bug_report,
           opt_image_codegen,
           opt_rr_detach,
           opt_strip_metadata,
           opt_strip_ir,
           opt_heap_size_hint,
           opt_gc_threads,
           opt_permalloc_pkgimg,
           opt_safe_crash_log_file,
    };
    static const char* const shortopts = "+vhqH:e:E:L:J:C:it:p:O:g:";
    static const struct option longopts[] = {
        // exposed command line options
        // NOTE: This set of required arguments need to be kept in sync
        // with the required arguments defined in base/options.jl `struct JLOptions`
        { "version",         no_argument,       0, 'v' },
        { "help",            no_argument,       0, 'h' },
        { "help-hidden",     no_argument,       0, opt_help_hidden },
        { "interactive",     no_argument,       0, 'i' },
        { "quiet",           no_argument,       0, 'q' },
        { "banner",          required_argument, 0, opt_banner },
        { "home",            required_argument, 0, 'H' },
        { "eval",            required_argument, 0, 'e' },
        { "print",           required_argument, 0, 'E' },
        { "load",            required_argument, 0, 'L' },
        { "bug-report",      required_argument, 0, opt_bug_report },
        { "sysimage",        required_argument, 0, 'J' },
        { "sysimage-native-code", required_argument, 0, opt_sysimage_native_code },
        { "compiled-modules",required_argument, 0, opt_compiled_modules },
        { "pkgimages",       required_argument, 0, opt_pkgimages },
        { "cpu-target",      required_argument, 0, 'C' },
        { "procs",           required_argument, 0, 'p' },
        { "threads",         required_argument, 0, 't' },
        { "gcthreads",       required_argument, 0, opt_gc_threads },
        { "machine-file",    required_argument, 0, opt_machine_file },
        { "project",         optional_argument, 0, opt_project },
        { "color",           required_argument, 0, opt_color },
        { "history-file",    required_argument, 0, opt_history_file },
        { "startup-file",    required_argument, 0, opt_startup_file },
        { "compile",         required_argument, 0, opt_compile },
        { "code-coverage",   optional_argument, 0, opt_code_coverage },
        { "track-allocation",optional_argument, 0, opt_track_allocation },
        { "optimize",        optional_argument, 0, 'O' },
        { "min-optlevel",    optional_argument, 0, opt_optlevel_min },
        { "debug-info",      optional_argument, 0, 'g' },
        { "check-bounds",    required_argument, 0, opt_check_bounds },
        { "output-bc",       required_argument, 0, opt_output_bc },
        { "output-unopt-bc", required_argument, 0, opt_output_unopt_bc },
        { "output-o",        required_argument, 0, opt_output_o },
        { "output-asm",      required_argument, 0, opt_output_asm },
        { "output-ji",       required_argument, 0, opt_output_ji },
        { "output-incremental",required_argument, 0, opt_incremental },
        { "depwarn",         required_argument, 0, opt_depwarn },
        { "warn-overwrite",  required_argument, 0, opt_warn_overwrite },
        { "warn-scope",      required_argument, 0, opt_warn_scope },
        { "inline",          required_argument, 0, opt_inline },
        { "polly",           required_argument, 0, opt_polly },
        { "timeout-for-safepoint-straggler", required_argument, 0, opt_timeout_for_safepoint_straggler },
        { "trace-compile",   required_argument, 0, opt_trace_compile },
        { "trace-compile-timing",  no_argument, 0, opt_trace_compile_timing },
        { "trace-dispatch",  required_argument, 0, opt_trace_dispatch },
        { "task-metrics",    required_argument, 0, opt_task_metrics },
        { "math-mode",       required_argument, 0, opt_math_mode },
        { "handle-signals",  required_argument, 0, opt_handle_signals },
        // hidden command line options
        { "worker",          optional_argument, 0, opt_worker },
        { "bind-to",         required_argument, 0, opt_bind_to },
        { "lisp",            no_argument,       0, 1 },
        { "image-codegen",   no_argument,       0, opt_image_codegen },
        { "rr-detach",       no_argument,       0, opt_rr_detach },
        { "strip-metadata",  no_argument,       0, opt_strip_metadata },
        { "strip-ir",        no_argument,       0, opt_strip_ir },
        { "permalloc-pkgimg",required_argument, 0, opt_permalloc_pkgimg },
        { "heap-size-hint",  required_argument, 0, opt_heap_size_hint },
        { "safe-crash-log-file",   required_argument, 0, opt_safe_crash_log_file },
        { 0, 0, 0, 0 }
    };

    // If CPUID specific binaries are enabled, this varies between runs, so initialize
    // it here, rather than as part of the static initialization above.
    jl_options.image_file = jl_get_default_sysimg_path();
    jl_options.cmds = NULL;

    int ncmds = 0;
    const char **cmds = NULL;
    int codecov = JL_LOG_NONE;
    int malloclog = JL_LOG_NONE;
    int pkgimage_explicit = 0;
    int argc = *argcp;
    char **argv = *argvp;
    char *endptr;
    opterr = 0; // suppress getopt warning messages
    while (1) {
        int lastind = optind;
        int c = getopt_long(argc, argv, shortopts, longopts, 0);
        if (c == -1) break;
restart_switch:
        switch (c) {
        case 0:
            break;
        case 1:
            jl_errorf("--lisp must be specified as the first argument");
            break;
        case '?':
        case ':':
            if (optopt) {
                if (optopt == 'g') {
                    c = 'g';
                    goto restart_switch;
                }
                for (const struct option *o = longopts; o->val; o++) {
                    if (optopt == o->val) {
                        if (o->has_arg == optional_argument) {
                            c = o->val;
                            goto restart_switch;
                        }
                        else {
                            const char *problem = o->has_arg ? "is missing an argument" : "does not accept an argument";
                            if (o->val <= 0xff && strchr(shortopts, o->val)) {
                                jl_errorf("option `-%c/--%s` %s", o->val, o->name, problem);
                            }
                            else {
                                jl_errorf("option `--%s` %s", o->name, problem);
                            }
                        }
                    }
                }
                jl_errorf("unknown option `-%c`", optopt);
            }
            else {
                jl_errorf("unknown option `%s`", argv[lastind]);
            }
            break;
        case 'v': // version
            jl_printf(JL_STDOUT, "julia version %s\n", JULIA_VERSION_STRING);
            exit(0);
        case 'h': // help
            jl_printf(JL_STDOUT, "%s%s", usage, opts);
            exit(0);
        case opt_help_hidden:
            jl_printf(JL_STDOUT, "%s%s", usage, opts_hidden);
            exit(0);
        case 'g': // debug info
            if (optarg != NULL) {
                if (!strcmp(optarg,"0"))
                    jl_options.debug_level = 0;
                else if (!strcmp(optarg,"1"))
                    jl_options.debug_level = 1;
                else if (!strcmp(optarg,"2"))
                    jl_options.debug_level = 2;
                else
                    jl_errorf("julia: invalid argument to -g (%s)", optarg);
                break;
            }
            else {
                jl_options.debug_level = 2;
            }
            break;
        case 'H': // home
            jl_options.julia_bindir = strdup(optarg);
            if (!jl_options.julia_bindir)
                jl_errorf("fatal error: failed to allocate memory: %s", strerror(errno));
            break;
        case 'e': // eval
        case 'E': // print
        case 'L': // load
        case opt_bug_report: // bug
        {
            size_t sz = strlen(optarg) + 1;
            char *arg = (char*)malloc_s(sz + 1);
            const char **newcmds;
            arg[0] = c == opt_bug_report ? 'B' : c;
            memcpy(arg + 1, optarg, sz);
            newcmds = (const char**)realloc_s(cmds, (ncmds + 2) * sizeof(char*));
            cmds = newcmds;
            cmds[ncmds] = arg;
            ncmds++;
            cmds[ncmds] = 0;
            jl_options.cmds = cmds;
            break;
        }
        case 'J': // sysimage
            jl_options.image_file = strdup(optarg);
            if (!jl_options.image_file)
                jl_errorf("fatal error: failed to allocate memory: %s", strerror(errno));
            jl_options.image_file_specified = 1;
            break;
        case 'q': // quiet
            jl_options.quiet = 1;
            if (jl_options.banner < 0)
                jl_options.banner = 0;
            break;
        case opt_banner: // banner
            if (!strcmp(optarg, "yes"))
                jl_options.banner = 1;
            else if (!strcmp(optarg, "no"))
                jl_options.banner = 0;
            else if (!strcmp(optarg, "auto"))
                jl_options.banner = -1;
            else
                jl_errorf("julia: invalid argument to --banner={yes|no|auto} (%s)", optarg);
            break;
        case opt_sysimage_native_code:
            if (!strcmp(optarg,"yes"))
                jl_options.use_sysimage_native_code = JL_OPTIONS_USE_SYSIMAGE_NATIVE_CODE_YES;
            else if (!strcmp(optarg,"no"))
                jl_options.use_sysimage_native_code = JL_OPTIONS_USE_SYSIMAGE_NATIVE_CODE_NO;
            else
                jl_errorf("julia: invalid argument to --sysimage-native-code={yes|no} (%s)", optarg);
            break;
        case opt_compiled_modules:
            if (!strcmp(optarg,"yes"))
                jl_options.use_compiled_modules = JL_OPTIONS_USE_COMPILED_MODULES_YES;
            else if (!strcmp(optarg,"no"))
                jl_options.use_compiled_modules = JL_OPTIONS_USE_COMPILED_MODULES_NO;
            else
                jl_errorf("julia: invalid argument to --compiled-modules={yes|no} (%s)", optarg);
            break;
        case opt_pkgimages:
            pkgimage_explicit = 1;
            if (!strcmp(optarg,"yes"))
                jl_options.use_pkgimages = JL_OPTIONS_USE_PKGIMAGES_YES;
            else if (!strcmp(optarg,"no"))
                jl_options.use_pkgimages = JL_OPTIONS_USE_PKGIMAGES_NO;
            else
                jl_errorf("julia: invalid argument to --pkgimage={yes|no} (%s)", optarg);
            break;
        case 'C': // cpu-target
            jl_options.cpu_target = strdup(optarg);
            if (!jl_options.cpu_target)
                jl_error("julia: failed to allocate memory");
            break;
        case 't': // threads
            errno = 0;
            jl_options.nthreadpools = 1;
            long nthreads = -1, nthreadsi = 0;
            if (!strncmp(optarg, "auto", 4)) {
                jl_options.nthreads = -1;
                if (optarg[4] == ',') {
                    if (!strncmp(&optarg[5], "auto", 4))
                        nthreadsi = 1;
                    else {
                        errno = 0;
                        nthreadsi = strtol(&optarg[5], &endptr, 10);
                        if (errno != 0 || endptr == &optarg[5] || *endptr != 0 || nthreadsi < 1 || nthreadsi >= INT16_MAX)
                            jl_errorf("julia: -t,--threads=auto,<m>; m must be an integer >= 1");
                    }
                    jl_options.nthreadpools++;
                }
            }
            else {
                nthreads = strtol(optarg, &endptr, 10);
                if (errno != 0 || optarg == endptr || nthreads < 1 || nthreads >= INT16_MAX)
                    jl_errorf("julia: -t,--threads=<n>[,auto|<m>]; n must be an integer >= 1");
                if (*endptr == ',') {
                    if (!strncmp(&endptr[1], "auto", 4))
                        nthreadsi = 1;
                    else {
                        errno = 0;
                        char *endptri;
                        nthreadsi = strtol(&endptr[1], &endptri, 10);
                        if (errno != 0 || endptri == &endptr[1] || *endptri != 0 || nthreadsi < 1 || nthreadsi >= INT16_MAX)
                            jl_errorf("julia: -t,--threads=<n>,<m>; n and m must be integers >= 1");
                    }
                    jl_options.nthreadpools++;
                }
                jl_options.nthreads = nthreads + nthreadsi;
            }
            int16_t *ntpp = (int16_t *)malloc_s(jl_options.nthreadpools * sizeof(int16_t));
            ntpp[0] = (int16_t)nthreads;
            if (jl_options.nthreadpools == 2)
                ntpp[1] = (int16_t)nthreadsi;
            jl_options.nthreads_per_pool = ntpp;
            break;
        case 'p': // procs
            errno = 0;
            if (!strcmp(optarg,"auto")) {
                jl_options.nprocs = jl_effective_threads();
            }
            else {
                long nprocs = strtol(optarg, &endptr, 10);
                if (errno != 0 || optarg == endptr || *endptr != 0 || nprocs < 1 || nprocs >= INT16_MAX)
                    jl_errorf("julia: -p,--procs=<n> must be an integer >= 1");
                jl_options.nprocs = (int)nprocs;
            }
            break;
        case opt_machine_file:
            jl_options.machine_file = strdup(optarg);
            if (!jl_options.machine_file)
                jl_error("julia: failed to allocate memory");
            break;
        case opt_project:
            jl_options.project = optarg ? strdup(optarg) : "@.";
            break;
        case opt_color:
            if (!strcmp(optarg, "yes"))
                jl_options.color = JL_OPTIONS_COLOR_ON;
            else if (!strcmp(optarg, "no"))
                jl_options.color = JL_OPTIONS_COLOR_OFF;
            else if (!strcmp(optarg, "auto"))
                jl_options.color = JL_OPTIONS_COLOR_AUTO;
            else
                jl_errorf("julia: invalid argument to --color={yes|no|auto} (%s)", optarg);
            break;
        case opt_history_file:
            if (!strcmp(optarg,"yes"))
                jl_options.historyfile = JL_OPTIONS_HISTORYFILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.historyfile = JL_OPTIONS_HISTORYFILE_OFF;
            else
                jl_errorf("julia: invalid argument to --history-file={yes|no} (%s)", optarg);
            break;
        case opt_startup_file:
            if (!strcmp(optarg,"yes"))
                jl_options.startupfile = JL_OPTIONS_STARTUPFILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.startupfile = JL_OPTIONS_STARTUPFILE_OFF;
            else
                jl_errorf("julia: invalid argument to --startup-file={yes|no} (%s)", optarg);
            break;
        case opt_compile:
            if (!strcmp(optarg,"yes"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_OFF;
            else if (!strcmp(optarg,"all"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_ALL;
            else if (!strcmp(optarg,"min"))
                jl_options.compile_enabled = JL_OPTIONS_COMPILE_MIN;
            else
                jl_errorf("julia: invalid argument to --compile (%s)", optarg);
            break;
        case opt_code_coverage:
            if (optarg != NULL) {
                size_t endof = strlen(optarg);
                if (!strcmp(optarg, "user"))
                    codecov = JL_LOG_USER;
                else if (!strcmp(optarg, "all"))
                    codecov = JL_LOG_ALL;
                else if (!strcmp(optarg, "none"))
                    codecov = JL_LOG_NONE;
                else if (endof > 5 && !strcmp(optarg + endof - 5, ".info")) {
                    if (codecov == JL_LOG_NONE)
                        codecov = JL_LOG_ALL;
                    jl_options.output_code_coverage = optarg;
                }
                else if (!strncmp(optarg, "@", 1)) {
                    codecov = JL_LOG_PATH;
                    jl_options.tracked_path = optarg + 1; // skip `@`
                }
                else
                    jl_errorf("julia: invalid argument to --code-coverage (%s)", optarg);
                break;
            }
            else {
                codecov = JL_LOG_USER;
            }
            break;
        case opt_track_allocation:
            if (optarg != NULL) {
                if (!strcmp(optarg,"user"))
                    malloclog = JL_LOG_USER;
                else if (!strcmp(optarg,"all"))
                    malloclog = JL_LOG_ALL;
                else if (!strcmp(optarg,"none"))
                    malloclog = JL_LOG_NONE;
                else if (!strncmp(optarg, "@", 1)) {
                    malloclog = JL_LOG_PATH;
                    jl_options.tracked_path = optarg + 1; // skip `@`
                }
                else
                    jl_errorf("julia: invalid argument to --track-allocation (%s)", optarg);
                break;
            }
            else {
                malloclog = JL_LOG_USER;
            }
            break;
        case 'O': // optimize
            if (optarg != NULL) {
                if (!strcmp(optarg,"0"))
                    jl_options.opt_level = 0;
                else if (!strcmp(optarg,"1"))
                    jl_options.opt_level = 1;
                else if (!strcmp(optarg,"2"))
                    jl_options.opt_level = 2;
                else if (!strcmp(optarg,"3"))
                    jl_options.opt_level = 3;
                else
                    jl_errorf("julia: invalid argument to -O (%s)", optarg);
                break;
            }
            else {
                jl_options.opt_level = 3;
            }
            break;
        case opt_optlevel_min: // minimum module optimize level
            if (optarg != NULL) {
                if (!strcmp(optarg,"0"))
                    jl_options.opt_level_min = 0;
                else if (!strcmp(optarg,"1"))
                    jl_options.opt_level_min = 1;
                else if (!strcmp(optarg,"2"))
                    jl_options.opt_level_min = 2;
                else if (!strcmp(optarg,"3"))
                    jl_options.opt_level_min = 3;
                else
                    jl_errorf("julia: invalid argument to --min-optlevel (%s)", optarg);
                break;
            }
            else {
                jl_options.opt_level_min = 0;
            }
            break;
        case 'i': // isinteractive
            jl_options.isinteractive = 1;
            break;
        case opt_check_bounds:
            if (!strcmp(optarg,"yes"))
                jl_options.check_bounds = JL_OPTIONS_CHECK_BOUNDS_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.check_bounds = JL_OPTIONS_CHECK_BOUNDS_OFF;
            else if (!strcmp(optarg,"auto"))
                jl_options.check_bounds = JL_OPTIONS_CHECK_BOUNDS_DEFAULT;
            else
                jl_errorf("julia: invalid argument to --check-bounds={yes|no|auto} (%s)", optarg);
            break;
        case opt_output_bc:
            jl_options.outputbc = optarg;
            if (!jl_options.image_file_specified) jl_options.image_file = NULL;
            break;
        case opt_output_unopt_bc:
            jl_options.outputunoptbc = optarg;
            if (!jl_options.image_file_specified) jl_options.image_file = NULL;
            break;
        case opt_output_o:
            jl_options.outputo = optarg;
            if (!jl_options.image_file_specified) jl_options.image_file = NULL;
            break;
        case opt_output_asm:
            jl_options.outputasm = optarg;
            if (!jl_options.image_file_specified) jl_options.image_file = NULL;
            break;
        case opt_output_ji:
            jl_options.outputji = optarg;
            if (!jl_options.image_file_specified) jl_options.image_file = NULL;
            break;
        case opt_incremental:
            if (!strcmp(optarg,"yes"))
                jl_options.incremental = 1;
            else if (!strcmp(optarg,"no"))
                jl_options.incremental = 0;
            else
                jl_errorf("julia: invalid argument to --output-incremental={yes|no} (%s)", optarg);
            break;
        case opt_depwarn:
            if (!strcmp(optarg,"yes"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_OFF;
            else if (!strcmp(optarg,"error"))
                jl_options.depwarn = JL_OPTIONS_DEPWARN_ERROR;
            else
                jl_errorf("julia: invalid argument to --depwarn={yes|no|error} (%s)", optarg);
            break;
        case opt_warn_overwrite:
            if (!strcmp(optarg,"yes"))
                jl_options.warn_overwrite = JL_OPTIONS_WARN_OVERWRITE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.warn_overwrite = JL_OPTIONS_WARN_OVERWRITE_OFF;
            else
                jl_errorf("julia: invalid argument to --warn-overwrite={yes|no} (%s)", optarg);
            break;
        case opt_warn_scope:
            if (!strcmp(optarg,"yes"))
                jl_options.warn_scope = JL_OPTIONS_WARN_SCOPE_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.warn_scope = JL_OPTIONS_WARN_SCOPE_OFF;
            else
                jl_errorf("julia: invalid argument to --warn-scope={yes|no} (%s)", optarg);
            break;
        case opt_inline:
            if (!strcmp(optarg,"yes"))
                jl_options.can_inline = 1;
            else if (!strcmp(optarg,"no"))
                jl_options.can_inline = 0;
            else {
                jl_errorf("julia: invalid argument to --inline (%s)", optarg);
            }
            break;
        case opt_polly:
            if (!strcmp(optarg,"yes"))
                jl_options.polly = JL_OPTIONS_POLLY_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.polly = JL_OPTIONS_POLLY_OFF;
            else {
                jl_errorf("julia: invalid argument to --polly (%s)", optarg);
            }
            break;
        case opt_trace_compile:
            jl_options.trace_compile = strdup(optarg);
            if (!jl_options.trace_compile)
                jl_errorf("fatal error: failed to allocate memory: %s", strerror(errno));
            break;
        case opt_trace_compile_timing:
            jl_options.trace_compile_timing = 1;
            break;
        case opt_trace_dispatch:
            jl_options.trace_dispatch = strdup(optarg);
            if (!jl_options.trace_dispatch)
                jl_errorf("fatal error: failed to allocate memory: %s", strerror(errno));
            break;
        case opt_math_mode:
            if (!strcmp(optarg,"ieee"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_OFF;
            else if (!strcmp(optarg,"fast"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_DEFAULT;
            else if (!strcmp(optarg,"user"))
                jl_options.fast_math = JL_OPTIONS_FAST_MATH_DEFAULT;
            else
                jl_errorf("julia: invalid argument to --math-mode (%s)", optarg);
            break;
        case opt_worker:
            jl_options.worker = 1;
            if (optarg != NULL) {
                jl_options.cookie = strdup(optarg);
                if (!jl_options.cookie)
                    jl_error("julia: failed to allocate memory");
            }
            break;
        case opt_bind_to:
            jl_options.bindto = strdup(optarg);
            if (!jl_options.bindto)
                jl_error("julia: failed to allocate memory");
            break;
        case opt_handle_signals:
            if (!strcmp(optarg,"yes"))
                jl_options.handle_signals = JL_OPTIONS_HANDLE_SIGNALS_ON;
            else if (!strcmp(optarg,"no"))
                jl_options.handle_signals = JL_OPTIONS_HANDLE_SIGNALS_OFF;
            else
                jl_errorf("julia: invalid argument to --handle-signals (%s)", optarg);
            break;
        case opt_image_codegen:
            jl_options.image_codegen = 1;
            break;
        case opt_rr_detach:
            jl_options.rr_detach = 1;
            break;
        case opt_strip_metadata:
            jl_options.strip_metadata = 1;
            break;
        case opt_strip_ir:
            jl_options.strip_ir = 1;
            break;
        case opt_heap_size_hint:
            if (optarg != NULL) {
                size_t endof = strlen(optarg);
                long double value = 0.0;
                if (sscanf(optarg, "%Lf", &value) == 1 && value > 1e-7) {
                    char unit = optarg[endof - 1];
                    uint64_t multiplier = 1ull;
                    switch (unit) {
                        case 'k':
                        case 'K':
                            multiplier <<= 10;
                            break;
                        case 'm':
                        case 'M':
                            multiplier <<= 20;
                            break;
                        case 'g':
                        case 'G':
                            multiplier <<= 30;
                            break;
                        case 't':
                        case 'T':
                            multiplier <<= 40;
                            break;
                        default:
                            break;
                    }
                    jl_options.heap_size_hint = (uint64_t)(value * multiplier);
                }
            }
            if (jl_options.heap_size_hint == 0)
                jl_errorf("julia: invalid argument to --heap-size-hint without memory size specified");

            break;
        case opt_gc_threads:
            errno = 0;
            long nmarkthreads = strtol(optarg, &endptr, 10);
            if (errno != 0 || optarg == endptr || nmarkthreads < 1 || nmarkthreads >= INT16_MAX) {
                jl_errorf("julia: --gcthreads=<n>[,<m>]; n must be an integer >= 1");
            }
            jl_options.nmarkthreads = (int16_t)nmarkthreads;
            if (*endptr == ',') {
                errno = 0;
                char *endptri;
                long nsweepthreads = strtol(&endptr[1], &endptri, 10);
                if (errno != 0 || endptri == &endptr[1] || *endptri != 0 || nsweepthreads < 0 || nsweepthreads > 1)
                    jl_errorf("julia: --gcthreads=<n>,<m>; m must be 0 or 1");
                jl_options.nsweepthreads = (int8_t)nsweepthreads;
            }
            break;
        case opt_permalloc_pkgimg:
            if (!strcmp(optarg,"yes"))
                jl_options.permalloc_pkgimg = 1;
            else if (!strcmp(optarg,"no"))
                jl_options.permalloc_pkgimg = 0;
            else
                jl_errorf("julia: invalid argument to --permalloc-pkgimg={yes|no} (%s)", optarg);
            break;
        case opt_safe_crash_log_file:
            jl_options.safe_crash_log_file = strdup(optarg);
            if (jl_options.safe_crash_log_file == NULL)
                jl_error("julia: failed to allocate memory for --safe-crash-log-file");
            break;
        case opt_timeout_for_safepoint_straggler:
            errno = 0;
            long timeout = strtol(optarg, &endptr, 10);
            if (errno != 0 || optarg == endptr || timeout < 1 || timeout > INT16_MAX)
                jl_errorf("julia: --timeout-for-safepoint-straggler=<seconds>; seconds must be an integer between 1 and %d", INT16_MAX);
            jl_options.timeout_for_safepoint_straggler_s = (int16_t)timeout;
            break;
        case opt_task_metrics:
            if (!strcmp(optarg, "no"))
                jl_options.task_metrics = JL_OPTIONS_TASK_METRICS_OFF;
            else if (!strcmp(optarg, "yes"))
                jl_options.task_metrics = JL_OPTIONS_TASK_METRICS_ON;
            else
                jl_errorf("julia: invalid argument to --task-metrics={yes|no} (%s)", optarg);
            break;
        default:
            jl_errorf("julia: unhandled option -- %c\n"
                      "This is a bug, please report it.", c);
        }
    }
    if (codecov || malloclog) {
        if (pkgimage_explicit && jl_options.use_pkgimages) {
            jl_errorf("julia: Can't use --pkgimages=yes together "
                      "with --track-allocation or --code-coverage.");
        }
        jl_options.use_pkgimages = 0;
    }
    jl_options.code_coverage = codecov;
    jl_options.malloc_log = malloclog;
    int proc_args = *argcp < optind ? *argcp : optind;
    *argvp += proc_args;
    *argcp -= proc_args;
}

JL_DLLEXPORT ssize_t jl_sizeof_jl_options(void)
{
    return sizeof(jl_options_t);
}
