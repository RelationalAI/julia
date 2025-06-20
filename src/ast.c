// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  AST
  components of the front-end, for obtaining and translating syntax trees
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"
#include "flisp.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

// head symbols for each expression type
JL_DLLEXPORT jl_sym_t *jl_call_sym;
JL_DLLEXPORT jl_sym_t *jl_invoke_sym;
JL_DLLEXPORT jl_sym_t *jl_invoke_modify_sym;
JL_DLLEXPORT jl_sym_t *jl_empty_sym;
JL_DLLEXPORT jl_sym_t *jl_top_sym;
JL_DLLEXPORT jl_sym_t *jl_module_sym;
JL_DLLEXPORT jl_sym_t *jl_slot_sym;
JL_DLLEXPORT jl_sym_t *jl_export_sym;
JL_DLLEXPORT jl_sym_t *jl_import_sym;
JL_DLLEXPORT jl_sym_t *jl_toplevel_sym;
JL_DLLEXPORT jl_sym_t *jl_quote_sym;
JL_DLLEXPORT jl_sym_t *jl_line_sym;
JL_DLLEXPORT jl_sym_t *jl_incomplete_sym;
JL_DLLEXPORT jl_sym_t *jl_goto_sym;
JL_DLLEXPORT jl_sym_t *jl_goto_ifnot_sym;
JL_DLLEXPORT jl_sym_t *jl_return_sym;
JL_DLLEXPORT jl_sym_t *jl_lineinfo_sym;
JL_DLLEXPORT jl_sym_t *jl_lambda_sym;
JL_DLLEXPORT jl_sym_t *jl_assign_sym;
JL_DLLEXPORT jl_sym_t *jl_globalref_sym;
JL_DLLEXPORT jl_sym_t *jl_do_sym;
JL_DLLEXPORT jl_sym_t *jl_method_sym;
JL_DLLEXPORT jl_sym_t *jl_core_sym;
JL_DLLEXPORT jl_sym_t *jl_enter_sym;
JL_DLLEXPORT jl_sym_t *jl_leave_sym;
JL_DLLEXPORT jl_sym_t *jl_pop_exception_sym;
JL_DLLEXPORT jl_sym_t *jl_exc_sym;
JL_DLLEXPORT jl_sym_t *jl_error_sym;
JL_DLLEXPORT jl_sym_t *jl_new_sym;
JL_DLLEXPORT jl_sym_t *jl_using_sym;
JL_DLLEXPORT jl_sym_t *jl_splatnew_sym;
JL_DLLEXPORT jl_sym_t *jl_block_sym;
JL_DLLEXPORT jl_sym_t *jl_new_opaque_closure_sym;
JL_DLLEXPORT jl_sym_t *jl_opaque_closure_method_sym;
JL_DLLEXPORT jl_sym_t *jl_const_sym;
JL_DLLEXPORT jl_sym_t *jl_thunk_sym;
JL_DLLEXPORT jl_sym_t *jl_foreigncall_sym;
JL_DLLEXPORT jl_sym_t *jl_as_sym;
JL_DLLEXPORT jl_sym_t *jl_global_sym;
JL_DLLEXPORT jl_sym_t *jl_list_sym;
JL_DLLEXPORT jl_sym_t *jl_dot_sym;
JL_DLLEXPORT jl_sym_t *jl_newvar_sym;
JL_DLLEXPORT jl_sym_t *jl_boundscheck_sym;
JL_DLLEXPORT jl_sym_t *jl_inbounds_sym;
JL_DLLEXPORT jl_sym_t *jl_copyast_sym;
JL_DLLEXPORT jl_sym_t *jl_cfunction_sym;
JL_DLLEXPORT jl_sym_t *jl_loopinfo_sym;
JL_DLLEXPORT jl_sym_t *jl_meta_sym;
JL_DLLEXPORT jl_sym_t *jl_inert_sym;
JL_DLLEXPORT jl_sym_t *jl_polly_sym;
JL_DLLEXPORT jl_sym_t *jl_unused_sym;
JL_DLLEXPORT jl_sym_t *jl_static_parameter_sym;
JL_DLLEXPORT jl_sym_t *jl_inline_sym;
JL_DLLEXPORT jl_sym_t *jl_noinline_sym;
JL_DLLEXPORT jl_sym_t *jl_generated_sym;
JL_DLLEXPORT jl_sym_t *jl_generated_only_sym;
JL_DLLEXPORT jl_sym_t *jl_isdefined_sym;
JL_DLLEXPORT jl_sym_t *jl_propagate_inbounds_sym;
JL_DLLEXPORT jl_sym_t *jl_specialize_sym;
JL_DLLEXPORT jl_sym_t *jl_aggressive_constprop_sym;
JL_DLLEXPORT jl_sym_t *jl_no_constprop_sym;
JL_DLLEXPORT jl_sym_t *jl_purity_sym;
JL_DLLEXPORT jl_sym_t *jl_nospecialize_sym;
JL_DLLEXPORT jl_sym_t *jl_nospecializeinfer_sym;
JL_DLLEXPORT jl_sym_t *jl_macrocall_sym;
JL_DLLEXPORT jl_sym_t *jl_colon_sym;
JL_DLLEXPORT jl_sym_t *jl_hygienicscope_sym;
JL_DLLEXPORT jl_sym_t *jl_throw_undef_if_not_sym;
JL_DLLEXPORT jl_sym_t *jl_getfield_undefref_sym;
JL_DLLEXPORT jl_sym_t *jl_gc_preserve_begin_sym;
JL_DLLEXPORT jl_sym_t *jl_gc_preserve_end_sym;
JL_DLLEXPORT jl_sym_t *jl_coverageeffect_sym;
JL_DLLEXPORT jl_sym_t *jl_escape_sym;
JL_DLLEXPORT jl_sym_t *jl_aliasscope_sym;
JL_DLLEXPORT jl_sym_t *jl_popaliasscope_sym;
JL_DLLEXPORT jl_sym_t *jl_optlevel_sym;
JL_DLLEXPORT jl_sym_t *jl_thismodule_sym;
JL_DLLEXPORT jl_sym_t *jl_atom_sym;
JL_DLLEXPORT jl_sym_t *jl_statement_sym;
JL_DLLEXPORT jl_sym_t *jl_all_sym;
JL_DLLEXPORT jl_sym_t *jl_compile_sym;
JL_DLLEXPORT jl_sym_t *jl_force_compile_sym;
JL_DLLEXPORT jl_sym_t *jl_infer_sym;
JL_DLLEXPORT jl_sym_t *jl_max_methods_sym;
JL_DLLEXPORT jl_sym_t *jl_atomic_sym;
JL_DLLEXPORT jl_sym_t *jl_not_atomic_sym;
JL_DLLEXPORT jl_sym_t *jl_unordered_sym;
JL_DLLEXPORT jl_sym_t *jl_monotonic_sym;
JL_DLLEXPORT jl_sym_t *jl_acquire_sym;
JL_DLLEXPORT jl_sym_t *jl_release_sym;
JL_DLLEXPORT jl_sym_t *jl_acquire_release_sym;
JL_DLLEXPORT jl_sym_t *jl_sequentially_consistent_sym;


static const uint8_t flisp_system_image[] = {
#include <julia_flisp.boot.inc>
};

typedef struct _jl_ast_context_t {
    fl_context_t fl;
    fltype_t *jvtype;

    value_t true_sym;
    value_t false_sym;
    value_t error_sym;
    value_t null_sym;
    value_t ssavalue_sym;
    value_t slot_sym;
    jl_module_t *module; // context module for `current-julia-module-counter`
    struct _jl_ast_context_t *next; // invasive list pointer for getting free contexts
} jl_ast_context_t;

static jl_ast_context_t jl_ast_main_ctx;

#ifdef __clang_gcanalyzer__
jl_ast_context_t *jl_ast_ctx(fl_context_t *fl) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT;
#else
#define jl_ast_ctx(fl_ctx) container_of(fl_ctx, jl_ast_context_t, fl)
#endif

struct macroctx_stack {
    jl_module_t *m;
    struct macroctx_stack *parent;
};

static jl_value_t *scm_to_julia(fl_context_t *fl_ctx, value_t e, jl_module_t *mod);
static value_t julia_to_scm(fl_context_t *fl_ctx, jl_value_t *v);
static jl_value_t *jl_expand_macros(jl_value_t *expr, jl_module_t *inmodule, struct macroctx_stack *macroctx, int onelevel, size_t world, int throw_load_error);

