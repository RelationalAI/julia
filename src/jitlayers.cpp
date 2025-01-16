// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include "platform.h"
#include <stdint.h>
#include <sstream>

#include "llvm/IR/Mangler.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Bitcode/BitcodeWriter.h>

// target machine computation
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Object/SymbolSize.h>

using namespace llvm;

#include "llvm-codegen-shared.h"
#include "jitlayers.h"
#include "julia_assert.h"
#include "processor.h"

# include <llvm/ExecutionEngine/Orc/DebuggerSupportPlugin.h>
# include <llvm/ExecutionEngine/JITLink/EHFrameSupport.h>
# include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
# if JL_LLVM_VERSION >= 150000
# include <llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h>
# endif
# include <llvm/ExecutionEngine/SectionMemoryManager.h>

#define DEBUG_TYPE "julia_jitlayers"

STATISTIC(LinkedGlobals, "Number of globals linked");
STATISTIC(CompiledCodeinsts, "Number of codeinsts compiled directly");
STATISTIC(MaxWorkqueueSize, "Maximum number of elements in the workqueue");
STATISTIC(IndirectCodeinsts, "Number of dependent codeinsts compiled");
STATISTIC(SpecFPtrCount, "Number of specialized function pointers compiled");
STATISTIC(UnspecFPtrCount, "Number of specialized function pointers compiled");
STATISTIC(ModulesAdded, "Number of modules added to the JIT");
STATISTIC(ModulesOptimized, "Number of modules optimized by the JIT");
STATISTIC(OptO0, "Number of modules optimized at level -O0");
STATISTIC(OptO1, "Number of modules optimized at level -O1");
STATISTIC(OptO2, "Number of modules optimized at level -O2");
STATISTIC(OptO3, "Number of modules optimized at level -O3");
STATISTIC(ModulesMerged, "Number of modules merged");
STATISTIC(InternedGlobals, "Number of global constants interned in the string pool");

#ifdef _COMPILER_MSAN_ENABLED_
// TODO: This should not be necessary on ELF x86_64, but LLVM's implementation
// of the TLS relocations is currently broken, so enable this unconditionally.
#define MSAN_EMUTLS_WORKAROUND 1

// See https://github.com/google/sanitizers/wiki/MemorySanitizerJIT
namespace msan_workaround {

extern "C" {
    extern __thread unsigned long long __msan_param_tls[];
    extern __thread unsigned int __msan_param_origin_tls[];
    extern __thread unsigned long long __msan_retval_tls[];
    extern __thread unsigned int __msan_retval_origin_tls;
    extern __thread unsigned long long __msan_va_arg_tls[];
    extern __thread unsigned int __msan_va_arg_origin_tls[];
    extern __thread unsigned long long __msan_va_arg_overflow_size_tls;
    extern __thread unsigned int __msan_origin_tls;
}

enum class MSanTLS
{
    param = 1,             // __msan_param_tls
    param_origin,          //__msan_param_origin_tls
    retval,                // __msan_retval_tls
    retval_origin,         //__msan_retval_origin_tls
    va_arg,                // __msan_va_arg_tls
    va_arg_origin,         // __msan_va_arg_origin_tls
    va_arg_overflow_size,  // __msan_va_arg_overflow_size_tls
    origin,                //__msan_origin_tls
};

static void *getTLSAddress(void *control)
{
    auto tlsIndex = static_cast<MSanTLS>(reinterpret_cast<uintptr_t>(control));
    switch(tlsIndex)
    {
    case MSanTLS::param: return reinterpret_cast<void *>(&__msan_param_tls);
    case MSanTLS::param_origin: return reinterpret_cast<void *>(&__msan_param_origin_tls);
    case MSanTLS::retval: return reinterpret_cast<void *>(&__msan_retval_tls);
    case MSanTLS::retval_origin: return reinterpret_cast<void *>(&__msan_retval_origin_tls);
    case MSanTLS::va_arg: return reinterpret_cast<void *>(&__msan_va_arg_tls);
    case MSanTLS::va_arg_origin: return reinterpret_cast<void *>(&__msan_va_arg_origin_tls);
    case MSanTLS::va_arg_overflow_size: return reinterpret_cast<void *>(&__msan_va_arg_overflow_size_tls);
    case MSanTLS::origin: return reinterpret_cast<void *>(&__msan_origin_tls);
    default:
        assert(false && "BAD MSAN TLS INDEX");
        return nullptr;
    }
}
}
#endif

// Snooping on which functions are being compiled, and how long it takes
extern "C" JL_DLLEXPORT_CODEGEN
void jl_dump_compiles_impl(void *s)
{
    **jl_ExecutionEngine->get_dump_compiles_stream() = (ios_t*)s;
}
extern "C" JL_DLLEXPORT_CODEGEN
void jl_dump_llvm_opt_impl(void *s)
{
    **jl_ExecutionEngine->get_dump_llvm_opt_stream() = (ios_t*)s;
}

static int jl_add_to_ee(
        orc::ThreadSafeModule &M,
        const StringMap<orc::ThreadSafeModule*> &NewExports,
        DenseMap<orc::ThreadSafeModule*, int> &Queued,
        std::vector<orc::ThreadSafeModule*> &Stack) JL_NOTSAFEPOINT;
static void jl_decorate_module(Module &M) JL_NOTSAFEPOINT;
static uint64_t getAddressForFunction(StringRef fname) JL_NOTSAFEPOINT;

void jl_link_global(GlobalVariable *GV, void *addr) JL_NOTSAFEPOINT
{
    ++LinkedGlobals;
    Constant *P = literal_static_pointer_val(addr, GV->getValueType());
    GV->setInitializer(P);
    GV->setDSOLocal(true);
    if (jl_options.image_codegen) {
        // If we are forcing imaging mode codegen for debugging,
        // emit external non-const symbol to avoid LLVM optimizing the code
        // similar to non-imaging mode.
        assert(GV->hasExternalLinkage());
    }
    else {
        GV->setConstant(true);
        GV->setLinkage(GlobalValue::PrivateLinkage);
        GV->setVisibility(GlobalValue::DefaultVisibility);
        GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    }
}

void jl_jit_globals(std::map<void *, GlobalVariable*> &globals) JL_NOTSAFEPOINT
{
    for (auto &global : globals) {
        jl_link_global(global.second, global.first);
    }
}

// used for image_codegen, where we keep all the gvs external
// so we can't jit them directly into each module
static orc::ThreadSafeModule jl_get_globals_module(orc::ThreadSafeContext &ctx, bool imaging_mode, const DataLayout &DL, const Triple &T, std::map<void *, GlobalVariable*> &globals) JL_NOTSAFEPOINT
{
    auto lock = ctx.getLock();
    auto GTSM = jl_create_ts_module("globals", ctx, imaging_mode, DL, T);
    auto GM = GTSM.getModuleUnlocked();
    for (auto &global : globals) {
        auto GV = global.second;
        auto GV2 = new GlobalVariable(*GM, GV->getValueType(), GV->isConstant(), GlobalValue::ExternalLinkage, literal_static_pointer_val(global.first, GV->getValueType()), GV->getName(), nullptr, GV->getThreadLocalMode(), GV->getAddressSpace(), false);
        GV2->copyAttributesFrom(GV);
        GV2->setDSOLocal(true);
        GV2->setAlignment(GV->getAlign());
    }
    return GTSM;
}

