// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include "passes.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/Debug.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <filesystem>

#include "jitlayers.h"


#define DEBUG_TYPE "module-hash-check"

using namespace llvm;

extern JuliaOJIT *jl_ExecutionEngine;

inline bool file_exists (const std::string& name) {
  struct stat buffer;
  return (stat (name.c_str(), &buffer) == 0);
}

/// An opaque object representing a stable hash code. It can be serialized,
/// deserialized, and is stable across processes and executions.
using stable_hash = uint64_t;

// Stable hashes are based on the 64-bit FNV-1 hash:
// https://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function
const uint64_t FNV_PRIME_64 = 1099511628211u;
const uint64_t FNV_OFFSET_64 = 14695981039346656037u;

inline void stable_hash_append(stable_hash &Hash, const char Value) {
  Hash = Hash ^ (Value & 0xFF);
  Hash = Hash * FNV_PRIME_64;
}

inline void stable_hash_append(stable_hash &Hash, stable_hash Value) {
  for (unsigned I = 0; I < 8; ++I) {
    stable_hash_append(Hash, static_cast<char>(Value));
    Value >>= 8;
  }
}

inline stable_hash stable_hash_combine(stable_hash A, stable_hash B) {
    stable_hash Hash = FNV_OFFSET_64;
    stable_hash_append(Hash, A);
    stable_hash_append(Hash, B);
    return Hash;
}

/// Compute a stable_hash for a sequence of values.
///
/// This hashes a sequence of values. It produces the same stable_hash as
/// 'stable_hash_combine(a, b, c, ...)', but can run over arbitrary sized
/// sequences and is significantly faster given pointers and types which
/// can be hashed as a sequence of bytes.
template <typename InputIteratorT>
stable_hash stable_hash_combine_range(InputIteratorT First,
                                      InputIteratorT Last) {
    stable_hash Hash = FNV_OFFSET_64;
    for (auto I = First; I != Last; ++I)
      stable_hash_append(Hash, *I);
    return Hash;
}

bool moduleHashCheck(Module &M)
{
    // If you have trouble boostraping julia uncomment this to disable the cache:
    // return false;

    // // The most strict hash (big perf hit and poor hit ratio)
    // std::string mod_ir;
    // raw_string_ostream mod_ir_stream(mod_ir);
    // mod_ir_stream << M;
    // mod_ir_stream.flush();
    //
    // auto strict_hash = stable_hash_combine_range(mod_ir.begin(), mod_ir.end());

    auto &ctx = M.getContext();

    // Adds time flag to use for timing later (uses a string for no reason, not optimized at all)
    auto now = std::chrono::high_resolution_clock::now();
    auto epoch = now.time_since_epoch();
    long long us = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
    M.addModuleFlag(Module::ModFlagBehavior::Override, "time", MDString::get(ctx, std::to_string(us)));

    stable_hash all_globals_hash = FNV_OFFSET_64;

    // Collects all global names, sorts and hashes them
    std::vector<StringRef> global_names;
    for (auto& global: M.global_objects()) {
        global_names.push_back(global.getName());
    }
    std::sort(global_names.begin(), global_names.end());
    for (auto& name: global_names) {
        auto hash = stable_hash_combine_range(name.begin(), name.end());
        stable_hash_append(all_globals_hash, hash);
    }

    // Counts the number of defined functions and detects if there is a runtime dlsym
    int n_defined_fns = 0;
    bool has_dlsym = false;
    for (auto &F : M) {
        if (!F.empty() || F.isMaterializable()) {
            n_defined_fns += 1;
        } else if (F.getName().find("ijl_dlsym") == 0 || F.getName().find("ijl_load_and_lookup") == 0) {
            has_dlsym = true;
        }
    }

    // FIXME Very crude way of detecting a type (this check is probably not needed though)
    bool is_type = false;
    if (!M.getName().empty() && isupper(M.getName()[0])) {
        is_type = true;
    }

    bool ignore = (M.getName() == "_foldl_impl" || M.getName() == "_groupedunique!");

    // Whether or not this is a merged module
    auto merged = M.getModuleFlag("merged");

    // TODO We can probably relax the n_defined_fns requirement
    if (n_defined_fns == 2 && !has_dlsym && !is_type && !ignore && !merged) {

        // Find main function name for reporting
        std::string f_prefix = "julia_" + std::string(M.getName()) + "_";
        std::string function_name("");
        for (auto &F : M) {
            if (!F.empty() || F.isMaterializable()) {
                if (F.getName().find(f_prefix) == 0 && F.getName().size() > 0) {
                    function_name = std::string(F.getName());
                    break;
                }
            }
            if (!function_name.empty()) {
                break;
            }
        }
        errs() << "Function name: " << function_name << "\n";

        if (!function_name.empty()) {
            // Add hash flag to perform caching later on
            M.addModuleFlag(Module::ModFlagBehavior::Override, "hash", MDString::get(ctx, std::to_string(all_globals_hash)));

            // Check if this module was cached before
            std::string filename = "/tmp/mod_" + std::to_string(all_globals_hash) + ".so";
            if (file_exists(filename)) {
                // If true clear the whole module so that optimization is very fast
                M.getGlobalList().clear();
                M.getFunctionList().clear();
                M.getAliasList().clear();
                M.getIFuncList().clear();
                return true;
            }
        }
    }

    return false;
}

PreservedAnalyses ModuleHashCheckPass::run(Module &M, ModuleAnalysisManager &AM)
{
    if (moduleHashCheck(M)) {
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}

namespace {
struct ModuleHashCheckLegacy : public ModulePass {
    static char ID;
    ModuleHashCheckLegacy() : ModulePass(ID) {};

    bool runOnModule(Module &M)
    {
        return moduleHashCheck(M);
    }
};

char ModuleHashCheckLegacy::ID = 0;
static RegisterPass<ModuleHashCheckLegacy>
        Y("ModuleHashCheck",
          "Hashes Module IR and checks cache.",
          false,
          false);
}

Pass *createModuleHashCheckPass()
{
    return new ModuleHashCheckLegacy();
}

extern "C" JL_DLLEXPORT void LLVMExtraAddModuleHashCheckPass_impl(LLVMPassManagerRef PM)
{
    unwrap(PM)->add(createModuleHashCheckPass());
}