static value_t fl_defined_julia_global(fl_context_t *fl_ctx, value_t *args, uint32_t nargs)
{
    // tells whether a var is defined in and *by* the current module
    argcount(fl_ctx, "defined-julia-global", nargs, 1);
    (void)tosymbol(fl_ctx, args[0], "defined-julia-global");
    jl_ast_context_t *ctx = jl_ast_ctx(fl_ctx);
    jl_sym_t *var = jl_symbol(symbol_name(fl_ctx, args[0]));
    jl_binding_t *b = jl_get_module_binding(ctx->module, var, 0);
    return (b != NULL && jl_atomic_load_relaxed(&b->owner) == b) ? fl_ctx->T : fl_ctx->F;
}

// Used to generate a unique suffix for a given symbol (e.g. variable or type name)
// first argument contains a stack of method definitions seen so far by `closure-convert` in flisp.
// if the top of the stack is non-NIL, we use it to augment the suffix so that it becomes
// of the form $top_level_method_name##$counter, where `counter` is the smallest integer
// such that the resulting name is not already defined in the current module's bindings.
// If the top of the stack is NIL, we simply return the current module's counter.
// This ensures that precompile statements are a bit more stable across different versions
// of a codebase. see #53719
static value_t fl_module_unique_name(fl_context_t *fl_ctx, value_t *args, uint32_t nargs)
{
    argcount(fl_ctx, "julia-module-unique-name", nargs, 1);
    jl_ast_context_t *ctx = jl_ast_ctx(fl_ctx);
    jl_module_t *m = ctx->module;
    assert(m != NULL);
    // Get the outermost function name from the `parsed_method_stack` top
    char *funcname = NULL;
    value_t parsed_method_stack = args[0];
    if (parsed_method_stack != fl_ctx->NIL) {
        value_t bottom_stack_symbol = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "last")), parsed_method_stack);
        funcname = tosymbol(fl_ctx, bottom_stack_symbol, "julia-module-unique-name")->name;
    }
    size_t sz = funcname != NULL ? strlen(funcname) + 32 : 32; // 32 is enough for the suffix
    char *buf = (char*)alloca(sz);
    if (funcname != NULL && strchr(funcname, '#') == NULL) {
        for (int i = 0; ; i++) {
            snprintf(buf, sz, "%s##%d", funcname, i);
            jl_sym_t *sym = jl_symbol(buf);
            JL_LOCK(&m->lock);
            if (jl_get_module_binding(m, sym, 0) == NULL) { // make sure this name is not already taken
                jl_get_module_binding(m, sym, 1); // create the binding
                JL_UNLOCK(&m->lock);
                return symbol(fl_ctx, buf);
            }
            JL_UNLOCK(&m->lock);
        }
    }
    else {
        snprintf(buf, sz, "%d", jl_module_next_counter(m));
    }
    return symbol(fl_ctx, buf);
}

static value_t fl_julia_current_file(fl_context_t *fl_ctx, value_t *args, uint32_t nargs) JL_NOTSAFEPOINT
{
    return symbol(fl_ctx, jl_filename);
}

static value_t fl_julia_current_line(fl_context_t *fl_ctx, value_t *args, uint32_t nargs) JL_NOTSAFEPOINT
{
    return fixnum(jl_lineno);
}

static int jl_is_number(jl_value_t *v)
{
    jl_datatype_t *t = (jl_datatype_t*)jl_typeof(v);
    for (; t->super != t; t = t->super)
        if (t == jl_number_type)
            return 1;
    return 0;
}

// Check whether v is a scalar for purposes of inlining fused-broadcast
// arguments when lowering; should agree with broadcast.jl on what is a
// scalar.  When in doubt, return false, since this is only an optimization.
static value_t fl_julia_scalar(fl_context_t *fl_ctx, value_t *args, uint32_t nargs)
{
    argcount(fl_ctx, "julia-scalar?", nargs, 1);
    if (fl_isnumber(fl_ctx, args[0]) || fl_isstring(fl_ctx, args[0]))
        return fl_ctx->T;
    else if (iscvalue(args[0]) && fl_ctx->jl_sym == cv_type((cvalue_t*)ptr(args[0]))) {
        jl_value_t *v = *(jl_value_t**)cptr(args[0]);
        if (jl_is_number(v) || jl_is_string(v))
            return fl_ctx->T;
    }
    return fl_ctx->F;
}

static jl_value_t *scm_to_julia_(fl_context_t *fl_ctx, value_t e, jl_module_t *mod);

static const builtinspec_t julia_flisp_ast_ext[] = {
    { "defined-julia-global", fl_defined_julia_global }, // TODO: can we kill this safepoint
    { "current-julia-module-counter", fl_module_unique_name },
    { "julia-scalar?", fl_julia_scalar },
    { "julia-current-file", fl_julia_current_file },
    { "julia-current-line", fl_julia_current_line },
    { NULL, NULL }
};

static void jl_init_ast_ctx(jl_ast_context_t *ctx) JL_NOTSAFEPOINT
{
    fl_context_t *fl_ctx = &ctx->fl;
    fl_init(fl_ctx, 4*1024*1024);

    if (fl_load_system_image_str(fl_ctx, (char*)flisp_system_image,
                                 sizeof(flisp_system_image))) {
        jl_error("fatal error loading system image");
    }

    fl_applyn(fl_ctx, 0, symbol_value(symbol(fl_ctx, "__init_globals")));

    ctx->jvtype = define_opaque_type(fl_ctx->jl_sym, sizeof(void*), NULL, NULL);
    assign_global_builtins(fl_ctx, julia_flisp_ast_ext);
    ctx->true_sym = symbol(fl_ctx, "true");
    ctx->false_sym = symbol(fl_ctx, "false");
    ctx->error_sym = symbol(fl_ctx, "error");
    ctx->null_sym = symbol(fl_ctx, "null");
    ctx->ssavalue_sym = symbol(fl_ctx, "ssavalue");
    ctx->slot_sym = symbol(fl_ctx, "slot");
    ctx->module = NULL;
    set(symbol(fl_ctx, "*scopewarn-opt*"), fixnum(jl_options.warn_scope));
}

// There should be no GC allocation while holding this lock
static uv_mutex_t flisp_lock;
static jl_ast_context_t *jl_ast_ctx_freed = NULL;

static jl_ast_context_t *jl_ast_ctx_enter(jl_module_t *m) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT
{
    JL_SIGATOMIC_BEGIN();
    uv_mutex_lock(&flisp_lock);
    jl_ast_context_t *ctx = jl_ast_ctx_freed;
    if (ctx != NULL) {
        jl_ast_ctx_freed = ctx->next;
        ctx->next = NULL;
    }
    uv_mutex_unlock(&flisp_lock);
    if (ctx == NULL) {
        // Construct a new one if we can't find any
        ctx = (jl_ast_context_t*)calloc(1, sizeof(jl_ast_context_t));
        jl_init_ast_ctx(ctx);
    }
    ctx->module = m;
    return ctx;
}

static void jl_ast_ctx_leave(jl_ast_context_t *ctx)
{
    uv_mutex_lock(&flisp_lock);
    ctx->module = NULL;
    ctx->next = jl_ast_ctx_freed;
    jl_ast_ctx_freed = ctx;
    uv_mutex_unlock(&flisp_lock);
    JL_SIGATOMIC_END();
}

void jl_init_flisp(void)
{
    if (jl_ast_ctx_freed)
        return;
    uv_mutex_init(&flisp_lock);
    jl_init_ast_ctx(&jl_ast_main_ctx);
    // To match the one in jl_ast_ctx_leave
    JL_SIGATOMIC_BEGIN();
    jl_ast_ctx_leave(&jl_ast_main_ctx);
}