// this generates llvm code for the lambda info
// and adds the result to the jitlayers
// (and the shadow module),
// and generates code for it
static jl_callptr_t _jl_compile_codeinst(
        jl_code_instance_t *codeinst,
        jl_code_info_t *src,
        size_t world,
        orc::ThreadSafeContext context,
        bool is_recompile)
{
    // caller must hold codegen_lock
    // and have disabled finalizers
    uint64_t start_time = 0;
    bool timed = !!*jl_ExecutionEngine->get_dump_compiles_stream();
    if (timed)
        start_time = jl_hrtime();

    assert(jl_is_code_instance(codeinst));
    assert(codeinst->min_world <= world && (codeinst->max_world >= world || codeinst->max_world == 0) &&
        "invalid world for method-instance");

    JL_TIMING(CODEINST_COMPILE, CODEINST_COMPILE);
#ifdef USE_TRACY
    if (is_recompile) {
        TracyCZoneColor(JL_TIMING_DEFAULT_BLOCK->tracy_ctx, 0xFFA500);
    }
#endif
    jl_callptr_t fptr = NULL;
    // emit the code in LLVM IR form
    jl_codegen_params_t params(std::move(context), jl_ExecutionEngine->getDataLayout(), jl_ExecutionEngine->getTargetTriple()); // Locks the context
    params.cache = true;
    params.world = world;
    params.imaging = imaging_default();
    params.debug_level = jl_options.debug_level;
    {
        orc::ThreadSafeModule result_m =
            jl_create_ts_module(name_from_method_instance(codeinst->def), params.tsctx, params.imaging, params.DL, params.TargetTriple);
        jl_llvm_functions_t decls = jl_emit_codeinst(result_m, codeinst, src, params);
        if (result_m)
            params.compiled_functions[codeinst] = {std::move(result_m), std::move(decls)};
        {
            auto temp_module = jl_create_llvm_module(name_from_method_instance(codeinst->def), params.getContext(), params.imaging);
            jl_compile_workqueue(params, *temp_module, CompilationPolicy::Default);
        }

        if (params._shared_module)
            jl_ExecutionEngine->addModule(orc::ThreadSafeModule(std::move(params._shared_module), params.tsctx));

        // In imaging mode, we can't inline global variable initializers in order to preserve
        // the fiction that we don't know what loads from the global will return. Thus, we
        // need to emit a separate module for the globals before any functions are compiled,
        // to ensure that the globals are defined when they are compiled.
        if (params.imaging) {
            jl_ExecutionEngine->addModule(jl_get_globals_module(params.tsctx, params.imaging, params.DL, params.TargetTriple, params.globals));
        } else {
            StringMap<void*> NewGlobals;
            for (auto &global : params.globals) {
                NewGlobals[global.second->getName()] = global.first;
            }
            for (auto &def : params.compiled_functions) {
                auto M = std::get<0>(def.second).getModuleUnlocked();
                for (auto &GV : M->globals()) {
                    auto InitValue = NewGlobals.find(GV.getName());
                    if (InitValue != NewGlobals.end()) {
                        jl_link_global(&GV, InitValue->second);
                    }
                }
            }
        }

        // Collect the exported functions from the params.compiled_functions modules,
        // which form dependencies on which functions need to be
        // compiled first. Cycles of functions are compiled together.
        // (essentially we compile a DAG of SCCs in reverse topological order,
        // if we treat declarations of external functions as edges from declaration
        // to definition)
        StringMap<orc::ThreadSafeModule*> NewExports;
        for (auto &def : params.compiled_functions) {
            orc::ThreadSafeModule &TSM = std::get<0>(def.second);
            //The underlying context object is still locked because params is not destroyed yet
            auto M = TSM.getModuleUnlocked();
            for (auto &F : M->global_objects()) {
                if (!F.isDeclaration() && F.getLinkage() == GlobalValue::ExternalLinkage) {
                    NewExports[F.getName()] = &TSM;
                }
            }
        }
        DenseMap<orc::ThreadSafeModule*, int> Queued;
        std::vector<orc::ThreadSafeModule*> Stack;
        for (auto &def : params.compiled_functions) {
            // Add the results to the execution engine now
            orc::ThreadSafeModule &M = std::get<0>(def.second);
            jl_add_to_ee(M, NewExports, Queued, Stack);
            assert(Queued.empty() && Stack.empty() && !M);
        }
        ++CompiledCodeinsts;
        MaxWorkqueueSize.updateMax(params.compiled_functions.size());
        IndirectCodeinsts += params.compiled_functions.size() - 1;
    }

    size_t i = 0;
    for (auto &def : params.compiled_functions) {
        jl_code_instance_t *this_code = def.first;
        if (i < jl_timing_print_limit)
            jl_timing_show_func_sig(this_code->def->specTypes, JL_TIMING_DEFAULT_BLOCK);

        jl_llvm_functions_t decls = std::get<1>(def.second);
        jl_callptr_t addr;
        bool isspecsig = false;
        if (decls.functionObject == "jl_fptr_args") {
            addr = jl_fptr_args_addr;
        }
        else if (decls.functionObject == "jl_fptr_sparam") {
            addr = jl_fptr_sparam_addr;
        }
        else if (decls.functionObject == "jl_f_opaque_closure_call") {
            addr = jl_f_opaque_closure_call_addr;
        }
        else {
            addr = (jl_callptr_t)getAddressForFunction(decls.functionObject);
            isspecsig = true;
        }
        if (!decls.specFunctionObject.empty()) {
            void *prev_specptr = NULL;
            auto spec = (void*)getAddressForFunction(decls.specFunctionObject);
            if (jl_atomic_cmpswap_acqrel(&this_code->specptr.fptr, &prev_specptr, spec)) {
                // only set specsig and invoke if we were the first to set specptr
                jl_atomic_store_relaxed(&this_code->specsigflags, (uint8_t) isspecsig);
                // we might overwrite invokeptr here; that's ok, anybody who relied on the identity of invokeptr
                // either assumes that specptr was null, doesn't care about specptr,
                // or will wait until specsigflags has 0b10 set before reloading invoke
                jl_atomic_store_release(&this_code->invoke, addr);
                jl_atomic_store_release(&this_code->specsigflags, (uint8_t) (0b10 | isspecsig));
            } else {
                //someone else beat us, don't commit any results
                while (!(jl_atomic_load_acquire(&this_code->specsigflags) & 0b10)) {
                    jl_cpu_pause();
                }
                addr = jl_atomic_load_relaxed(&this_code->invoke);
            }
        } else {
            jl_callptr_t prev_invoke = NULL;
            if (!jl_atomic_cmpswap_acqrel(&this_code->invoke, &prev_invoke, addr)) {
                addr = prev_invoke;
                //TODO do we want to potentially promote invoke anyways? (e.g. invoke is jl_interpret_call or some other
                //known lesser function)
            }
        }
        if (this_code == codeinst)
            fptr = addr;
        i++;
    }
    if (i > jl_timing_print_limit)
        jl_timing_printf(JL_TIMING_DEFAULT_BLOCK, "... <%d methods truncated>", i - 10);

    uint64_t end_time = 0;
    if (timed)
        end_time = jl_hrtime();

    // If logging of the compilation stream is enabled,
    // then dump the method-instance specialization type to the stream
    jl_method_instance_t *mi = codeinst->def;
    if (jl_is_method(mi->def.method)) {
        auto stream = *jl_ExecutionEngine->get_dump_compiles_stream();
        if (stream) {
            ios_printf(stream, "%" PRIu64 "\t\"", end_time - start_time);
            jl_static_show((JL_STREAM*)stream, mi->specTypes);
            ios_printf(stream, "\"\n");
        }
    }
    return fptr;
}

const char *jl_generate_ccallable(LLVMOrcThreadSafeModuleRef llvmmod, void *sysimg_handle, jl_value_t *declrt, jl_value_t *sigt, jl_codegen_params_t &params);

// compile a C-callable alias
extern "C" JL_DLLEXPORT_CODEGEN
int jl_compile_extern_c_impl(LLVMOrcThreadSafeModuleRef llvmmod, void *p, void *sysimg, jl_value_t *declrt, jl_value_t *sigt)
{
    auto ct = jl_current_task;
    bool timed = (ct->reentrant_timing & 1) == 0;
    if (timed)
        ct->reentrant_timing |= 1;
    uint64_t compiler_start_time = 0;
    uint8_t measure_compile_time_enabled = jl_atomic_load_relaxed(&jl_measure_compile_time_enabled);
    if (measure_compile_time_enabled)
        compiler_start_time = jl_hrtime();
    orc::ThreadSafeContext ctx;
    auto into = unwrap(llvmmod);
    jl_codegen_params_t *pparams = (jl_codegen_params_t*)p;
    orc::ThreadSafeModule backing;
    if (into == NULL) {
        if (!pparams) {
            ctx = jl_ExecutionEngine->acquireContext();
        }
        backing = jl_create_ts_module("cextern", pparams ? pparams->tsctx : ctx, pparams ? pparams->imaging : imaging_default());
        into = &backing;
    }
    JL_LOCK(&jl_codegen_lock);
    auto target_info = into->withModuleDo([&](Module &M) {
        return std::make_pair(M.getDataLayout(), Triple(M.getTargetTriple()));
    });
    jl_codegen_params_t params(into->getContext(), std::move(target_info.first), std::move(target_info.second));
    params.imaging = imaging_default();
    params.debug_level = jl_options.debug_level;
    if (pparams == NULL)
        pparams = &params;
    assert(pparams->tsctx.getContext() == into->getContext().getContext());
    const char *name = jl_generate_ccallable(wrap(into), sysimg, declrt, sigt, *pparams);
    bool success = true;
    if (!sysimg) {
        if (jl_ExecutionEngine->getGlobalValueAddress(name)) {
            success = false;
        }
        if (success && p == NULL) {
            jl_jit_globals(params.globals);
            assert(params.workqueue.empty());
            if (params._shared_module)
                jl_ExecutionEngine->addModule(orc::ThreadSafeModule(std::move(params._shared_module), params.tsctx));
        }
        if (success && llvmmod == NULL)
            jl_ExecutionEngine->addModule(std::move(*into));
    }
    JL_UNLOCK(&jl_codegen_lock);
    if (timed) {
        if (measure_compile_time_enabled) {
            auto end = jl_hrtime();
            jl_atomic_fetch_add_relaxed(&jl_cumulative_compile_time, end - compiler_start_time);
        }
        ct->reentrant_timing &= ~1ull;
    }
    if (ctx.getContext()) {
        jl_ExecutionEngine->releaseContext(std::move(ctx));
    }
    return success;
}

// declare a C-callable entry point; called during code loading from the toplevel
extern "C" JL_DLLEXPORT_CODEGEN
void jl_extern_c_impl(jl_value_t *declrt, jl_tupletype_t *sigt)
{
    // validate arguments. try to do as many checks as possible here to avoid
    // throwing errors later during codegen.
    JL_TYPECHK(@ccallable, type, declrt);
    if (!jl_is_tuple_type(sigt))
        jl_type_error("@ccallable", (jl_value_t*)jl_anytuple_type_type, (jl_value_t*)sigt);
    // check that f is a guaranteed singleton type
    jl_datatype_t *ft = (jl_datatype_t*)jl_tparam0(sigt);
    if (!jl_is_datatype(ft) || ft->instance == NULL)
        jl_error("@ccallable: function object must be a singleton");

    // compute / validate return type
    if (!jl_is_concrete_type(declrt) || jl_is_kind(declrt))
        jl_error("@ccallable: return type must be concrete and correspond to a C type");
    if (!jl_type_mappable_to_c(declrt))
        jl_error("@ccallable: return type doesn't correspond to a C type");

    // validate method signature
    size_t i, nargs = jl_nparams(sigt);
    for (i = 1; i < nargs; i++) {
        jl_value_t *ati = jl_tparam(sigt, i);
        if (!jl_is_concrete_type(ati) || jl_is_kind(ati) || !jl_type_mappable_to_c(ati))
            jl_error("@ccallable: argument types must be concrete");
    }

    // save a record of this so that the alias is generated when we write an object file
    jl_method_t *meth = (jl_method_t*)jl_methtable_lookup(ft->name->mt, (jl_value_t*)sigt, jl_atomic_load_acquire(&jl_world_counter));
    if (!jl_is_method(meth))
        jl_error("@ccallable: could not find requested method");
    JL_GC_PUSH1(&meth);
    meth->ccallable = jl_svec2(declrt, (jl_value_t*)sigt);
    jl_gc_wb(meth, meth->ccallable);
    JL_GC_POP();

    // create the alias in the current runtime environment
    int success = jl_compile_extern_c(NULL, NULL, NULL, declrt, (jl_value_t*)sigt);
    if (!success)
        jl_error("@ccallable was already defined for this method name");
}