void jl_init_common_symbols(void)
{
    jl_empty_sym = jl_symbol("");
    jl_call_sym = jl_symbol("call");
    jl_invoke_sym = jl_symbol("invoke");
    jl_invoke_modify_sym = jl_symbol("invoke_modify");
    jl_foreigncall_sym = jl_symbol("foreigncall");
    jl_cfunction_sym = jl_symbol("cfunction");
    jl_quote_sym = jl_symbol("quote");
    jl_inert_sym = jl_symbol("inert");
    jl_top_sym = jl_symbol("top");
    jl_core_sym = jl_symbol("core");
    jl_globalref_sym = jl_symbol("globalref");
    jl_line_sym = jl_symbol("line");
    jl_lineinfo_sym = jl_symbol("lineinfo");
    jl_incomplete_sym = jl_symbol("incomplete");
    jl_error_sym = jl_symbol("error");
    jl_goto_sym = jl_symbol("goto");
    jl_goto_ifnot_sym = jl_symbol("gotoifnot");
    jl_return_sym = jl_symbol("return");
    jl_lambda_sym = jl_symbol("lambda");
    jl_module_sym = jl_symbol("module");
    jl_export_sym = jl_symbol("export");
    jl_import_sym = jl_symbol("import");
    jl_using_sym = jl_symbol("using");
    jl_assign_sym = jl_symbol("=");
    jl_method_sym = jl_symbol("method");
    jl_exc_sym = jl_symbol("the_exception");
    jl_enter_sym = jl_symbol("enter");
    jl_leave_sym = jl_symbol("leave");
    jl_pop_exception_sym = jl_symbol("pop_exception");
    jl_new_sym = jl_symbol("new");
    jl_splatnew_sym = jl_symbol("splatnew");
    jl_new_opaque_closure_sym = jl_symbol("new_opaque_closure");
    jl_opaque_closure_method_sym = jl_symbol("opaque_closure_method");
    jl_const_sym = jl_symbol("const");
    jl_global_sym = jl_symbol("global");
    jl_thunk_sym = jl_symbol("thunk");
    jl_toplevel_sym = jl_symbol("toplevel");
    jl_dot_sym = jl_symbol(".");
    jl_as_sym = jl_symbol("as");
    jl_colon_sym = jl_symbol(":");
    jl_boundscheck_sym = jl_symbol("boundscheck");
    jl_inbounds_sym = jl_symbol("inbounds");
    jl_newvar_sym = jl_symbol("newvar");
    jl_copyast_sym = jl_symbol("copyast");
    jl_loopinfo_sym = jl_symbol("loopinfo");
    jl_meta_sym = jl_symbol("meta");
    jl_list_sym = jl_symbol("list");
    jl_unused_sym = jl_symbol("#unused#");
    jl_slot_sym = jl_symbol("slot");
    jl_static_parameter_sym = jl_symbol("static_parameter");
    jl_inline_sym = jl_symbol("inline");
    jl_noinline_sym = jl_symbol("noinline");
    jl_polly_sym = jl_symbol("polly");
    jl_propagate_inbounds_sym = jl_symbol("propagate_inbounds");
    jl_aggressive_constprop_sym = jl_symbol("aggressive_constprop");
    jl_no_constprop_sym = jl_symbol("no_constprop");
    jl_purity_sym = jl_symbol("purity");
    jl_isdefined_sym = jl_symbol("isdefined");
    jl_nospecialize_sym = jl_symbol("nospecialize");
    jl_specialize_sym = jl_symbol("specialize");
    jl_nospecializeinfer_sym = jl_symbol("nospecializeinfer");
    jl_optlevel_sym = jl_symbol("optlevel");
    jl_compile_sym = jl_symbol("compile");
    jl_force_compile_sym = jl_symbol("force_compile");
    jl_infer_sym = jl_symbol("infer");
    jl_max_methods_sym = jl_symbol("max_methods");
    jl_macrocall_sym = jl_symbol("macrocall");
    jl_escape_sym = jl_symbol("escape");
    jl_hygienicscope_sym = jl_symbol("hygienic-scope");
    jl_gc_preserve_begin_sym = jl_symbol("gc_preserve_begin");
    jl_gc_preserve_end_sym = jl_symbol("gc_preserve_end");
    jl_generated_sym = jl_symbol("generated");
    jl_generated_only_sym = jl_symbol("generated_only");
    jl_throw_undef_if_not_sym = jl_symbol("throw_undef_if_not");
    jl_getfield_undefref_sym = jl_symbol("##getfield##");
    jl_do_sym = jl_symbol("do");
    jl_coverageeffect_sym = jl_symbol("code_coverage_effect");
    jl_aliasscope_sym = jl_symbol("aliasscope");
    jl_popaliasscope_sym = jl_symbol("popaliasscope");
    jl_thismodule_sym = jl_symbol("thismodule");
    jl_block_sym = jl_symbol("block");
    jl_atom_sym = jl_symbol("atom");
    jl_statement_sym = jl_symbol("statement");
    jl_all_sym = jl_symbol("all");
    jl_atomic_sym = jl_symbol("atomic");
    jl_not_atomic_sym = jl_symbol("not_atomic");
    jl_unordered_sym = jl_symbol("unordered");
    jl_monotonic_sym = jl_symbol("monotonic");
    jl_acquire_sym = jl_symbol("acquire");
    jl_release_sym = jl_symbol("release");
    jl_acquire_release_sym = jl_symbol("acquire_release");
    jl_sequentially_consistent_sym = jl_symbol("sequentially_consistent");
}

JL_DLLEXPORT void jl_lisp_prompt(void)
{
    // Make `--lisp` sigatomic in order to avoid triggering the sigint safepoint.
    // We don't have our signal handler registered in that case anyway...
    JL_SIGATOMIC_BEGIN();
    jl_init_flisp();
    jl_ast_context_t *ctx = jl_ast_ctx_enter(jl_main_module);
    fl_context_t *fl_ctx = &ctx->fl;
    fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "__start")), fl_cons(fl_ctx, fl_ctx->NIL,fl_ctx->NIL));
    jl_ast_ctx_leave(ctx);
}

JL_DLLEXPORT void fl_show_profile(void)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    fl_applyn(fl_ctx, 0, symbol_value(symbol(fl_ctx, "show-profiles")));
    jl_ast_ctx_leave(ctx);
}

JL_DLLEXPORT void fl_clear_profile(void)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    fl_applyn(fl_ctx, 0, symbol_value(symbol(fl_ctx, "clear-profiles")));
    jl_ast_ctx_leave(ctx);
}

JL_DLLEXPORT void fl_profile(const char *fname)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "profile-e")), symbol(fl_ctx, fname));
    jl_ast_ctx_leave(ctx);
}


static jl_sym_t *scmsym_to_julia(fl_context_t *fl_ctx, value_t s)
{
    assert(issymbol(s));
    if (fl_isgensym(fl_ctx, s)) {
        char gsname[16];
        char *n = uint2str(&gsname[1], sizeof(gsname)-1,
                           ((gensym_t*)ptr(s))->id, 10);
        *(--n) = '#';
        return jl_symbol(n);
    }
    return jl_symbol(symbol_name(fl_ctx, s));
}

static jl_value_t *scm_to_julia(fl_context_t *fl_ctx, value_t e, jl_module_t *mod)
{
    jl_value_t *v = NULL;
    JL_GC_PUSH1(&v);
    JL_TRY {
        v = scm_to_julia_(fl_ctx, e, mod);
    }
    JL_CATCH {
        // if expression cannot be converted, replace with error expr
        //jl_(jl_current_exception());
        //jlbacktrace();
        jl_expr_t *ex = jl_exprn(jl_error_sym, 1);
        v = (jl_value_t*)ex;
        jl_array_ptr_set(ex->args, 0, jl_cstr_to_string("invalid AST"));
    }
    JL_GC_POP();
    return v;
}

extern int64_t conv_to_int64(void *data, numerictype_t tag);