// this compiles li and emits fptr
extern "C" JL_DLLEXPORT_CODEGEN
jl_code_instance_t *jl_generate_fptr_impl(jl_method_instance_t *mi JL_PROPAGATES_ROOT, size_t world, int *did_compile)
{
    if (did_compile != NULL)
        *did_compile = 0;
    auto ct = jl_current_task;
    bool timed = (ct->reentrant_timing & 1) == 0;
    if (timed)
        ct->reentrant_timing |= 1;
    uint64_t compiler_start_time = 0;
    uint8_t measure_compile_time_enabled = jl_atomic_load_relaxed(&jl_measure_compile_time_enabled);
    bool is_recompile = false;
    if (measure_compile_time_enabled)
        compiler_start_time = jl_hrtime();
    // if we don't have any decls already, try to generate it now
    jl_code_info_t *src = NULL;
    jl_code_instance_t *codeinst = NULL;
    JL_GC_PUSH2(&src, &codeinst);
    JL_LOCK(&jl_codegen_lock); // also disables finalizers, to prevent any unexpected recursion
    jl_value_t *ci = jl_rettype_inferred_addr(mi, world, world);
    if (ci != jl_nothing)
        codeinst = (jl_code_instance_t*)ci;
    if (codeinst) {
        src = (jl_code_info_t*)jl_atomic_load_relaxed(&codeinst->inferred);
        if ((jl_value_t*)src == jl_nothing)
            src = NULL;
        else if (jl_is_method(mi->def.method))
            src = jl_uncompress_ir(mi->def.method, codeinst, (jl_value_t*)src);
    }
    else {
        // identify whether this is an invalidated method that is being recompiled
        is_recompile = jl_atomic_load_relaxed(&mi->cache) != NULL;
    }
    if (src == NULL && jl_is_method(mi->def.method) &&
             jl_symbol_name(mi->def.method->name)[0] != '@') {
        if (mi->def.method->source != jl_nothing) {
            // If the caller didn't provide the source and IR is available,
            // see if it is inferred, or try to infer it for ourself.
            // (but don't bother with typeinf on macros or toplevel thunks)
            src = jl_type_infer(mi, world, 0);
        }
    }
    jl_code_instance_t *compiled = jl_method_compiled(mi, world);
    if (compiled) {
        codeinst = compiled;
    }
    else if (src && jl_is_code_info(src)) {
        if (!codeinst) {
            codeinst = jl_get_method_inferred(mi, src->rettype, src->min_world, src->max_world);
            if (src->inferred) {
                jl_value_t *null = nullptr;
                jl_atomic_cmpswap_relaxed(&codeinst->inferred, &null, jl_nothing);
            }
        }
        ++SpecFPtrCount;
        _jl_compile_codeinst(codeinst, src, world, *jl_ExecutionEngine->getContext(), is_recompile);
        if (jl_atomic_load_relaxed(&codeinst->invoke) == NULL)
            codeinst = NULL;
        else if (did_compile != NULL)
            *did_compile = 1;
    }
    else {
        codeinst = NULL;
    }
    JL_UNLOCK(&jl_codegen_lock);
    if (timed) {
        if (measure_compile_time_enabled) {
            uint64_t t_comp = jl_hrtime() - compiler_start_time;
            if (is_recompile) {
                jl_atomic_fetch_add_relaxed(&jl_cumulative_recompile_time, t_comp);
            }
            jl_atomic_fetch_add_relaxed(&jl_cumulative_compile_time, t_comp);
        }
        ct->reentrant_timing &= ~1ull;
    }
    JL_GC_POP();
    return codeinst;
}

extern "C" JL_DLLEXPORT_CODEGEN
void jl_generate_fptr_for_oc_wrapper_impl(jl_code_instance_t *oc_wrap)
{
    if (jl_atomic_load_relaxed(&oc_wrap->invoke) != NULL) {
        return;
    }
    JL_LOCK(&jl_codegen_lock);
    if (jl_atomic_load_relaxed(&oc_wrap->invoke) == NULL) {
        _jl_compile_codeinst(oc_wrap, NULL, 1, *jl_ExecutionEngine->getContext(), 0);
    }
    JL_UNLOCK(&jl_codegen_lock); // Might GC
}

extern "C" JL_DLLEXPORT_CODEGEN
void jl_generate_fptr_for_unspecialized_impl(jl_code_instance_t *unspec)
{
    if (jl_atomic_load_relaxed(&unspec->invoke) != NULL) {
        return;
    }
    auto ct = jl_current_task;
    bool timed = (ct->reentrant_timing & 1) == 0;
    if (timed)
        ct->reentrant_timing |= 1;
    uint64_t compiler_start_time = 0;
    uint8_t measure_compile_time_enabled = jl_atomic_load_relaxed(&jl_measure_compile_time_enabled);
    if (measure_compile_time_enabled)
        compiler_start_time = jl_hrtime();
    JL_LOCK(&jl_codegen_lock);
    if (jl_atomic_load_relaxed(&unspec->invoke) == NULL) {
        jl_code_info_t *src = NULL;
        JL_GC_PUSH1(&src);
        jl_method_t *def = unspec->def->def.method;
        if (jl_is_method(def)) {
            src = (jl_code_info_t*)def->source;
            if (src && (jl_value_t*)src != jl_nothing)
                src = jl_uncompress_ir(def, NULL, (jl_value_t*)src);
        }
        else {
            src = (jl_code_info_t*)jl_atomic_load_relaxed(&unspec->def->uninferred);
            assert(src);
        }
        if (src) {
            assert(jl_is_code_info(src));
            ++UnspecFPtrCount;
            _jl_compile_codeinst(unspec, src, unspec->min_world, *jl_ExecutionEngine->getContext(), 0);
        }
        jl_callptr_t null = nullptr;
        // if we hit a codegen bug (or ran into a broken generated function or llvmcall), fall back to the interpreter as a last resort
        jl_atomic_cmpswap(&unspec->invoke, &null, jl_fptr_interpret_call_addr);
        JL_GC_POP();
    }
    JL_UNLOCK(&jl_codegen_lock); // Might GC
    if (timed) {
        if (measure_compile_time_enabled) {
            auto end = jl_hrtime();
            jl_atomic_fetch_add_relaxed(&jl_cumulative_compile_time, end - compiler_start_time);
        }
        ct->reentrant_timing &= ~1ull;
    }
}


// get a native disassembly for a compiled method
extern "C" JL_DLLEXPORT_CODEGEN
jl_value_t *jl_dump_method_asm_impl(jl_method_instance_t *mi, size_t world,
        char emit_mc, char getwrapper, const char* asm_variant, const char *debuginfo, char binary)
{
    // printing via disassembly
    jl_code_instance_t *codeinst = jl_generate_fptr(mi, world, NULL);
    if (codeinst) {
        uintptr_t fptr = (uintptr_t)jl_atomic_load_acquire(&codeinst->invoke);
        if (getwrapper)
            return jl_dump_fptr_asm(fptr, emit_mc, asm_variant, debuginfo, binary);
        uintptr_t specfptr = (uintptr_t)jl_atomic_load_relaxed(&codeinst->specptr.fptr);
        if (fptr == (uintptr_t)jl_fptr_const_return_addr && specfptr == 0) {
            // normally we prevent native code from being generated for these functions,
            // (using sentinel value `1` instead)
            // so create an exception here so we can print pretty our lies
            auto ct = jl_current_task;
            bool timed = (ct->reentrant_timing & 1) == 0;
            if (timed)
                ct->reentrant_timing |= 1;
            uint64_t compiler_start_time = 0;
            uint8_t measure_compile_time_enabled = jl_atomic_load_relaxed(&jl_measure_compile_time_enabled);
            if (measure_compile_time_enabled)
                compiler_start_time = jl_hrtime();
            JL_LOCK(&jl_codegen_lock); // also disables finalizers, to prevent any unexpected recursion
            specfptr = (uintptr_t)jl_atomic_load_relaxed(&codeinst->specptr.fptr);
            if (specfptr == 0) {
                jl_code_info_t *src = jl_type_infer(mi, world, 0);
                JL_GC_PUSH1(&src);
                jl_method_t *def = mi->def.method;
                if (jl_is_method(def)) {
                    if (!src) {
                        // TODO: jl_code_for_staged can throw
                        src = def->generator ? jl_code_for_staged(mi, world) : (jl_code_info_t*)def->source;
                    }
                    if (src && (jl_value_t*)src != jl_nothing)
                        src = jl_uncompress_ir(mi->def.method, codeinst, (jl_value_t*)src);
                }
                fptr = (uintptr_t)jl_atomic_load_acquire(&codeinst->invoke);
                specfptr = (uintptr_t)jl_atomic_load_relaxed(&codeinst->specptr.fptr);
                if (src && jl_is_code_info(src)) {
                    if (fptr == (uintptr_t)jl_fptr_const_return_addr && specfptr == 0) {
                        fptr = (uintptr_t)_jl_compile_codeinst(codeinst, src, world, *jl_ExecutionEngine->getContext(), 0);
                        specfptr = (uintptr_t)jl_atomic_load_relaxed(&codeinst->specptr.fptr);
                    }
                }
                JL_GC_POP();
            }
            JL_UNLOCK(&jl_codegen_lock);
            if (timed) {
                if (measure_compile_time_enabled) {
                    auto end = jl_hrtime();
                    jl_atomic_fetch_add_relaxed(&jl_cumulative_compile_time, end - compiler_start_time);
                }
                ct->reentrant_timing &= ~1ull;
            }
        }
        if (specfptr != 0)
            return jl_dump_fptr_asm(specfptr, emit_mc, asm_variant, debuginfo, binary);
    }

    // whatever, that didn't work - use the assembler output instead
    jl_llvmf_dump_t llvmf_dump;
    jl_get_llvmf_defn(&llvmf_dump, mi, world, getwrapper, true, jl_default_cgparams);
    if (!llvmf_dump.F)
        return jl_an_empty_string;
    return jl_dump_function_asm(&llvmf_dump, emit_mc, asm_variant, debuginfo, binary, false);
}

CodeGenOpt::Level CodeGenOptLevelFor(int optlevel)
{
#ifdef DISABLE_OPT
    return CodeGenOpt::None;
#else
    return optlevel == 0 ? CodeGenOpt::None :
        optlevel == 1 ? CodeGenOpt::Less :
        optlevel == 2 ? CodeGenOpt::Default :
        CodeGenOpt::Aggressive;
#endif
}

static auto countBasicBlocks(const Function &F) JL_NOTSAFEPOINT
{
    return std::distance(F.begin(), F.end());
}

void JuliaOJIT::OptSelLayerT::emit(std::unique_ptr<orc::MaterializationResponsibility> R, orc::ThreadSafeModule TSM) {
    ++ModulesOptimized;
    size_t optlevel = SIZE_MAX;
    TSM.withModuleDo([&](Module &M) {
        if (jl_generating_output()) {
            optlevel = 0;
        }
        else {
            optlevel = std::max(static_cast<int>(jl_options.opt_level), 0);
            size_t optlevel_min = std::max(static_cast<int>(jl_options.opt_level_min), 0);
            for (auto &F : M.functions()) {
                if (!F.getBasicBlockList().empty()) {
                    Attribute attr = F.getFnAttribute("julia-optimization-level");
                    StringRef val = attr.getValueAsString();
                    if (val != "") {
                        size_t ol = (size_t)val[0] - '0';
                        if (ol < optlevel)
                            optlevel = ol;
                    }
                    attr = F.getFnAttribute("julia-min-optimization-level");
                    val = attr.getValueAsString();
                    if (val != "") {
                        size_t ol = (size_t)val[0] - '0';
                        if (ol > opt_level)
                            opt_level = ol;
                        jl_safe_printf("Function %s opt: %d\n", F.getName().str().c_str(), (int)opt_level);
                    }
                }
            }
            optlevel = std::min(std::max(optlevel, optlevel_min), this->count);
        }
    });
    assert(optlevel != SIZE_MAX && "Failed to select a valid optimization level!");
    this->optimizers[optlevel]->OptimizeLayer.emit(std::move(R), std::move(TSM));
}