static jl_value_t *scm_to_julia_(fl_context_t *fl_ctx, value_t e, jl_module_t *mod)
{
    if (fl_isnumber(fl_ctx, e)) {
        int64_t i64;
        if (isfixnum(e)) {
            i64 = numval(e);
        }
        else {
            assert(iscprim(e));
            cprim_t *cp = (cprim_t*)ptr(e);
            numerictype_t nt = cp_numtype(cp);
            switch (nt) {
            case T_DOUBLE:
                return (jl_value_t*)jl_box_float64(*(double*)cp_data(cp));
            case T_FLOAT:
                return (jl_value_t*)jl_box_float32(*(float*)cp_data(cp));
            case T_UINT8:
                return (jl_value_t*)jl_box_uint8(*(uint8_t*)cp_data(cp));
            case T_UINT16:
                return (jl_value_t*)jl_box_uint16(*(uint16_t*)cp_data(cp));
            case T_UINT32:
                return (jl_value_t*)jl_box_uint32(*(uint32_t*)cp_data(cp));
            case T_UINT64:
                return (jl_value_t*)jl_box_uint64(*(uint64_t*)cp_data(cp));
            default:
                ;
            }
            i64 = conv_to_int64(cp_data(cp), nt);
        }
#ifdef _P64
        return (jl_value_t*)jl_box_int64(i64);
#else
        if (i64 > (int64_t)S32_MAX || i64 < (int64_t)S32_MIN)
            return (jl_value_t*)jl_box_int64(i64);
        else
            return (jl_value_t*)jl_box_int32((int32_t)i64);
#endif
    }
    if (issymbol(e))
        return (jl_value_t*)scmsym_to_julia(fl_ctx, e);
    if (fl_isstring(fl_ctx, e))
        return jl_pchar_to_string((char*)cvalue_data(e), cvalue_len(e));
    if (iscons(e) || e == fl_ctx->NIL) {
        value_t hd;
        jl_sym_t *sym;
        if (e == fl_ctx->NIL) {
            hd = e;
        }
        else {
            hd = car_(e);
            if (hd == jl_ast_ctx(fl_ctx)->ssavalue_sym)
                return jl_box_ssavalue(numval(car_(cdr_(e))));
            else if (hd == jl_ast_ctx(fl_ctx)->slot_sym)
                return jl_box_slotnumber(numval(car_(cdr_(e))));
            else if (hd == jl_ast_ctx(fl_ctx)->null_sym && llength(e) == 1)
                return jl_nothing;
            else if (hd == jl_ast_ctx(fl_ctx)->true_sym && llength(e) == 1)
                return jl_true;
            else if (hd == jl_ast_ctx(fl_ctx)->false_sym && llength(e) == 1)
                return jl_false;
            else if (hd == fl_ctx->jl_char_sym && llength(e) == 2) {
                value_t v = car_(cdr_(e));
                if (!(iscprim(v) && cp_class((cprim_t*)ptr(v)) == fl_ctx->uint32type))
                    jl_error("malformed julia char");
                uint32_t c = *(uint32_t*)cp_data((cprim_t*)ptr(v));
                return jl_box_char(c);
            }
        }
        if (issymbol(hd))
            sym = scmsym_to_julia(fl_ctx, hd);
        else
            sym = jl_list_sym;
        size_t n = llength(e)-1;
        if (issymbol(hd))
            e = cdr_(e);
        else
            n++;
        // nodes with special representations
        jl_value_t *ex = NULL, *temp = NULL;
        if (sym == jl_line_sym && (n == 1 || n == 2)) {
            jl_value_t *linenum = scm_to_julia_(fl_ctx, car_(e), mod);
            jl_value_t *file = jl_nothing;
            JL_GC_PUSH2(&linenum, &file);
            if (n == 2)
                file = scm_to_julia_(fl_ctx, car_(cdr_(e)), mod);
            temp = jl_new_struct(jl_linenumbernode_type, linenum, file);
            JL_GC_POP();
            return temp;
        }
        else if (sym == jl_lineinfo_sym && n == 5) {
            jl_value_t *modu=NULL, *name=NULL, *file=NULL, *linenum=NULL, *inlinedat=NULL;
            JL_GC_PUSH5(&modu, &name, &file, &linenum, &inlinedat);
            value_t lst = e;
            modu = scm_to_julia_(fl_ctx, car_(lst), mod);
            lst = cdr_(lst);
            name = scm_to_julia_(fl_ctx, car_(lst), mod);
            lst = cdr_(lst);
            file = scm_to_julia_(fl_ctx, car_(lst), mod);
            lst = cdr_(lst);
            linenum = scm_to_julia_(fl_ctx, car_(lst), mod);
            lst = cdr_(lst);
            inlinedat = scm_to_julia_(fl_ctx, car_(lst), mod);
            temp = jl_new_struct(jl_lineinfonode_type, modu, name, file, linenum, inlinedat);
            JL_GC_POP();
            return temp;
        }
        JL_GC_PUSH2(&ex, &temp);
        if (sym == jl_goto_sym) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            temp = jl_new_struct(jl_gotonode_type, ex);
        }
        else if (sym == jl_goto_ifnot_sym) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            temp = scm_to_julia(fl_ctx, car_(cdr_(e)), mod);
            temp = jl_new_struct(jl_gotoifnot_type, ex, temp);
        }
        else if (sym == jl_newvar_sym) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            temp = jl_new_struct(jl_newvarnode_type, ex);
        }
        else if (sym == jl_globalref_sym) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            temp = scm_to_julia_(fl_ctx, car_(cdr_(e)), mod);
            assert(jl_is_module(ex));
            assert(jl_is_symbol(temp));
            temp = jl_module_globalref((jl_module_t*)ex, (jl_sym_t*)temp);
        }
        else if (sym == jl_top_sym) {
            assert(mod && "top should not be generated by the parser");
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            assert(jl_is_symbol(ex));
            temp = jl_module_globalref(jl_base_relative_to(mod), (jl_sym_t*)ex);
        }
        else if (sym == jl_core_sym) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            assert(jl_is_symbol(ex));
            temp = jl_module_globalref(jl_core_module, (jl_sym_t*)ex);
        }
        else if (sym == jl_thismodule_sym) {
            temp = (jl_value_t*)mod;
        }
        else if (iscons(e) && (sym == jl_inert_sym || (sym == jl_quote_sym && (!iscons(car_(e)))))) {
            ex = scm_to_julia_(fl_ctx, car_(e), mod);
            temp = jl_new_struct(jl_quotenode_type, ex);
        }
        if (temp) {
            JL_GC_POP();
            return temp;
        }
        ex = (jl_value_t*)jl_exprn(sym, n);
        size_t i;
        for (i = 0; i < n; i++) {
            assert(iscons(e));
            jl_array_ptr_set(((jl_expr_t*)ex)->args, i, scm_to_julia_(fl_ctx, car_(e), mod));
            e = cdr_(e);
        }
        if (sym == jl_lambda_sym)
            ex = (jl_value_t*)jl_new_code_info_from_ir((jl_expr_t*)ex);
        JL_GC_POP();
        if (sym == jl_list_sym)
            return (jl_value_t*)((jl_expr_t*)ex)->args;
        return (jl_value_t*)ex;
    }
    if (iscprim(e) && cp_class((cprim_t*)ptr(e)) == fl_ctx->wchartype) {
        uint32_t c, u = *(uint32_t*)cp_data((cprim_t*)ptr(e));
        if (u < 0x80) {
            c = u << 24;
        } else {
            c = ((u << 0) & 0x0000003f) | ((u << 2) & 0x00003f00) |
                ((u << 4) & 0x003f0000) | ((u << 6) & 0x3f000000);
            c = u < 0x00000800 ? (c << 16) | 0xc0800000 :
                u < 0x00010000 ? (c <<  8) | 0xe0808000 :
                                 (c <<  0) | 0xf0808080 ;
        }
        return jl_box_char(c);
    }
    if (iscvalue(e) && cv_class((cvalue_t*)ptr(e)) == jl_ast_ctx(fl_ctx)->jvtype) {
        return *(jl_value_t**)cv_data((cvalue_t*)ptr(e));
    }
    jl_error("malformed tree");
}

static value_t julia_to_scm_(fl_context_t *fl_ctx, jl_value_t *v, int check_valid);

static value_t julia_to_scm(fl_context_t *fl_ctx, jl_value_t *v)
{
    value_t temp;
    // need try/catch to reset GC handle stack in case of error
    FL_TRY_EXTERN(fl_ctx) {
        temp = julia_to_scm_(fl_ctx, v, 1);
    }
    FL_CATCH_EXTERN(fl_ctx) {
        temp = fl_ctx->lasterror;
    }
    return temp;
}

static void array_to_list(fl_context_t *fl_ctx, jl_array_t *a, value_t *pv, int check_valid)
{
    value_t temp;
    for(long i=jl_array_len(a)-1; i >= 0; i--) {
        *pv = fl_cons(fl_ctx, fl_ctx->NIL, *pv);
        temp = julia_to_scm_(fl_ctx, jl_array_ptr_ref(a,i), check_valid);
        // note: must be separate statement
        car_(*pv) = temp;
    }
}

static value_t julia_to_list2(fl_context_t *fl_ctx, jl_value_t *a, jl_value_t *b, int check_valid)
{
    value_t sa = julia_to_scm_(fl_ctx, a, check_valid);
    fl_gc_handle(fl_ctx, &sa);
    value_t sb = julia_to_scm_(fl_ctx, b, check_valid);
    value_t l = fl_list2(fl_ctx, sa, sb);
    fl_free_gc_handles(fl_ctx, 1);
    return l;
}

static int julia_to_scm_noalloc1(fl_context_t *fl_ctx, jl_value_t *v, value_t *retval) JL_NOTSAFEPOINT
{
    if (v == NULL)
        lerror(fl_ctx, symbol(fl_ctx, "error"), "undefined reference in AST");
    else if (jl_is_symbol(v))
        *retval = symbol(fl_ctx, jl_symbol_name((jl_sym_t*)v));
    else if (v == jl_true)
        *retval = fl_cons(fl_ctx, jl_ast_ctx(fl_ctx)->true_sym, fl_ctx->NIL);
    else if (v == jl_false)
        *retval = fl_cons(fl_ctx, jl_ast_ctx(fl_ctx)->false_sym, fl_ctx->NIL);
    else if (v == jl_nothing)
        *retval = fl_cons(fl_ctx, jl_ast_ctx(fl_ctx)->null_sym, fl_ctx->NIL);
    else
        return 0;
    return 1;
}