void jl_register_jit_object(const object::ObjectFile &debugObj,
                            std::function<uint64_t(const StringRef &)> getLoadAddress,
                            std::function<void *(void *)> lookupWriteAddress) JL_NOTSAFEPOINT;

namespace {

using namespace llvm::orc;

struct JITObjectInfo {
    std::unique_ptr<MemoryBuffer> BackingBuffer;
    std::unique_ptr<object::ObjectFile> Object;
    StringMap<uint64_t> SectionLoadAddresses;
};

class JLDebuginfoPlugin : public ObjectLinkingLayer::Plugin {
    std::mutex PluginMutex;
    std::map<MaterializationResponsibility *, std::unique_ptr<JITObjectInfo>> PendingObjs;
    // Resources from distinct MaterializationResponsibilitys can get merged
    // after emission, so we can have multiple debug objects per resource key.
    std::map<ResourceKey, std::vector<std::unique_ptr<JITObjectInfo>>> RegisteredObjs;

public:
    void notifyMaterializing(MaterializationResponsibility &MR, jitlink::LinkGraph &G,
                             jitlink::JITLinkContext &Ctx,
                             MemoryBufferRef InputObject) override
    {
        // Keeping around a full copy of the input object file (and re-parsing it) is
        // wasteful, but for now, this lets us reuse the existing debuginfo.cpp code.
        // Should look into just directly pulling out all the information required in
        // a JITLink pass and just keeping the required tables/DWARF sections around
        // (perhaps using the LLVM DebuggerSupportPlugin as a reference).
        auto NewBuffer =
            MemoryBuffer::getMemBufferCopy(InputObject.getBuffer(), G.getName());
        auto NewObj =
            cantFail(object::ObjectFile::createObjectFile(NewBuffer->getMemBufferRef()));

        {
            std::lock_guard<std::mutex> lock(PluginMutex);
            assert(PendingObjs.count(&MR) == 0);
            PendingObjs[&MR] = std::unique_ptr<JITObjectInfo>(
                new JITObjectInfo{std::move(NewBuffer), std::move(NewObj), {}});
        }
    }

    Error notifyEmitted(MaterializationResponsibility &MR) override
    {
        {
            std::lock_guard<std::mutex> lock(PluginMutex);
            auto It = PendingObjs.find(&MR);
            if (It == PendingObjs.end())
                return Error::success();

            auto NewInfo = PendingObjs[&MR].get();
            auto getLoadAddress = [NewInfo](const StringRef &Name) -> uint64_t {
                auto result = NewInfo->SectionLoadAddresses.find(Name);
                if (result == NewInfo->SectionLoadAddresses.end()) {
                    LLVM_DEBUG({
                        dbgs() << "JLDebuginfoPlugin: No load address found for section '"
                            << Name << "'\n";
                    });
                    return 0;
                }
                return result->second;
            };

            jl_register_jit_object(*NewInfo->Object, getLoadAddress, nullptr);
        }

        cantFail(MR.withResourceKeyDo([&](ResourceKey K) {
            std::lock_guard<std::mutex> lock(PluginMutex);
            RegisteredObjs[K].push_back(std::move(PendingObjs[&MR]));
            PendingObjs.erase(&MR);
        }));

        return Error::success();
    }

    Error notifyFailed(MaterializationResponsibility &MR) override
    {
        std::lock_guard<std::mutex> lock(PluginMutex);
        PendingObjs.erase(&MR);
        return Error::success();
    }

    Error notifyRemovingResources(ResourceKey K) override
    {
        std::lock_guard<std::mutex> lock(PluginMutex);
        RegisteredObjs.erase(K);
        // TODO: If we ever unload code, need to notify debuginfo registry.
        return Error::success();
    }

    void notifyTransferringResources(ResourceKey DstKey, ResourceKey SrcKey) override
    {
        std::lock_guard<std::mutex> lock(PluginMutex);
        auto SrcIt = RegisteredObjs.find(SrcKey);
        if (SrcIt != RegisteredObjs.end()) {
            for (std::unique_ptr<JITObjectInfo> &Info : SrcIt->second)
                RegisteredObjs[DstKey].push_back(std::move(Info));
            RegisteredObjs.erase(SrcIt);
        }
    }

    void modifyPassConfig(MaterializationResponsibility &MR, jitlink::LinkGraph &,
                          jitlink::PassConfiguration &PassConfig) override
    {
        std::lock_guard<std::mutex> lock(PluginMutex);
        auto It = PendingObjs.find(&MR);
        if (It == PendingObjs.end())
            return;

        JITObjectInfo &Info = *It->second;
        PassConfig.PostAllocationPasses.push_back([&Info, this](jitlink::LinkGraph &G) -> Error {
            std::lock_guard<std::mutex> lock(PluginMutex);
            for (const jitlink::Section &Sec : G.sections()) {
#if defined(_OS_DARWIN_)
                // Canonical JITLink section names have the segment name included, e.g.
                // "__TEXT,__text" or "__DWARF,__debug_str". There are some special internal
                // sections without a comma separator, which we can just ignore.
                size_t SepPos = Sec.getName().find(',');
                if (SepPos >= 16 || (Sec.getName().size() - (SepPos + 1) > 16)) {
                    LLVM_DEBUG({
                        dbgs() << "JLDebuginfoPlugin: Ignoring section '" << Sec.getName()
                               << "'\n";
                    });
                    continue;
                }
                auto SecName = Sec.getName().substr(SepPos + 1);
#else
                auto SecName = Sec.getName();
#endif
                // https://github.com/llvm/llvm-project/commit/118e953b18ff07d00b8f822dfbf2991e41d6d791
               Info.SectionLoadAddresses[SecName] = jitlink::SectionRange(Sec).getStart().getValue();
            }
            return Error::success();
        });
    }
};

class JLMemoryUsagePlugin : public ObjectLinkingLayer::Plugin {
private:
    std::atomic<size_t> &total_size;

public:

    JLMemoryUsagePlugin(std::atomic<size_t> &total_size)
        : total_size(total_size) {}

    Error notifyFailed(orc::MaterializationResponsibility &MR) override {
        return Error::success();
    }
    Error notifyRemovingResources(orc::ResourceKey K) override {
        return Error::success();
    }
    void notifyTransferringResources(orc::ResourceKey DstKey,
                                     orc::ResourceKey SrcKey) override {}

    void modifyPassConfig(orc::MaterializationResponsibility &,
                          jitlink::LinkGraph &,
                          jitlink::PassConfiguration &Config) override {
        Config.PostAllocationPasses.push_back([this](jitlink::LinkGraph &G) {
            size_t graph_size = 0;
            size_t code_size = 0;
            size_t data_size = 0;
            for (auto block : G.blocks()) {
                graph_size += block->getSize();
            }
            for (auto &section : G.sections()) {
                size_t secsize = 0;
                for (auto block : section.blocks()) {
                    secsize += block->getSize();
                }
                if ((section.getMemProt() & jitlink::MemProt::Exec) == jitlink::MemProt::None) {
                    data_size += secsize;
                } else {
                    code_size += secsize;
                }
                graph_size += secsize;
            }
            (void) code_size;
            (void) data_size;
            this->total_size.fetch_add(graph_size, std::memory_order_relaxed);
            jl_timing_counter_inc(JL_TIMING_COUNTER_JITSize, graph_size);
            jl_timing_counter_inc(JL_TIMING_COUNTER_JITCodeSize, code_size);
            jl_timing_counter_inc(JL_TIMING_COUNTER_JITDataSize, data_size);
            return Error::success();
        });
    }
};

// replace with [[maybe_unused]] when we get to C++17
#ifdef _COMPILER_GCC_
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#ifdef _COMPILER_CLANG_
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

// TODO: Port our memory management optimisations to JITLink instead of using the
// default InProcessMemoryManager.
std::unique_ptr<jitlink::JITLinkMemoryManager> createJITLinkMemoryManager() {
#if JL_LLVM_VERSION < 150000
    return cantFail(jitlink::InProcessMemoryManager::Create());
#else
    return cantFail(orc::MapperJITLinkMemoryManager::CreateWithMapper<orc::InProcessMemoryMapper>());
#endif
}

#ifdef _COMPILER_CLANG_
#pragma clang diagnostic pop
#endif

#ifdef _COMPILER_GCC_
#pragma GCC diagnostic pop
#endif
}

class JLEHFrameRegistrar final : public jitlink::EHFrameRegistrar {
public:
    Error registerEHFrames(orc::ExecutorAddrRange EHFrameSection) override {
        register_eh_frames(EHFrameSection.Start.toPtr<uint8_t *>(), static_cast<size_t>(EHFrameSection.size()));
        return Error::success();
    }

    Error deregisterEHFrames(orc::ExecutorAddrRange EHFrameSection) override {
        deregister_eh_frames(EHFrameSection.Start.toPtr<uint8_t *>(), static_cast<size_t>(EHFrameSection.size()));
        return Error::success();
    }
};

RTDyldMemoryManager* createRTDyldMemoryManager(void);

// A simple forwarding class, since OrcJIT v2 needs a unique_ptr, while we have a shared_ptr
class ForwardingMemoryManager : public RuntimeDyld::MemoryManager {
private:
    std::shared_ptr<RuntimeDyld::MemoryManager> MemMgr;

public:
    ForwardingMemoryManager(std::shared_ptr<RuntimeDyld::MemoryManager> MemMgr) : MemMgr(MemMgr) {}
    virtual ~ForwardingMemoryManager() = default;
    virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                     unsigned SectionID,
                                     StringRef SectionName) override {
        return MemMgr->allocateCodeSection(Size, Alignment, SectionID, SectionName);
    }
    virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                     unsigned SectionID,
                                     StringRef SectionName,
                                     bool IsReadOnly) override {
        return MemMgr->allocateDataSection(Size, Alignment, SectionID, SectionName, IsReadOnly);
    }
    virtual void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
                                        uintptr_t RODataSize,
                                        uint32_t RODataAlign,
                                        uintptr_t RWDataSize,
                                        uint32_t RWDataAlign) override {
        return MemMgr->reserveAllocationSpace(CodeSize, CodeAlign, RODataSize, RODataAlign, RWDataSize, RWDataAlign);
    }
    virtual bool needsToReserveAllocationSpace() override {
        return MemMgr->needsToReserveAllocationSpace();
    }
    virtual void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                  size_t Size) override {
        return MemMgr->registerEHFrames(Addr, LoadAddr, Size);
    }
    virtual void deregisterEHFrames() override {
        return MemMgr->deregisterEHFrames();
    }
    virtual bool finalizeMemory(std::string *ErrMsg = nullptr) override {
        return MemMgr->finalizeMemory(ErrMsg);
    }
    virtual void notifyObjectLoaded(RuntimeDyld &RTDyld,
                                    const object::ObjectFile &Obj) override {
        return MemMgr->notifyObjectLoaded(RTDyld, Obj);
    }
};