static value_t julia_to_scm_noalloc2(fl_context_t *fl_ctx, jl_value_t *v, int check_valid) JL_NOTSAFEPOINT
{
    if (jl_is_long(v) && fits_fixnum(jl_unbox_long(v)))
        return fixnum(jl_unbox_long(v));
    if (check_valid) {
        if (jl_is_ssavalue(v))
            lerror(fl_ctx, symbol(fl_ctx, "error"), "SSAValue objects should not occur in an AST");
        if (jl_is_slotnumber(v))
            lerror(fl_ctx, symbol(fl_ctx, "error"), "SlotNumber objects should not occur in an AST");
    }
    value_t opaque = cvalue(fl_ctx, jl_ast_ctx(fl_ctx)->jvtype, sizeof(void*));
    *(jl_value_t**)cv_data((cvalue_t*)ptr(opaque)) = v;
    return opaque;
}

static value_t julia_to_scm_noalloc(fl_context_t *fl_ctx, jl_value_t *v, int check_valid) JL_NOTSAFEPOINT
{
    value_t retval;
    if (julia_to_scm_noalloc1(fl_ctx, v, &retval))
        return retval;
    assert(!jl_is_expr(v) &&
           !jl_typetagis(v, jl_linenumbernode_type) &&
           !jl_typetagis(v, jl_gotonode_type) &&
           !jl_typetagis(v, jl_quotenode_type) &&
           !jl_typetagis(v, jl_newvarnode_type) &&
           !jl_typetagis(v, jl_globalref_type));
    return julia_to_scm_noalloc2(fl_ctx, v, check_valid);
}

static value_t julia_to_list2_noalloc(fl_context_t *fl_ctx, jl_value_t *a, jl_value_t *b, int check_valid) JL_NOTSAFEPOINT
{
    value_t sa = julia_to_scm_noalloc(fl_ctx, a, check_valid);
    fl_gc_handle(fl_ctx, &sa);
    value_t sb = julia_to_scm_noalloc(fl_ctx, b, check_valid);
    value_t l = fl_list2(fl_ctx, sa, sb);
    fl_free_gc_handles(fl_ctx, 1);
    return l;
}

static value_t julia_to_scm_(fl_context_t *fl_ctx, jl_value_t *v, int check_valid)
{
    value_t retval;
    if (julia_to_scm_noalloc1(fl_ctx, v, &retval))
        return retval;
    if (jl_is_expr(v)) {
        jl_expr_t *ex = (jl_expr_t*)v;
        value_t args = fl_ctx->NIL;
        fl_gc_handle(fl_ctx, &args);
        if (jl_expr_nargs(ex) > 520000 && ex->head != jl_block_sym)
            lerror(fl_ctx, symbol(fl_ctx, "error"), "expression too large");
        array_to_list(fl_ctx, ex->args, &args, check_valid);
        value_t hd = julia_to_scm_(fl_ctx, (jl_value_t*)ex->head, check_valid);
        if (ex->head == jl_lambda_sym && jl_expr_nargs(ex)>0 && jl_is_array(jl_exprarg(ex,0))) {
            value_t llist = fl_ctx->NIL;
            fl_gc_handle(fl_ctx, &llist);
            array_to_list(fl_ctx, (jl_array_t*)jl_exprarg(ex,0), &llist, check_valid);
            car_(args) = llist;
            fl_free_gc_handles(fl_ctx, 1);
        }
        value_t scmv = fl_cons(fl_ctx, hd, args);
        fl_free_gc_handles(fl_ctx, 1);
        return scmv;
    }
    // GC Note: jl_fieldref(v, 0) allocates for GotoNode
    //          but we don't need a GC root here because julia_to_list2_noalloc
    //          shouldn't allocate in this case.
    if (jl_is_linenode(v)) {
        jl_value_t *file = jl_fieldref_noalloc(v,1);
        jl_value_t *line = jl_fieldref(v,0);
        value_t args = julia_to_list2_noalloc(fl_ctx, line, file, check_valid);
        fl_gc_handle(fl_ctx, &args);
        value_t hd = julia_to_scm_(fl_ctx, (jl_value_t*)jl_line_sym, check_valid);
        value_t scmv = fl_cons(fl_ctx, hd, args);
        fl_free_gc_handles(fl_ctx, 1);
        return scmv;
    }
    if (jl_typetagis(v, jl_gotonode_type))
        return julia_to_list2_noalloc(fl_ctx, (jl_value_t*)jl_goto_sym, jl_fieldref(v,0), check_valid);
    if (jl_typetagis(v, jl_quotenode_type))
        return julia_to_list2(fl_ctx, (jl_value_t*)jl_inert_sym, jl_fieldref_noalloc(v,0), 0);
    if (jl_typetagis(v, jl_newvarnode_type))
        return julia_to_list2_noalloc(fl_ctx, (jl_value_t*)jl_newvar_sym, jl_fieldref(v,0), check_valid);
    if (jl_typetagis(v, jl_globalref_type)) {
        jl_module_t *m = jl_globalref_mod(v);
        jl_sym_t *sym = jl_globalref_name(v);
        if (m == jl_core_module)
            return julia_to_list2(fl_ctx, (jl_value_t*)jl_core_sym,
                                  (jl_value_t*)sym, check_valid);
        value_t args = julia_to_list2(fl_ctx, (jl_value_t*)m, (jl_value_t*)sym, check_valid);
        fl_gc_handle(fl_ctx, &args);
        value_t hd = julia_to_scm_(fl_ctx, (jl_value_t*)jl_globalref_sym, check_valid);
        value_t scmv = fl_cons(fl_ctx, hd, args);
        fl_free_gc_handles(fl_ctx, 1);
        return scmv;
    }
    return julia_to_scm_noalloc2(fl_ctx, v, check_valid);
}

// Parse `text` starting at 0-based `offset` and attributing the content to
// `filename`. Return an svec of (parsed_expr, final_offset)
JL_DLLEXPORT jl_value_t *jl_fl_parse(const char *text, size_t text_len,
                                     jl_value_t *filename, size_t lineno,
                                     size_t offset, jl_value_t *options)
{
    JL_TIMING(PARSING, PARSING);
    jl_timing_show_filename(jl_string_data(filename), JL_TIMING_DEFAULT_BLOCK);
    if (offset > text_len) {
        jl_value_t *textstr = jl_pchar_to_string(text, text_len);
        JL_GC_PUSH1(&textstr);
        jl_bounds_error(textstr, jl_box_long(offset+1));
    }
    jl_sym_t *rule = (jl_sym_t*)options;
    if (rule != jl_atom_sym && rule != jl_statement_sym && rule != jl_all_sym) {
        jl_error("jl_fl_parse: unrecognized parse options");
    }
    if (offset != 0 && rule == jl_all_sym) {
        jl_error("Parse `all`: offset not supported");
    }

    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    value_t fl_text = cvalue_static_cstrn(fl_ctx, text, text_len);
    fl_gc_handle(fl_ctx, &fl_text);
    value_t fl_filename = cvalue_static_cstrn(fl_ctx, jl_string_data(filename),
                                              jl_string_len(filename));
    fl_gc_handle(fl_ctx, &fl_filename);
    value_t fl_expr;
    size_t offset1 = 0;
    if (rule == jl_all_sym) {
        value_t e = fl_applyn(fl_ctx, 3, symbol_value(symbol(fl_ctx, "jl-parse-all")),
                              fl_text, fl_filename, fixnum(lineno));
        fl_expr = e;
        offset1 = e == fl_ctx->FL_EOF ? text_len : 0;
    }
    else {
        value_t greedy = rule == jl_statement_sym ? fl_ctx->T : fl_ctx->F;
        value_t p = fl_applyn(fl_ctx, 5, symbol_value(symbol(fl_ctx, "jl-parse-one")),
                              fl_text, fl_filename, fixnum(offset), greedy, fixnum(lineno));
        fl_expr = car_(p);
        offset1 = tosize(fl_ctx, cdr_(p), "parse");
    }
    fl_free_gc_handles(fl_ctx, 2);

    // Convert to julia values
    jl_value_t *expr = NULL, *end_offset = NULL;
    JL_GC_PUSH2(&expr, &end_offset);
    expr = fl_expr == fl_ctx->FL_EOF ? jl_nothing : scm_to_julia(fl_ctx, fl_expr, NULL);
    end_offset = jl_box_long(offset1);
    jl_ast_ctx_leave(ctx);
    jl_value_t *result = (jl_value_t*)jl_svec2(expr, end_offset);
    JL_GC_POP();
    return result;
}