#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
void *lookupWriteAddressFor(RTDyldMemoryManager *MemMgr, void *rt_addr);
#endif

void registerRTDyldJITObject(const object::ObjectFile &Object,
                             const RuntimeDyld::LoadedObjectInfo &L,
                             const std::shared_ptr<RTDyldMemoryManager> &MemMgr)
{
    auto SavedObject = L.getObjectForDebug(Object).takeBinary();
    // If the debug object is unavailable, save (a copy of) the original object
    // for our backtraces.
    // This copy seems unfortunate, but there doesn't seem to be a way to take
    // ownership of the original buffer.
    if (!SavedObject.first) {
        auto NewBuffer =
            MemoryBuffer::getMemBufferCopy(Object.getData(), Object.getFileName());
        auto NewObj =
            cantFail(object::ObjectFile::createObjectFile(NewBuffer->getMemBufferRef()));
        SavedObject = std::make_pair(std::move(NewObj), std::move(NewBuffer));
    }
    const object::ObjectFile *DebugObj = SavedObject.first.release();
    SavedObject.second.release();

    StringMap<object::SectionRef> loadedSections;
    // Use the original Object, not the DebugObject, as this is used for the
    // RuntimeDyld::LoadedObjectInfo lookup.
    for (const object::SectionRef &lSection : Object.sections()) {
        auto sName = lSection.getName();
        if (sName) {
            bool inserted = loadedSections.insert(std::make_pair(*sName, lSection)).second;
            assert(inserted);
            (void)inserted;
        }
    }
    auto getLoadAddress = [loadedSections = std::move(loadedSections),
                           &L](const StringRef &sName) -> uint64_t {
        auto search = loadedSections.find(sName);
        if (search == loadedSections.end())
            return 0;
        return L.getSectionLoadAddress(search->second);
    };

    jl_register_jit_object(*DebugObj, getLoadAddress,
#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
        [MemMgr](void *p) { return lookupWriteAddressFor(MemMgr.get(), p); }
#else
        nullptr
#endif
    );
}
namespace {
    static std::unique_ptr<TargetMachine> createTargetMachine() JL_NOTSAFEPOINT {
        TargetOptions options = TargetOptions();

        Triple TheTriple(sys::getProcessTriple());
        // use ELF because RuntimeDyld COFF i686 support didn't exist
        // use ELF because RuntimeDyld COFF X86_64 doesn't seem to work (fails to generate function pointers)?
        bool force_elf = TheTriple.isOSWindows();
#ifdef FORCE_ELF
        force_elf = true;
#endif
        if (force_elf) {
            TheTriple.setObjectFormat(Triple::ELF);
        }
        //options.PrintMachineCode = true; //Print machine code produced during JIT compiling
#if defined(MSAN_EMUTLS_WORKAROUND)
        options.EmulatedTLS = true;
        options.ExplicitEmulatedTLS = true;
#endif
        uint32_t target_flags = 0;
        auto target = jl_get_llvm_target(imaging_default(), target_flags);
        auto &TheCPU = target.first;
        SmallVector<std::string, 10> targetFeatures(target.second.begin(), target.second.end());
        std::string errorstr;
        const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, errorstr);
        if (!TheTarget) {
            jl_errorf("Internal problem with process triple %s lookup: %s", TheTriple.str().c_str(), errorstr.c_str());
            return nullptr;
        }
        if (jl_processor_print_help || (target_flags & JL_TARGET_UNKNOWN_NAME)) {
            std::unique_ptr<MCSubtargetInfo> MSTI(
                TheTarget->createMCSubtargetInfo(TheTriple.str(), "", ""));
            if (!MSTI->isCPUStringValid(TheCPU)) {
                jl_errorf("Invalid CPU name \"%s\".", TheCPU.c_str());
                return nullptr;
            }
            if (jl_processor_print_help) {
                // This is the only way I can find to print the help message once.
                // It'll be nice if we can iterate through the features and print our own help
                // message...
                MSTI->setDefaultFeatures("help", "", "");
            }
        }
        // Package up features to be passed to target/subtarget
        std::string FeaturesStr;
        if (!targetFeatures.empty()) {
            SubtargetFeatures Features;
            for (unsigned i = 0; i != targetFeatures.size(); ++i)
                Features.AddFeature(targetFeatures[i]);
            FeaturesStr = Features.getString();
        }
        // Allocate a target...
        Optional<CodeModel::Model> codemodel =
#ifdef _P64
            // Make sure we are using the large code model on 64bit
            // Let LLVM pick a default suitable for jitting on 32bit
            CodeModel::Large;
#else
            None;
#endif
        auto optlevel = CodeGenOptLevelFor(jl_options.opt_level);
        auto TM = TheTarget->createTargetMachine(
                TheTriple.getTriple(), TheCPU, FeaturesStr,
                options,
                Reloc::Static, // Generate simpler code for JIT
                codemodel,
                optlevel,
                true // JIT
                );
        assert(TM && "Failed to select target machine -"
                     " Is the LLVM backend for this CPU enabled?");
        if (!TheTriple.isARM() && !TheTriple.isPPC64()) {
            // FastISel seems to be buggy for ARM. Ref #13321
            if (jl_options.opt_level < 2)
                TM->setFastISel(true);
        }
        return std::unique_ptr<TargetMachine>(TM);
    }
} // namespace

namespace {

#ifndef JL_USE_NEW_PM
    typedef legacy::PassManager PassManager;
#else
    typedef NewPM PassManager;
#endif

    orc::JITTargetMachineBuilder createJTMBFromTM(TargetMachine &TM, int optlevel) JL_NOTSAFEPOINT {
        return orc::JITTargetMachineBuilder(TM.getTargetTriple())
            .setCPU(TM.getTargetCPU().str())
            .setFeatures(TM.getTargetFeatureString())
            .setOptions(TM.Options)
            .setRelocationModel(Reloc::Static)
            .setCodeModel(TM.getCodeModel())
            .setCodeGenOptLevel(CodeGenOptLevelFor(optlevel));
    }

    struct TMCreator {
        orc::JITTargetMachineBuilder JTMB;

        TMCreator(TargetMachine &TM, int optlevel) JL_NOTSAFEPOINT
            : JTMB(createJTMBFromTM(TM, optlevel)) {}

        std::unique_ptr<TargetMachine> operator()() JL_NOTSAFEPOINT {
            return cantFail(JTMB.createTargetMachine());
        }
    };

#ifndef JL_USE_NEW_PM
    struct PMCreator {
        std::unique_ptr<TargetMachine> TM;
        int optlevel;
        PMCreator(TargetMachine &TM, int optlevel) JL_NOTSAFEPOINT
            : TM(cantFail(createJTMBFromTM(TM, optlevel).createTargetMachine())), optlevel(optlevel) {}
        // overload for newpm compatibility
        PMCreator(TargetMachine &TM, int optlevel, std::vector<std::function<void()>> &) JL_NOTSAFEPOINT
            : PMCreator(TM, optlevel) {}
        PMCreator(const PMCreator &other) JL_NOTSAFEPOINT
            : PMCreator(*other.TM, other.optlevel) {}
        PMCreator(PMCreator &&other) JL_NOTSAFEPOINT
            : TM(std::move(other.TM)), optlevel(other.optlevel) {}
        friend void swap(PMCreator &self, PMCreator &other) JL_NOTSAFEPOINT {
            using std::swap;
            swap(self.TM, other.TM);
            swap(self.optlevel, other.optlevel);
        }
        PMCreator &operator=(PMCreator other) JL_NOTSAFEPOINT {
            swap(*this, other);
            return *this;
        }
        auto operator()() JL_NOTSAFEPOINT {
            auto PM = std::make_unique<legacy::PassManager>();
            addTargetPasses(PM.get(), TM->getTargetTriple(), TM->getTargetIRAnalysis());
            addOptimizationPasses(PM.get(), optlevel);
            addMachinePasses(PM.get(), optlevel);
            return PM;
        }
    };
#else
    struct PMCreator {
        orc::JITTargetMachineBuilder JTMB;
        OptimizationLevel O;
        std::vector<std::function<void()>> &printers;
        PMCreator(TargetMachine &TM, int optlevel, std::vector<std::function<void()>> &printers) JL_NOTSAFEPOINT
            : JTMB(createJTMBFromTM(TM, optlevel)), O(getOptLevel(optlevel)), printers(printers) {}

        auto operator()() JL_NOTSAFEPOINT {
            auto NPM = std::make_unique<NewPM>(cantFail(JTMB.createTargetMachine()), O);
            printers.push_back([NPM = NPM.get()]() JL_NOTSAFEPOINT {
                NPM->printTimers();
            });
            return NPM;
        }
    };
#endif

    struct OptimizerT {
        OptimizerT(TargetMachine &TM, int optlevel, std::vector<std::function<void()>> &printers) JL_NOTSAFEPOINT
            : optlevel(optlevel), PMs(PMCreator(TM, optlevel, printers)) {}
        OptimizerT(OptimizerT&) JL_NOTSAFEPOINT = delete;
        OptimizerT(OptimizerT&&) JL_NOTSAFEPOINT = default;

        OptimizerResultT operator()(orc::ThreadSafeModule TSM, orc::MaterializationResponsibility &R) JL_NOTSAFEPOINT {
            TSM.withModuleDo([&](Module &M) JL_NOTSAFEPOINT {
                uint64_t start_time = 0;
                std::stringstream before_stats_ss;
                bool should_dump_opt_stats = false;
                {
                    auto stream = *jl_ExecutionEngine->get_dump_llvm_opt_stream();
                    if (stream) {
                        // Ensures that we don't _just_ write the second part of the YAML object
                        should_dump_opt_stats = true;
                        // We use a stringstream to later atomically write a YAML object
                        // without the need to hold the stream lock over the optimization
                        // Print LLVM function statistics _before_ optimization
                        // Print all the information about this invocation as a YAML object
                        before_stats_ss << "- \n";
                        // We print the name and some statistics for each function in the module, both
                        // before optimization and again afterwards.
                        before_stats_ss << "  before: \n";
                        for (auto &F : M.functions()) {
                            if (F.isDeclaration() || F.getName().startswith("jfptr_")) {
                                continue;
                            }
                            // Each function is printed as a YAML object with several attributes
                            before_stats_ss << "    \"" << F.getName().str().c_str() << "\":\n";
                            before_stats_ss << "        instructions: " << F.getInstructionCount() << "\n";
                            before_stats_ss << "        basicblocks: " << countBasicBlocks(F) << "\n";
                        }

                        start_time = jl_hrtime();
                    }
                }

                JL_TIMING(LLVM_OPT, LLVM_OPT);

                //Run the optimization
                assert(!verifyModule(M, &errs()));
                (***PMs).run(M);
                assert(!verifyModule(M, &errs()));

                uint64_t end_time = 0;
                {
                    auto stream = *jl_ExecutionEngine->get_dump_llvm_opt_stream();
                    if (stream && should_dump_opt_stats) {
                        ios_printf(stream, "%s", before_stats_ss.str().c_str());
                        end_time = jl_hrtime();
                        ios_printf(stream, "  time_ns: %" PRIu64 "\n", end_time - start_time);
                        ios_printf(stream, "  optlevel: %d\n", optlevel);

                        // Print LLVM function statistics _after_ optimization
                        ios_printf(stream, "  after: \n");
                        for (auto &F : M.functions()) {
                            if (F.isDeclaration() || F.getName().startswith("jfptr_")) {
                                continue;
                            }
                            ios_printf(stream, "    \"%s\":\n", F.getName().str().c_str());
                            ios_printf(stream, "        instructions: %u\n", F.getInstructionCount());
                            ios_printf(stream, "        basicblocks: %zd\n", countBasicBlocks(F));
                        }
                    }
                }
            });
            switch (optlevel) {
                case 0:
                    ++OptO0;
                    break;
                case 1:
                    ++OptO1;
                    break;
                case 2:
                    ++OptO2;
                    break;
                case 3:
                    ++OptO3;
                    break;
                default:
                    llvm_unreachable("optlevel is between 0 and 3!");
            }
            return Expected<orc::ThreadSafeModule>{std::move(TSM)};
        }
    private:
        int optlevel;
        JuliaOJIT::ResourcePool<std::unique_ptr<PassManager>> PMs;
    };

    struct CompilerT : orc::IRCompileLayer::IRCompiler {

        CompilerT(orc::IRSymbolMapper::ManglingOptions MO, TargetMachine &TM, int optlevel) JL_NOTSAFEPOINT
            : orc::IRCompileLayer::IRCompiler(MO), TMs(TMCreator(TM, optlevel)) {}

        Expected<std::unique_ptr<MemoryBuffer>> operator()(Module &M) override {
            return orc::SimpleCompiler(***TMs)(M);
        }

        JuliaOJIT::ResourcePool<std::unique_ptr<TargetMachine>> TMs;
    };

    struct JITPointersT {

        JITPointersT(SharedBytesT &SharedBytes, std::mutex &Lock) JL_NOTSAFEPOINT
            : SharedBytes(SharedBytes), Lock(Lock) {}

        void operator()(Module &M) JL_NOTSAFEPOINT {
            std::lock_guard<std::mutex> locked(Lock);
            for (auto &GV : make_early_inc_range(M.globals())) {
                if (auto *Shared = getSharedBytes(GV)) {
                    ++InternedGlobals;
                    GV.replaceAllUsesWith(Shared);
                    GV.eraseFromParent();
                }
            }

            // Windows needs some inline asm to help
            // build unwind tables
            jl_decorate_module(M);
        }

    private:
        // optimize memory by turning long strings into memoized copies, instead of
        // making a copy per object file of output.
        // we memoize them using a StringSet with a custom-alignment allocator
        // to ensure they are properly aligned
        Constant *getSharedBytes(GlobalVariable &GV) JL_NOTSAFEPOINT {
            // We could probably technically get away with
            // interning even external linkage globals,
            // as long as they have global unnamedaddr,
            // but currently we shouldn't be emitting those
            // except in imaging mode, and we don't want to
            // do this optimization there.
            if (GV.hasExternalLinkage() || !GV.hasGlobalUnnamedAddr()) {
                return nullptr;
            }
            if (!GV.hasInitializer()) {
                return nullptr;
            }
            if (!GV.isConstant()) {
                return nullptr;
            }
            auto CDS = dyn_cast<ConstantDataSequential>(GV.getInitializer());
            if (!CDS) {
                return nullptr;
            }
            StringRef Data = CDS->getRawDataValues();
            if (Data.size() < 16) {
                // Cutoff, since we don't want to intern small strings
                return nullptr;
            }
            Align Required = GV.getAlign().valueOrOne();
            Align Preferred = MaxAlignedAlloc::alignment(Data.size());
            if (Required > Preferred)
                return nullptr;
            StringRef Interned = SharedBytes.insert(Data).first->getKey();
            assert(llvm::isAddrAligned(Preferred, Interned.data()));
            return literal_static_pointer_val(Interned.data(), GV.getType());
        }

        SharedBytesT &SharedBytes;
        std::mutex &Lock;
    };
}

llvm::DataLayout jl_create_datalayout(TargetMachine &TM) {
    // Mark our address spaces as non-integral
    auto jl_data_layout = TM.createDataLayout();
    jl_data_layout.reset(jl_data_layout.getStringRepresentation() + "-ni:10:11:12:13");
    return jl_data_layout;
}

JuliaOJIT::PipelineT::PipelineT(orc::ObjectLayer &BaseLayer, TargetMachine &TM, int optlevel, std::vector<std::function<void()>> &PrintLLVMTimers)
  : CompileLayer(BaseLayer.getExecutionSession(), BaseLayer,
      std::make_unique<CompilerT>(orc::irManglingOptionsFromTargetOptions(TM.Options), TM, optlevel)),
    OptimizeLayer(CompileLayer.getExecutionSession(), CompileLayer,
            llvm::orc::IRTransformLayer::TransformFunction(OptimizerT(TM, optlevel, PrintLLVMTimers))) {}

#ifdef _COMPILER_ASAN_ENABLED_
int64_t ___asan_globals_registered;
#endif

JuliaOJIT::JuliaOJIT()
  : TM(createTargetMachine()),
    DL(jl_create_datalayout(*TM)),
    ES(cantFail(orc::SelfExecutorProcessControl::Create())),
    GlobalJD(ES.createBareJITDylib("JuliaGlobals")),
    JD(ES.createBareJITDylib("JuliaOJIT")),
    ExternalJD(ES.createBareJITDylib("JuliaExternal")),
    ContextPool([](){
        auto ctx = std::make_unique<LLVMContext>();
        if (!ctx->hasSetOpaquePointersValue())
#ifndef JL_LLVM_OPAQUE_POINTERS
            ctx->setOpaquePointers(false);
#else
            ctx->setOpaquePointers(true);
#endif
        return orc::ThreadSafeContext(std::move(ctx));
    }),
#ifdef JL_USE_JITLINK
    MemMgr(createJITLinkMemoryManager()),
    ObjectLayer(ES, *MemMgr),
#else
    MemMgr(createRTDyldMemoryManager()),
    ObjectLayer(
            ES,
            [this]() {
                std::unique_ptr<RuntimeDyld::MemoryManager> result(new ForwardingMemoryManager(MemMgr));
                return result;
            }
        ),
#endif
    LockLayer(ObjectLayer),
    Pipelines{
        std::make_unique<PipelineT>(LockLayer, *TM, 0, PrintLLVMTimers),
        std::make_unique<PipelineT>(LockLayer, *TM, 1, PrintLLVMTimers),
        std::make_unique<PipelineT>(LockLayer, *TM, 2, PrintLLVMTimers),
        std::make_unique<PipelineT>(LockLayer, *TM, 3, PrintLLVMTimers),
    },
    OptSelLayer(Pipelines),
    ExternalCompileLayer(ES, LockLayer,
        std::make_unique<CompilerT>(orc::irManglingOptionsFromTargetOptions(TM->Options), *TM, 2))
{
#ifdef JL_USE_JITLINK
# if defined(LLVM_SHLIB)
    // When dynamically linking against LLVM, use our custom EH frame registration code
    // also used with RTDyld to inform both our and the libc copy of libunwind.
    auto ehRegistrar = std::make_unique<JLEHFrameRegistrar>();
# else
    auto ehRegistrar = std::make_unique<jitlink::InProcessEHFrameRegistrar>();
# endif
    ObjectLayer.addPlugin(std::make_unique<EHFrameRegistrationPlugin>(
        ES, std::move(ehRegistrar)));

    ObjectLayer.addPlugin(std::make_unique<JLDebuginfoPlugin>());
    ObjectLayer.addPlugin(std::make_unique<JLMemoryUsagePlugin>(total_size));
#else
    ObjectLayer.setNotifyLoaded(
        [this](orc::MaterializationResponsibility &MR,
               const object::ObjectFile &Object,
               const RuntimeDyld::LoadedObjectInfo &LO) {
            registerRTDyldJITObject(Object, LO, MemMgr);
        });
#endif

    std::string ErrorStr;

    // Make sure that libjulia-internal is loaded and placed first in the
    // DynamicLibrary order so that calls to runtime intrinsics are resolved
    // to the correct library when multiple libjulia-*'s have been loaded
    // (e.g. when we `ccall` into a PackageCompiler.jl-created shared library)
    sys::DynamicLibrary libjulia_internal_dylib = sys::DynamicLibrary::addPermanentLibrary(
      jl_libjulia_internal_handle, &ErrorStr);
    if(!ErrorStr.empty())
        report_fatal_error(llvm::Twine("FATAL: unable to dlopen libjulia-internal\n") + ErrorStr);

    // Make sure SectionMemoryManager::getSymbolAddressInProcess can resolve
    // symbols in the program as well. The nullptr argument to the function
    // tells DynamicLibrary to load the program, not a library.
    if (sys::DynamicLibrary::LoadLibraryPermanently(nullptr, &ErrorStr))
        report_fatal_error(llvm::Twine("FATAL: unable to dlopen self\n") + ErrorStr);

    GlobalJD.addGenerator(
      std::make_unique<orc::DynamicLibrarySearchGenerator>(
        libjulia_internal_dylib,
        DL.getGlobalPrefix(),
        orc::DynamicLibrarySearchGenerator::SymbolPredicate()));

    GlobalJD.addGenerator(
      cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        DL.getGlobalPrefix())));

    // Resolve non-lock free atomic functions in the libatomic1 library.
    // This is the library that provides support for c11/c++11 atomic operations.
    auto TT = getTargetTriple();
    const char *const libatomic = TT.isOSLinux() || TT.isOSFreeBSD() ?
        "libatomic.so.1" : TT.isOSWindows() ?
        "libatomic-1.dll" : nullptr;
    if (libatomic) {
        static void *atomic_hdl = jl_load_dynamic_library(libatomic, JL_RTLD_LOCAL, 0);
        if (atomic_hdl != NULL) {
            GlobalJD.addGenerator(
              cantFail(orc::DynamicLibrarySearchGenerator::Load(
                  libatomic,
                  DL.getGlobalPrefix(),
                  [&](const orc::SymbolStringPtr &S) {
                        const char *const atomic_prefix = "__atomic_";
                        return (*S).startswith(atomic_prefix);
                  })));
        }
    }

    JD.addToLinkOrder(GlobalJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly);
    JD.addToLinkOrder(ExternalJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly);
    ExternalJD.addToLinkOrder(GlobalJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly);
    ExternalJD.addToLinkOrder(JD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly);