// returns either an expression or a thunk
static jl_value_t *jl_call_scm_on_ast(const char *funcname, jl_value_t *expr, jl_module_t *inmodule)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(inmodule);
    fl_context_t *fl_ctx = &ctx->fl;
    value_t arg = julia_to_scm(fl_ctx, expr);
    value_t e = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, funcname)), arg);
    jl_value_t *result = scm_to_julia(fl_ctx, e, inmodule);
    JL_GC_PUSH1(&result);
    jl_ast_ctx_leave(ctx);
    JL_GC_POP();
    return result;
}

jl_value_t *jl_call_scm_on_ast_and_loc(const char *funcname, jl_value_t *expr,
                                       jl_module_t *inmodule, const char *file, int line)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(inmodule);
    fl_context_t *fl_ctx = &ctx->fl;
    value_t arg = julia_to_scm(fl_ctx, expr);
    value_t e = fl_applyn(fl_ctx, 3, symbol_value(symbol(fl_ctx, funcname)), arg,
                          symbol(fl_ctx, file), fixnum(line));
    jl_value_t *result = scm_to_julia(fl_ctx, e, inmodule);
    JL_GC_PUSH1(&result);
    jl_ast_ctx_leave(ctx);
    JL_GC_POP();
    return result;
}

// syntax tree accessors

JL_DLLEXPORT jl_value_t *jl_copy_ast(jl_value_t *expr)
{
    if (!expr)
        return NULL;
    if (jl_is_code_info(expr)) {
        jl_code_info_t *new_ci = (jl_code_info_t *)expr;
        jl_array_t *new_code = NULL;
        JL_GC_PUSH2(&new_ci, &new_code);
        new_ci = jl_copy_code_info(new_ci);
        new_code = jl_array_copy(new_ci->code);
        size_t clen = jl_array_len(new_code);
        for (int i = 0; i < clen; ++i) {
            jl_array_ptr_set(new_code, i, jl_copy_ast(
                jl_array_ptr_ref(new_code, i)
            ));
        }
        new_ci->code = new_code;
        jl_gc_wb(new_ci, new_code);
        new_ci->slotnames = jl_array_copy(new_ci->slotnames);
        jl_gc_wb(new_ci, new_ci->slotnames);
        new_ci->slotflags = jl_array_copy(new_ci->slotflags);
        jl_gc_wb(new_ci, new_ci->slotflags);
        new_ci->codelocs = (jl_value_t*)jl_array_copy((jl_array_t*)new_ci->codelocs);
        jl_gc_wb(new_ci, new_ci->codelocs);
        new_ci->linetable = (jl_value_t*)jl_array_copy((jl_array_t*)new_ci->linetable);
        jl_gc_wb(new_ci, new_ci->linetable);
        new_ci->ssaflags = jl_array_copy(new_ci->ssaflags);
        jl_gc_wb(new_ci, new_ci->ssaflags);

        if (new_ci->edges != jl_nothing) {
            new_ci->edges = (jl_value_t*)jl_array_copy((jl_array_t*)new_ci->edges);
            jl_gc_wb(new_ci, new_ci->edges);
        }

        if (jl_is_array(new_ci->ssavaluetypes)) {
            new_ci->ssavaluetypes = (jl_value_t*)jl_array_copy((jl_array_t*)new_ci->ssavaluetypes);
            jl_gc_wb(new_ci, new_ci->ssavaluetypes);
        }
        JL_GC_POP();
        return (jl_value_t*)new_ci;
    }
    if (jl_is_expr(expr)) {
        jl_expr_t *e = (jl_expr_t*)expr;
        size_t i, l = jl_array_len(e->args);
        jl_expr_t *ne = jl_exprn(e->head, l);
        JL_GC_PUSH2(&ne, &expr);
        for (i = 0; i < l; i++) {
            jl_value_t *a = jl_exprarg(e, i);
            jl_exprargset(ne, i, jl_copy_ast(a));
        }
        JL_GC_POP();
        return (jl_value_t*)ne;
    }
    if (jl_is_phinode(expr)) {
        jl_array_t *edges = (jl_array_t*)jl_fieldref_noalloc(expr, 0);
        jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(expr, 1);
        JL_GC_PUSH2(&edges, &values);
        edges = jl_array_copy(edges);
        values = jl_array_copy(values);
        jl_value_t *ret = jl_new_struct(jl_phinode_type, edges, values);
        JL_GC_POP();
        return ret;
    }
    if (jl_is_phicnode(expr)) {
        jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(expr, 0);
        JL_GC_PUSH1(&values);
        values = jl_array_copy(values);
        jl_value_t *ret = jl_new_struct(jl_phicnode_type, values);
        JL_GC_POP();
        return ret;
    }
    return expr;
}

JL_DLLEXPORT int jl_is_operator(const char *sym)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    int res = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "operator?")), symbol(fl_ctx, sym)) == fl_ctx->T;
    jl_ast_ctx_leave(ctx);
    return res;
}

JL_DLLEXPORT int jl_is_unary_operator(const char *sym)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    int res = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "unary-op?")), symbol(fl_ctx, sym)) == fl_ctx->T;
    jl_ast_ctx_leave(ctx);
    return res;
}

JL_DLLEXPORT int jl_is_unary_and_binary_operator(const char *sym)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    int res = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "unary-and-binary-op?")), symbol(fl_ctx, sym)) == fl_ctx->T;
    jl_ast_ctx_leave(ctx);
    return res;
}

JL_DLLEXPORT int jl_is_syntactic_operator(const char *sym)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    int res = fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "syntactic-op?")), symbol(fl_ctx, sym)) == fl_ctx->T;
    jl_ast_ctx_leave(ctx);
    return res;
}

JL_DLLEXPORT int jl_operator_precedence(const char *sym)
{
    jl_ast_context_t *ctx = jl_ast_ctx_enter(NULL);
    fl_context_t *fl_ctx = &ctx->fl;
    int res = numval(fl_applyn(fl_ctx, 1, symbol_value(symbol(fl_ctx, "operator-precedence")), symbol(fl_ctx, sym)));
    jl_ast_ctx_leave(ctx);
    return res;
}

int jl_has_meta(jl_array_t *body, jl_sym_t *sym) JL_NOTSAFEPOINT
{
    size_t i, l = jl_array_len(body);
    for (i = 0; i < l; i++) {
        jl_expr_t *stmt = (jl_expr_t*)jl_array_ptr_ref(body, i);
        if (jl_is_expr((jl_value_t*)stmt) && stmt->head == jl_meta_sym) {
            size_t i, l = jl_array_len(stmt->args);
            for (i = 0; i < l; i++)
                if (jl_array_ptr_ref(stmt->args, i) == (jl_value_t*)sym)
                    return 1;
        }
    }
    return 0;
}

// Utility function to return whether `e` is any of the special AST types or
// will always evaluate to itself exactly unchanged. This corresponds to
// `is_self_quoting` in Core.Compiler utilities.
int jl_is_ast_node(jl_value_t *e) JL_NOTSAFEPOINT
{
    return jl_is_newvarnode(e)
        || jl_is_code_info(e)
        || jl_is_linenode(e)
        || jl_is_gotonode(e)
        || jl_is_gotoifnot(e)
        || jl_is_returnnode(e)
        || jl_is_ssavalue(e)
        || jl_is_slotnumber(e)
        || jl_is_argument(e)
        || jl_is_quotenode(e)
        || jl_is_globalref(e)
        || jl_is_symbol(e)
        || jl_is_pinode(e)
        || jl_is_phinode(e)
        || jl_is_phicnode(e)
        || jl_is_upsilonnode(e)
        || jl_is_expr(e);
}

static int is_self_quoting_expr(jl_expr_t *e) JL_NOTSAFEPOINT
{
    return (e->head == jl_inert_sym ||
            e->head == jl_core_sym ||
            e->head == jl_line_sym ||
            e->head == jl_lineinfo_sym ||
            e->head == jl_meta_sym ||
            e->head == jl_boundscheck_sym ||
            e->head == jl_inline_sym ||
            e->head == jl_noinline_sym);
}

// any AST, except those that cannot contain symbols
// and have no side effects
int need_esc_node(jl_value_t *e) JL_NOTSAFEPOINT
{
    if (jl_is_linenode(e)
        || jl_is_ssavalue(e)
        || jl_is_slotnumber(e)
        || jl_is_argument(e)
        || jl_is_quotenode(e))
        return 0;
    if (jl_is_expr(e))
        return !is_self_quoting_expr((jl_expr_t*)e);
    // note: jl_is_globalref(e) is not included here, since we care a little about about having a line number for it
    return jl_is_ast_node(e);
}

static jl_value_t *jl_invoke_julia_macro(jl_array_t *args, jl_module_t *inmodule, jl_module_t **ctx, jl_value_t **lineinfo, size_t world, int throw_load_error)
{
    jl_task_t *ct = jl_current_task;
    JL_TIMING(MACRO_INVOCATION, MACRO_INVOCATION);
    size_t nargs = jl_array_len(args) + 1;
    JL_NARGSV("macrocall", 3); // macro name, location, and module
    jl_value_t **margs;
    JL_GC_PUSHARGS(margs, nargs);
    int i;
    margs[0] = jl_array_ptr_ref(args, 0);
    // __source__ argument
    jl_value_t *lno = jl_array_ptr_ref(args, 1);
    if (!jl_is_linenode(lno))
        lno = jl_new_struct(jl_linenumbernode_type, jl_box_long(0), jl_nothing);
    margs[1] = lno;
    margs[2] = (jl_value_t*)inmodule;
    for (i = 3; i < nargs; i++)
        margs[i] = jl_array_ptr_ref(args, i - 1);

    size_t last_age = ct->world_age;
    ct->world_age = jl_atomic_load_acquire(&jl_world_counter);
    if (ct->world_age > world)
        ct->world_age = world;
    jl_value_t *result;
    JL_TRY {
        margs[0] = jl_toplevel_eval(*ctx, margs[0]);
        jl_method_instance_t *mfunc = jl_method_lookup(margs, nargs, ct->world_age);
        JL_GC_PROMISE_ROOTED(mfunc);
        if (mfunc == NULL) {
            jl_method_error(margs[0], &margs[1], nargs, ct->world_age);
            // unreachable
        }
        jl_timing_show_macro(mfunc, margs[1], inmodule, JL_TIMING_DEFAULT_BLOCK);
        *ctx = mfunc->def.method->module;
        result = jl_invoke(margs[0], &margs[1], nargs - 1, mfunc);
    }
    JL_CATCH {
        if ((jl_loaderror_type == NULL) || !throw_load_error) {
            jl_rethrow();
        }
        else {
            jl_value_t *lno = margs[1];
            jl_value_t *file = jl_fieldref(lno, 1);
            if (jl_is_symbol(file))
                margs[0] = jl_cstr_to_string(jl_symbol_name((jl_sym_t*)file));
            else
                margs[0] = jl_cstr_to_string("<macrocall>");
            margs[1] = jl_fieldref(lno, 0); // extract and allocate line number
            jl_rethrow_other(jl_new_struct(jl_loaderror_type, margs[0], margs[1],
                                           jl_current_exception()));
        }
    }
    ct->world_age = last_age;
    *lineinfo = margs[1];
    JL_GC_POP();
    return result;
}

static jl_value_t *jl_expand_macros(jl_value_t *expr, jl_module_t *inmodule, struct macroctx_stack *macroctx, int onelevel, size_t world, int throw_load_error)
{
    if (!expr || !jl_is_expr(expr))
        return expr;
    jl_expr_t *e = (jl_expr_t*)expr;
    if (e->head == jl_inert_sym ||
        e->head == jl_module_sym ||
        //e->head == jl_toplevel_sym || // TODO: enable this once julia-expand-macroscope is fixed / removed
        e->head == jl_meta_sym) {
        return expr;
    }
    if (e->head == jl_quote_sym && jl_expr_nargs(e) == 1) {
        expr = jl_call_scm_on_ast("julia-bq-macro", jl_exprarg(e, 0), inmodule);
        JL_GC_PUSH1(&expr);
        expr = jl_expand_macros(expr, inmodule, macroctx, onelevel, world, throw_load_error);
        JL_GC_POP();
        return expr;
    }
    if (e->head == jl_hygienicscope_sym && jl_expr_nargs(e) >= 2) {
        struct macroctx_stack newctx;
        newctx.m = (jl_module_t*)jl_exprarg(e, 1);
        JL_TYPECHK(hygienic-scope, module, (jl_value_t*)newctx.m);
        newctx.parent = macroctx;
        jl_value_t *a = jl_exprarg(e, 0);
        jl_value_t *a2 = jl_expand_macros(a, inmodule, &newctx, onelevel, world, throw_load_error);
        if (jl_is_expr(a2) && ((jl_expr_t*)a2)->head == jl_escape_sym && !need_esc_node(jl_exprarg(a2, 0)))
            expr = jl_exprarg(a2, 0);
        else if (!need_esc_node(a2))
            expr = a2;
        else if (a != a2)
            jl_array_ptr_set(e->args, 0, a2);
        return expr;
    }
    if (e->head == jl_macrocall_sym) {
        struct macroctx_stack newctx;
        newctx.m = macroctx ? macroctx->m : inmodule;
        newctx.parent = macroctx;
        jl_value_t *lineinfo = NULL;
        jl_value_t *result = jl_invoke_julia_macro(e->args, inmodule, &newctx.m, &lineinfo, world, throw_load_error);
        if (!need_esc_node(result))
            return result;
        jl_value_t *wrap = NULL;
        JL_GC_PUSH4(&result, &wrap, &newctx.m, &lineinfo);
        // copy and wrap the result in `(hygienic-scope ,result ,newctx)
        if (jl_is_expr(result) && ((jl_expr_t*)result)->head == jl_escape_sym)
            result = jl_exprarg(result, 0);
        else
            wrap = (jl_value_t*)jl_exprn(jl_hygienicscope_sym, 3);
        result = jl_copy_ast(result);
        if (!onelevel)
            result = jl_expand_macros(result, inmodule, wrap ? &newctx : macroctx, onelevel, world, throw_load_error);
        if (wrap && need_esc_node(result)) {
            jl_exprargset(wrap, 0, result);
            jl_exprargset(wrap, 1, newctx.m);
            jl_exprargset(wrap, 2, lineinfo);
            if (jl_is_expr(result) && ((jl_expr_t*)result)->head == jl_escape_sym)
                result = jl_exprarg(result, 0);
            else
                result = wrap;
        }
        JL_GC_POP();
        return result;
    }
    if (e->head == jl_do_sym && jl_expr_nargs(e) == 2 && jl_is_expr(jl_exprarg(e, 0)) &&
        ((jl_expr_t*)jl_exprarg(e, 0))->head == jl_macrocall_sym) {
        jl_expr_t *mc = (jl_expr_t*)jl_exprarg(e, 0);
        size_t nm = jl_expr_nargs(mc);
        jl_expr_t *mc2 = jl_exprn(jl_macrocall_sym, nm+1);
        JL_GC_PUSH1(&mc2);
        jl_exprargset(mc2, 0, jl_exprarg(mc, 0));  // macro name
        jl_exprargset(mc2, 1, jl_exprarg(mc, 1));  // location
        jl_exprargset(mc2, 2, jl_exprarg(e, 1));   // function argument
        size_t j;
        for (j = 2; j < nm; j++) {
            jl_exprargset(mc2, j+1, jl_exprarg(mc, j));
        }
        jl_value_t *ret = jl_expand_macros((jl_value_t*)mc2, inmodule, macroctx, onelevel, world, throw_load_error);
        JL_GC_POP();
        return ret;
    }
    if (e->head == jl_escape_sym && macroctx) {
        macroctx = macroctx->parent;
    }

    size_t i;
    for (i = 0; i < jl_array_len(e->args); i++) {
        jl_value_t *a = jl_array_ptr_ref(e->args, i);
        jl_value_t *a2 = jl_expand_macros(a, inmodule, macroctx, onelevel, world, throw_load_error);
        if (a != a2)
            jl_array_ptr_set(e->args, i, a2);
    }
    return expr;
}

JL_DLLEXPORT jl_value_t *jl_macroexpand(jl_value_t *expr, jl_module_t *inmodule)
{
    JL_TIMING(LOWERING, LOWERING);
    JL_GC_PUSH1(&expr);
    expr = jl_copy_ast(expr);
    expr = jl_expand_macros(expr, inmodule, NULL, 0, jl_atomic_load_acquire(&jl_world_counter), 0);
    expr = jl_call_scm_on_ast("jl-expand-macroscope", expr, inmodule);
    JL_GC_POP();
    return expr;
}