#if JULIA_FLOAT16_ABI == 1
    orc::SymbolAliasMap jl_crt = {
        { mangle("__gnu_h2f_ieee"), { mangle("julia__gnu_h2f_ieee"), JITSymbolFlags::Exported } },
        { mangle("__extendhfsf2"),  { mangle("julia__gnu_h2f_ieee"), JITSymbolFlags::Exported } },
        { mangle("__gnu_f2h_ieee"), { mangle("julia__gnu_f2h_ieee"), JITSymbolFlags::Exported } },
        { mangle("__truncsfhf2"),   { mangle("julia__gnu_f2h_ieee"), JITSymbolFlags::Exported } },
        { mangle("__truncdfhf2"),   { mangle("julia__truncdfhf2"),   JITSymbolFlags::Exported } }
    };
    cantFail(GlobalJD.define(orc::symbolAliases(jl_crt)));
#endif

#ifdef MSAN_EMUTLS_WORKAROUND
    orc::SymbolMap msan_crt;
    msan_crt[mangle("__emutls_get_address")] = JITEvaluatedSymbol::fromPointer(msan_workaround::getTLSAddress, JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_param_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::param)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_param_origin_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::param_origin)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_retval_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::retval)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_retval_origin_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::retval_origin)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_va_arg_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::va_arg)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_va_arg_origin_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::va_arg_origin)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_va_arg_overflow_size_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::va_arg_overflow_size)), JITSymbolFlags::Exported);
    msan_crt[mangle("__emutls_v.__msan_origin_tls")] = JITEvaluatedSymbol::fromPointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(msan_workaround::MSanTLS::origin)), JITSymbolFlags::Exported);
    cantFail(GlobalJD.define(orc::absoluteSymbols(msan_crt)));
#endif
#ifdef _COMPILER_ASAN_ENABLED_
    orc::SymbolMap asan_crt;
    asan_crt[mangle("___asan_globals_registered")] = JITEvaluatedSymbol::fromPointer(&___asan_globals_registered, JITSymbolFlags::Exported);
    cantFail(JD.define(orc::absoluteSymbols(asan_crt)));
#endif
}

JuliaOJIT::~JuliaOJIT() = default;

orc::SymbolStringPtr JuliaOJIT::mangle(StringRef Name)
{
    std::string MangleName = getMangledName(Name);
    return ES.intern(MangleName);
}

void JuliaOJIT::addGlobalMapping(StringRef Name, uint64_t Addr)
{
    cantFail(JD.define(orc::absoluteSymbols({{mangle(Name), JITEvaluatedSymbol::fromPointer((void*)Addr)}})));
}

void JuliaOJIT::addModule(orc::ThreadSafeModule TSM)
{
    JL_TIMING(LLVM_ORC, LLVM_ORC);
    ++ModulesAdded;
    orc::SymbolLookupSet NewExports;
    TSM.withModuleDo([&](Module &M) JL_NOTSAFEPOINT {
        JITPointersT(SharedBytes, RLST_mutex)(M);
        for (auto &F : M.global_values()) {
            if (!F.isDeclaration() && F.getLinkage() == GlobalValue::ExternalLinkage) {
                auto Name = ES.intern(getMangledName(F.getName()));
                NewExports.add(std::move(Name));
            }
        }
#if !defined(JL_NDEBUG) && !defined(JL_USE_JITLINK)
        // validate the relocations for M (not implemented for the JITLink memory manager yet)
        for (Module::global_object_iterator I = M.global_objects().begin(), E = M.global_objects().end(); I != E; ) {
            GlobalObject *F = &*I;
            ++I;
            if (F->isDeclaration()) {
                if (F->use_empty())
                    F->eraseFromParent();
                else if (!((isa<Function>(F) && isIntrinsicFunction(cast<Function>(F))) ||
                        findUnmangledSymbol(F->getName()) ||
                        SectionMemoryManager::getSymbolAddressInProcess(
                            getMangledName(F->getName())))) {
                    llvm::errs() << "FATAL ERROR: "
                                << "Symbol \"" << F->getName().str() << "\""
                                << "not found";
                    abort();
                }
            }
        }
#endif
    });

    // TODO: what is the performance characteristics of this?
    cantFail(OptSelLayer.add(JD, std::move(TSM)));
    // force eager compilation (for now), due to memory management specifics
    // (can't handle compilation recursion)
    for (auto &sym : cantFail(ES.lookup({{&JD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}}, NewExports))) {
        assert(sym.second);
        (void) sym;
    }
}

Error JuliaOJIT::addExternalModule(orc::JITDylib &JD, orc::ThreadSafeModule TSM, bool ShouldOptimize)
{
    if (auto Err = TSM.withModuleDo([&](Module &M) JL_NOTSAFEPOINT -> Error
            {
            if (M.getDataLayout().isDefault())
                M.setDataLayout(DL);
            if (M.getDataLayout() != DL)
                return make_error<StringError>(
                    "Added modules have incompatible data layouts: " +
                    M.getDataLayout().getStringRepresentation() + " (module) vs " +
                    DL.getStringRepresentation() + " (jit)",
                inconvertibleErrorCode());

            return Error::success();
            }))
        return Err;
    return ExternalCompileLayer.add(JD.getDefaultResourceTracker(), std::move(TSM));
}

Error JuliaOJIT::addObjectFile(orc::JITDylib &JD, std::unique_ptr<MemoryBuffer> Obj) {
    assert(Obj && "Can not add null object");
    return LockLayer.add(JD.getDefaultResourceTracker(), std::move(Obj));
}

JL_JITSymbol JuliaOJIT::findSymbol(StringRef Name, bool ExportedSymbolsOnly)
{
    orc::JITDylib* SearchOrders[3] = {&JD, &GlobalJD, &ExternalJD};
    ArrayRef<orc::JITDylib*> SearchOrder = makeArrayRef(&SearchOrders[0], ExportedSymbolsOnly ? 3 : 1);
    auto Sym = ES.lookup(SearchOrder, Name);
    if (Sym)
        return *Sym;
    return Sym.takeError();
}

JL_JITSymbol JuliaOJIT::findUnmangledSymbol(StringRef Name)
{
    return findSymbol(getMangledName(Name), true);
}

Expected<JITEvaluatedSymbol> JuliaOJIT::findExternalJDSymbol(StringRef Name, bool ExternalJDOnly)
{
    orc::JITDylib* SearchOrders[3] = {&ExternalJD, &GlobalJD, &JD};
    ArrayRef<orc::JITDylib*> SearchOrder = makeArrayRef(&SearchOrders[0], ExternalJDOnly ? 1 : 3);
    auto Sym = ES.lookup(SearchOrder, getMangledName(Name));
    return Sym;
}

uint64_t JuliaOJIT::getGlobalValueAddress(StringRef Name)
{
    auto addr = findSymbol(getMangledName(Name), false);
    if (!addr) {
        consumeError(addr.takeError());
        return 0;
    }
    return cantFail(addr.getAddress());
}

uint64_t JuliaOJIT::getFunctionAddress(StringRef Name)
{
    auto addr = findSymbol(getMangledName(Name), false);
    if (!addr) {
        consumeError(addr.takeError());
        return 0;
    }
    return cantFail(addr.getAddress());
}

StringRef JuliaOJIT::getFunctionAtAddress(uint64_t Addr, jl_code_instance_t *codeinst)
{
    std::lock_guard<std::mutex> lock(RLST_mutex);
    std::string *fname = &ReverseLocalSymbolTable[(void*)(uintptr_t)Addr];
    if (fname->empty()) {
        std::string string_fname;
        raw_string_ostream stream_fname(string_fname);
        // try to pick an appropriate name that describes it
        jl_callptr_t invoke = jl_atomic_load_relaxed(&codeinst->invoke);
        if (Addr == (uintptr_t)invoke) {
            stream_fname << "jsysw_";
        }
        else if (invoke == jl_fptr_args_addr) {
            stream_fname << "jsys1_";
        }
        else if (invoke == jl_fptr_sparam_addr) {
            stream_fname << "jsys3_";
        }
        else {
            stream_fname << "jlsys_";
        }
        const char* unadorned_name = jl_symbol_name(codeinst->def->def.method->name);
        stream_fname << unadorned_name << "_" << RLST_inc++;
        *fname = std::move(stream_fname.str()); // store to ReverseLocalSymbolTable
        addGlobalMapping(*fname, Addr);
    }
    return *fname;
}


#ifdef JL_USE_JITLINK
extern "C" orc::shared::CWrapperFunctionResult
llvm_orc_registerJITLoaderGDBAllocAction(const char *Data, size_t Size);