JL_DLLEXPORT jl_value_t *jl_macroexpand1(jl_value_t *expr, jl_module_t *inmodule)
{
    JL_TIMING(LOWERING, LOWERING);
    JL_GC_PUSH1(&expr);
    expr = jl_copy_ast(expr);
    expr = jl_expand_macros(expr, inmodule, NULL, 1, jl_atomic_load_acquire(&jl_world_counter), 0);
    expr = jl_call_scm_on_ast("jl-expand-macroscope", expr, inmodule);
    JL_GC_POP();
    return expr;
}

// Lower an expression tree into Julia's intermediate-representation.
JL_DLLEXPORT jl_value_t *jl_expand(jl_value_t *expr, jl_module_t *inmodule)
{
    return jl_expand_with_loc(expr, inmodule, "none", 0);
}

// Lowering, with starting program location specified
JL_DLLEXPORT jl_value_t *jl_expand_with_loc(jl_value_t *expr, jl_module_t *inmodule,
                                            const char *file, int line)
{
    return jl_expand_in_world(expr, inmodule, file, line, ~(size_t)0);
}

// Lowering, with starting program location and worldage specified
JL_DLLEXPORT jl_value_t *jl_expand_in_world(jl_value_t *expr, jl_module_t *inmodule,
                                            const char *file, int line, size_t world)
{
    JL_TIMING(LOWERING, LOWERING);
    jl_timing_show_location(file, line, inmodule, JL_TIMING_DEFAULT_BLOCK);
    JL_GC_PUSH1(&expr);
    expr = jl_copy_ast(expr);
    expr = jl_expand_macros(expr, inmodule, NULL, 0, world, 1);
    expr = jl_call_scm_on_ast_and_loc("jl-expand-to-thunk", expr, inmodule, file, line);
    JL_GC_POP();
    return expr;
}

// Same as the above, but printing warnings when applicable
JL_DLLEXPORT jl_value_t *jl_expand_with_loc_warn(jl_value_t *expr, jl_module_t *inmodule,
                                                 const char *file, int line)
{
    JL_TIMING(LOWERING, LOWERING);
    jl_timing_show_location(file, line, inmodule, JL_TIMING_DEFAULT_BLOCK);
    jl_array_t *kwargs = NULL;
    JL_GC_PUSH2(&expr, &kwargs);
    expr = jl_copy_ast(expr);
    expr = jl_expand_macros(expr, inmodule, NULL, 0, ~(size_t)0, 1);
    jl_ast_context_t *ctx = jl_ast_ctx_enter(inmodule);
    fl_context_t *fl_ctx = &ctx->fl;
    value_t arg = julia_to_scm(fl_ctx, expr);
    value_t e = fl_applyn(fl_ctx, 4, symbol_value(symbol(fl_ctx, "jl-expand-to-thunk-warn")), arg,
                          symbol(fl_ctx, file), fixnum(line), fl_ctx->F);
    expr = scm_to_julia(fl_ctx, e, inmodule);
    jl_ast_ctx_leave(ctx);
    jl_sym_t *warn_sym = jl_symbol("warn");
    if (jl_is_expr(expr) && ((jl_expr_t*)expr)->head == warn_sym) {
        size_t nargs = jl_expr_nargs(expr);
        for (int i = 0; i < nargs - 1; i++) {
            jl_value_t *warning = jl_exprarg(expr, i);
            size_t nargs = 0;
            if (jl_is_expr(warning) && ((jl_expr_t*)warning)->head == warn_sym)
                 nargs = jl_expr_nargs(warning);
            int kwargs_len = (int)nargs - 6;
            if (nargs < 6 || kwargs_len % 2 != 0) {
                jl_error("julia-logmsg: bad argument list - expected "
                         ":warn level (symbol) group (symbol) id file line msg . kwargs");
            }
            jl_value_t *level = jl_exprarg(warning, 0);
            jl_value_t *group = jl_exprarg(warning, 1);
            jl_value_t *id = jl_exprarg(warning, 2);
            jl_value_t *file = jl_exprarg(warning, 3);
            jl_value_t *line = jl_exprarg(warning, 4);
            jl_value_t *msg = jl_exprarg(warning, 5);
            kwargs = jl_alloc_vec_any(kwargs_len);
            for (int i = 0; i < kwargs_len; ++i) {
                jl_array_ptr_set(kwargs, i, jl_exprarg(warning, i + 6));
            }
            JL_TYPECHK(logmsg, long, level);
            jl_log(jl_unbox_long(level), NULL, group, id, file, line, (jl_value_t*)kwargs, msg);
        }
        expr = jl_exprarg(expr, nargs - 1);
    }
    JL_GC_POP();
    return expr;
}

// expand in a context where the expression value is unused
JL_DLLEXPORT jl_value_t *jl_expand_stmt_with_loc(jl_value_t *expr, jl_module_t *inmodule,
                                                 const char *file, int line)
{
    JL_TIMING(LOWERING, LOWERING);
    JL_GC_PUSH1(&expr);
    expr = jl_copy_ast(expr);
    expr = jl_expand_macros(expr, inmodule, NULL, 0, ~(size_t)0, 1);
    expr = jl_call_scm_on_ast_and_loc("jl-expand-to-thunk-stmt", expr, inmodule, file, line);
    JL_GC_POP();
    return expr;
}

JL_DLLEXPORT jl_value_t *jl_expand_stmt(jl_value_t *expr, jl_module_t *inmodule)
{
    return jl_expand_stmt_with_loc(expr, inmodule, "none", 0);
}


//------------------------------------------------------------------------------
// Parsing API and utils for calling parser from runtime

// Internal C entry point to parser
// `text` is passed as a pointer to allow raw non-String buffers to be used
// without copying.
jl_value_t *jl_parse(const char *text, size_t text_len, jl_value_t *filename,
                     size_t lineno, size_t offset, jl_value_t *options)
{
    jl_value_t *core_parse = NULL;
    if (jl_core_module) {
        core_parse = jl_get_global(jl_core_module, jl_symbol("_parse"));
    }
    if (!core_parse || core_parse == jl_nothing) {
        // In bootstrap, directly call the builtin parser.
        jl_value_t *result = jl_fl_parse(text, text_len, filename, lineno, offset, options);
        return result;
    }
    jl_value_t **args;
    JL_GC_PUSHARGS(args, 6);
    args[0] = core_parse;
    args[1] = (jl_value_t*)jl_alloc_svec(2);
    jl_svecset(args[1], 0, jl_box_uint8pointer((uint8_t*)text));
    jl_svecset(args[1], 1, jl_box_long(text_len));
    args[2] = filename;
    args[3] = jl_box_long(lineno);
    args[4] = jl_box_long(offset);
    args[5] = options;
    jl_task_t *ct = jl_current_task;
    size_t last_age = ct->world_age;
    ct->world_age = jl_atomic_load_acquire(&jl_world_counter);
    jl_value_t *result = jl_apply(args, 6);
    ct->world_age = last_age;
    args[0] = result; // root during error checks below
    JL_TYPECHK(parse, simplevector, result);
    if (jl_svec_len(result) != 2)
        jl_error("Result from parser should be `svec(a::Expr, b::Int)`");
    JL_TYPECHK(parse, expr, jl_svecref(result, 0));
    JL_TYPECHK(parse, long, jl_svecref(result, 1));
    JL_GC_POP();
    return result;
}

// parse an entire string as a file, reading multiple expressions
JL_DLLEXPORT jl_value_t *jl_parse_all(const char *text, size_t text_len,
                                      const char *filename, size_t filename_len, size_t lineno)
{
    jl_value_t *fname = jl_pchar_to_string(filename, filename_len);
    JL_GC_PUSH1(&fname);
    jl_value_t *p = jl_parse(text, text_len, fname, lineno, 0, (jl_value_t*)jl_all_sym);
    JL_GC_POP();
    return jl_svecref(p, 0);
}

// this is for parsing one expression out of a string, keeping track of
// the current position.
JL_DLLEXPORT jl_value_t *jl_parse_string(const char *text, size_t text_len,
                                         int offset, int greedy)
{
    jl_value_t *fname = jl_cstr_to_string("none");
    JL_GC_PUSH1(&fname);
    jl_value_t *result = jl_parse(text, text_len, fname, 1, offset,
                                  (jl_value_t*)(greedy ? jl_statement_sym : jl_atom_sym));
    JL_GC_POP();
    return result;
}

// deprecated
JL_DLLEXPORT jl_value_t *jl_parse_input_line(const char *text, size_t text_len,
                                             const char *filename, size_t filename_len)
{
    return jl_parse_all(text, text_len, filename, filename_len, 1);
}

#ifdef __cplusplus
}
#endif