void JuliaOJIT::enableJITDebuggingSupport()
{
    orc::SymbolMap GDBFunctions;
    GDBFunctions[mangle("llvm_orc_registerJITLoaderGDBAllocAction")] = JITEvaluatedSymbol::fromPointer(&llvm_orc_registerJITLoaderGDBAllocAction, JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    GDBFunctions[mangle("llvm_orc_registerJITLoaderGDBWrapper")] = JITEvaluatedSymbol::fromPointer(&llvm_orc_registerJITLoaderGDBWrapper, JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    cantFail(JD.define(orc::absoluteSymbols(GDBFunctions)));
    if (TM->getTargetTriple().isOSBinFormatMachO())
        ObjectLayer.addPlugin(cantFail(orc::GDBJITDebugInfoRegistrationPlugin::Create(ES, JD, TM->getTargetTriple())));
    else if (TM->getTargetTriple().isOSBinFormatELF())
        //EPCDebugObjectRegistrar doesn't take a JITDylib, so we have to directly provide the call address
        ObjectLayer.addPlugin(std::make_unique<orc::DebugObjectManagerPlugin>(ES, std::make_unique<orc::EPCDebugObjectRegistrar>(ES, orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper))));
}
#else
void JuliaOJIT::enableJITDebuggingSupport()
{
    RegisterJITEventListener(JITEventListener::createGDBRegistrationListener());
}

void JuliaOJIT::RegisterJITEventListener(JITEventListener *L)
{
    if (!L)
        return;
    this->ObjectLayer.registerJITEventListener(*L);
}
#endif

const DataLayout& JuliaOJIT::getDataLayout() const
{
    return DL;
}

std::string JuliaOJIT::getMangledName(StringRef Name)
{
    SmallString<128> FullName;
    Mangler::getNameWithPrefix(FullName, Name, DL);
    return FullName.str().str();
}

std::string JuliaOJIT::getMangledName(const GlobalValue *GV)
{
    return getMangledName(GV->getName());
}

#ifdef JL_USE_JITLINK
size_t JuliaOJIT::getTotalBytes() const
{
    return total_size.load(std::memory_order_relaxed);
}
#else
size_t getRTDyldMemoryManagerTotalBytes(RTDyldMemoryManager *mm) JL_NOTSAFEPOINT;

size_t JuliaOJIT::getTotalBytes() const
{
    return getRTDyldMemoryManagerTotalBytes(MemMgr.get());
}
#endif

void JuliaOJIT::printTimers()
{
#ifdef JL_USE_NEW_PM
    for (auto &printer : PrintLLVMTimers) {
        printer();
    }
#endif
    reportAndResetTimings();
}

JuliaOJIT *jl_ExecutionEngine;

// destructively move the contents of src into dest
// this assumes that the targets of the two modules are the same
// including the DataLayout and ModuleFlags (for example)
// and that there is no module-level assembly
// Comdat is also removed, since the JIT doesn't need it
void jl_merge_module(orc::ThreadSafeModule &destTSM, orc::ThreadSafeModule srcTSM)
{
    ++ModulesMerged;
    destTSM.withModuleDo([&](Module &dest) JL_NOTSAFEPOINT {
        srcTSM.withModuleDo([&](Module &src) JL_NOTSAFEPOINT {
            assert(&dest != &src && "Cannot merge module with itself!");
            assert(&dest.getContext() == &src.getContext() && "Cannot merge modules with different contexts!");
            assert(dest.getDataLayout() == src.getDataLayout() && "Cannot merge modules with different data layouts!");
            assert(dest.getTargetTriple() == src.getTargetTriple() && "Cannot merge modules with different target triples!");

            for (Module::global_iterator I = src.global_begin(), E = src.global_end(); I != E;) {
                GlobalVariable *sG = &*I;
                GlobalVariable *dG = cast_or_null<GlobalVariable>(dest.getNamedValue(sG->getName()));
                ++I;
                // Replace a declaration with the definition:
                if (dG) {
                    if (sG->isDeclaration()) {
                        sG->replaceAllUsesWith(dG);
                        sG->eraseFromParent();
                        continue;
                    }
                    //// If we start using llvm.used, we need to enable and test this
                    //else if (!dG->isDeclaration() && dG->hasAppendingLinkage() && sG->hasAppendingLinkage()) {
                    //    auto *dCA = cast<ConstantArray>(dG->getInitializer());
                    //    auto *sCA = cast<ConstantArray>(sG->getInitializer());
                    //    SmallVector<Constant *, 16> Init;
                    //    for (auto &Op : dCA->operands())
                    //        Init.push_back(cast_or_null<Constant>(Op));
                    //    for (auto &Op : sCA->operands())
                    //        Init.push_back(cast_or_null<Constant>(Op));
                    //    Type *Int8PtrTy = Type::getInt8PtrTy(dest.getContext());
                    //    ArrayType *ATy = ArrayType::get(Int8PtrTy, Init.size());
                    //    GlobalVariable *GV = new GlobalVariable(dest, ATy, dG->isConstant(),
                    //            GlobalValue::AppendingLinkage, ConstantArray::get(ATy, Init), "",
                    //            dG->getThreadLocalMode(), dG->getType()->getAddressSpace());
                    //    GV->copyAttributesFrom(dG);
                    //    sG->replaceAllUsesWith(GV);
                    //    dG->replaceAllUsesWith(GV);
                    //    GV->takeName(sG);
                    //    sG->eraseFromParent();
                    //    dG->eraseFromParent();
                    //    continue;
                    //}
                    else {
                        assert(dG->isDeclaration() || dG->getInitializer() == sG->getInitializer());
                        dG->replaceAllUsesWith(sG);
                        dG->eraseFromParent();
                    }
                }
                // Reparent the global variable:
                sG->removeFromParent();
                dest.getGlobalList().push_back(sG);
                // Comdat is owned by the Module
                sG->setComdat(nullptr);
            }

            for (Module::iterator I = src.begin(), E = src.end(); I != E;) {
                Function *sG = &*I;
                Function *dG = cast_or_null<Function>(dest.getNamedValue(sG->getName()));
                ++I;
                // Replace a declaration with the definition:
                if (dG) {
                    if (sG->isDeclaration()) {
                        sG->replaceAllUsesWith(dG);
                        sG->eraseFromParent();
                        continue;
                    }
                    else {
                        assert(dG->isDeclaration());
                        dG->replaceAllUsesWith(sG);
                        dG->eraseFromParent();
                    }
                }
                // Reparent the global variable:
                sG->removeFromParent();
                dest.getFunctionList().push_back(sG);
                // Comdat is owned by the Module
                sG->setComdat(nullptr);
            }

            for (Module::alias_iterator I = src.alias_begin(), E = src.alias_end(); I != E;) {
                GlobalAlias *sG = &*I;
                GlobalAlias *dG = cast_or_null<GlobalAlias>(dest.getNamedValue(sG->getName()));
                ++I;
                if (dG) {
                    if (!dG->isDeclaration()) { // aliases are always definitions, so this test is reversed from the above two
                        sG->replaceAllUsesWith(dG);
                        sG->eraseFromParent();
                        continue;
                    }
                    else {
                        dG->replaceAllUsesWith(sG);
                        dG->eraseFromParent();
                    }
                }
                sG->removeFromParent();
                dest.getAliasList().push_back(sG);
            }

            // metadata nodes need to be explicitly merged not just copied
            // so there are special passes here for each known type of metadata
            NamedMDNode *sNMD = src.getNamedMetadata("llvm.dbg.cu");
            if (sNMD) {
                NamedMDNode *dNMD = dest.getOrInsertNamedMetadata("llvm.dbg.cu");
                for (MDNode *I : sNMD->operands()) {
                    dNMD->addOperand(I);
                }
            }
        });
    });
}

//TargetMachine pass-through methods

std::unique_ptr<TargetMachine> JuliaOJIT::cloneTargetMachine() const
{
    return std::unique_ptr<TargetMachine>(getTarget()
        .createTargetMachine(
            getTargetTriple().str(),
            getTargetCPU(),
            getTargetFeatureString(),
            getTargetOptions(),
            TM->getRelocationModel(),
            TM->getCodeModel(),
            TM->getOptLevel()));
}

const Triple& JuliaOJIT::getTargetTriple() const {
    return TM->getTargetTriple();
}
StringRef JuliaOJIT::getTargetFeatureString() const {
    return TM->getTargetFeatureString();
}
StringRef JuliaOJIT::getTargetCPU() const {
    return TM->getTargetCPU();
}
const TargetOptions &JuliaOJIT::getTargetOptions() const {
    return TM->Options;
}
const Target &JuliaOJIT::getTarget() const {
    return TM->getTarget();
}
TargetIRAnalysis JuliaOJIT::getTargetIRAnalysis() const {
    return TM->getTargetIRAnalysis();
}

static void jl_decorate_module(Module &M) {
    auto TT = Triple(M.getTargetTriple());
    if (TT.isOSWindows() && TT.getArch() == Triple::x86_64) {
        // Add special values used by debuginfo to build the UnwindData table registration for Win64
        // This used to be GV, but with https://reviews.llvm.org/D100944 we no longer can emit GV into `.text`
        // TODO: The data is set in debuginfo.cpp but it should be okay to actually emit it here.
        M.appendModuleInlineAsm("\
    .section .text                  \n\
    .type   __UnwindData,@object    \n\
    .p2align        2, 0x90         \n\
    __UnwindData:                   \n\
        .zero   12                  \n\
        .size   __UnwindData, 12    \n\
                                    \n\
        .type   __catchjmp,@object  \n\
        .p2align        2, 0x90     \n\
    __catchjmp:                     \n\
        .zero   12                  \n\
        .size   __catchjmp, 12");
    }
}

// Implements Tarjan's SCC (strongly connected components) algorithm, simplified to remove the count variable
static int jl_add_to_ee(
        orc::ThreadSafeModule &M,
        const StringMap<orc::ThreadSafeModule*> &NewExports,
        DenseMap<orc::ThreadSafeModule*, int> &Queued,
        std::vector<orc::ThreadSafeModule*> &Stack)
{
    // First check if the TSM is empty (already compiled)
    if (!M)
        return 0;
    // Next check and record if it is on the stack somewhere
    {
        auto &Id = Queued[&M];
        if (Id)
            return Id;
        Stack.push_back(&M);
        Id = Stack.size();
    }
    // Finally work out the SCC
    int depth = Stack.size();
    int MergeUp = depth;
    std::vector<orc::ThreadSafeModule*> Children;
    M.withModuleDo([&](Module &m) JL_NOTSAFEPOINT {
        for (auto &F : m.global_objects()) {
            if (F.isDeclaration() && F.getLinkage() == GlobalValue::ExternalLinkage) {
                auto Callee = NewExports.find(F.getName());
                if (Callee != NewExports.end()) {
                    auto *CM = Callee->second;
                    if (*CM && CM != &M) {
                        auto Down = Queued.find(CM);
                        if (Down != Queued.end())
                            MergeUp = std::min(MergeUp, Down->second);
                        else
                            Children.push_back(CM);
                    }
                }
            }
        }
    });
    assert(MergeUp > 0);
    for (auto *CM : Children) {
        int Down = jl_add_to_ee(*CM, NewExports, Queued, Stack);
        assert(Down <= (int)Stack.size());
        if (Down)
            MergeUp = std::min(MergeUp, Down);
    }
    if (MergeUp < depth)
        return MergeUp;
    while (1) {
        // Not in a cycle (or at the top of it)
        // remove SCC state and merge every CM from the cycle into M
        orc::ThreadSafeModule *CM = Stack.back();
        auto it = Queued.find(CM);
        assert(it->second == (int)Stack.size());
        Queued.erase(it);
        Stack.pop_back();
        if ((int)Stack.size() < depth) {
            assert(&M == CM);
            break;
        }
        jl_merge_module(M, std::move(*CM));
    }
    jl_ExecutionEngine->addModule(std::move(M));
    return 0;
}

static uint64_t getAddressForFunction(StringRef fname)
{
    auto addr = jl_ExecutionEngine->getFunctionAddress(fname);
    assert(addr);
    return addr;
}

// helper function for adding a DLLImport (dlsym) address to the execution engine
void add_named_global(StringRef name, void *addr)
{
    jl_ExecutionEngine->addGlobalMapping(name, (uint64_t)(uintptr_t)addr);
}

extern "C" JL_DLLEXPORT_CODEGEN
size_t jl_jit_total_bytes_impl(void)
{
    return jl_ExecutionEngine->getTotalBytes();
}
