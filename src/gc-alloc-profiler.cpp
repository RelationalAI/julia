// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "gc-alloc-profiler.h"

#include "julia_internal.h"
#include "gc.h"

#include <string>
#include <vector>

using std::string;
using std::vector;

struct jl_raw_backtrace_t {
    jl_bt_element_t *data;
    size_t size;
};

struct jl_raw_alloc_t {
    jl_datatype_t *type_address;
    jl_raw_backtrace_t backtrace;
    size_t size;
    void *task;
    uint64_t timestamp;
};

// == These structs define the global singleton profile buffer that will be used by
// callbacks to store profile results. ==
struct jl_per_thread_alloc_profile_t {
    vector<jl_raw_alloc_t> allocs;
};

struct jl_alloc_profile_t {
    double sample_rate;

    vector<jl_per_thread_alloc_profile_t> per_thread_profiles;
};

struct jl_combined_results {
    vector<jl_raw_alloc_t> combined_allocs;
};

// == Global variables manipulated by callbacks ==

jl_alloc_profile_t g_alloc_profile;
int g_alloc_profile_enabled = false;
jl_combined_results g_combined_results; // Will live forever.

// === stack stuff ===

jl_raw_backtrace_t get_raw_backtrace() JL_NOTSAFEPOINT {
    // We first record the backtrace onto a MAX-sized buffer, so that we don't have to
    // allocate the buffer until we know the size. To ensure thread-safety, we use a
    // per-thread backtrace buffer.
    jl_ptls_t ptls = jl_current_task->ptls;
    jl_bt_element_t *shared_bt_data_buffer = ptls->profiling_bt_buffer;
    if (shared_bt_data_buffer == NULL) {
        size_t size = sizeof(jl_bt_element_t) * (JL_MAX_BT_SIZE + 1);
        shared_bt_data_buffer = (jl_bt_element_t*) malloc_s(size);
        ptls->profiling_bt_buffer = shared_bt_data_buffer;
    }

    size_t bt_size = rec_backtrace(shared_bt_data_buffer, JL_MAX_BT_SIZE, 2);

    // Then we copy only the needed bytes out of the buffer into our profile.
    size_t bt_bytes = bt_size * sizeof(jl_bt_element_t);
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc_s(bt_bytes);
    memcpy(bt_data, shared_bt_data_buffer, bt_bytes);


    return jl_raw_backtrace_t{
        bt_data,
        bt_size
    };
}

// == exported interface ==

extern "C" {  // Needed since these functions doesn't take any arguments.

uint64_t num_boxed_inputs;
uint64_t boxed_inputs_size;
uint64_t extra_num_boxed_inputs;
uint64_t extra_boxed_inputs_size;
uint64_t num_boxed_returns;
uint64_t boxed_returns_size;

JL_DLLEXPORT uint64_t jl_total_boxes()
{
    return num_boxed_inputs + extra_num_boxed_inputs + num_boxed_returns;
}
JL_DLLEXPORT uint64_t jl_total_boxes_size()
{
    return boxed_inputs_size + extra_boxed_inputs_size + boxed_returns_size;
}
JL_DLLEXPORT uint64_t jl_num_boxed_inputs()
{
    return num_boxed_inputs;
}
JL_DLLEXPORT uint64_t jl_extra_num_boxed_inputs()
{
    return extra_num_boxed_inputs;
}
JL_DLLEXPORT uint64_t jl_boxed_inputs_size()
{
    return boxed_inputs_size;
}
JL_DLLEXPORT uint64_t jl_extra_boxed_inputs_size()
{
    return extra_boxed_inputs_size;
}
JL_DLLEXPORT uint64_t jl_num_boxed_returns()
{
    return num_boxed_returns;
}
JL_DLLEXPORT uint64_t jl_boxed_returns_size()
{
    return boxed_returns_size;
}

static float extra_allocs_rate = 0.0f;
JL_DLLEXPORT void jl_set_extra_allocs_rate(float rate)
{
    extra_allocs_rate = rate;
}

#ifdef JL_DISPATCH_LOG_BOXES
JL_DLLEXPORT void jl_log_box_input(size_t sz)
{
    num_boxed_inputs++;
    boxed_inputs_size += sz;

    // Randomly, with a probability of `extra_allocs_rate`, record some number of
    // extra allocations. The goal is to estimate the impact of _reducing_ the
    // number of allocations for boxing. For a rate >1, more than one allocation
    // may be recorded: we pick a random number between 0 and extra_allocs_rate,
    // then round it and allocate that many extra objects.
    if (extra_allocs_rate > 0.0f) {
        float num_extra_allocs = extra_allocs_rate;
        jl_value_t *extra_obj;
        while (num_extra_allocs > 1) {
            num_extra_allocs--;
            extra_num_boxed_inputs++;
            extra_boxed_inputs_size += sz;
            extra_obj = jl_gc_allocobj(sz);
            memset(extra_obj, 0, sz);
        }
        // decide whether or not to allocate for the last one
        float sample = float(rand()) / float(RAND_MAX);
        if (sample < num_extra_allocs) {
            extra_num_boxed_inputs++;
            extra_boxed_inputs_size += sz;
            extra_obj = jl_gc_allocobj(sz);
            memset(extra_obj, 0, sz);
        }
    }
}
JL_DLLEXPORT void jl_log_box_return(size_t sz)
{
    num_boxed_returns++;
    boxed_returns_size += sz;
}
#endif

JL_DLLEXPORT void jl_start_alloc_profile(double sample_rate) {
    // We only need to do this once, the first time this is called.
    size_t nthreads = jl_atomic_load_acquire(&jl_n_threads);
    while (g_alloc_profile.per_thread_profiles.size() < nthreads) {
        g_alloc_profile.per_thread_profiles.push_back(jl_per_thread_alloc_profile_t{});
    }

    g_alloc_profile.sample_rate = sample_rate;
    g_alloc_profile_enabled = true;
}

JL_DLLEXPORT jl_profile_allocs_raw_results_t jl_fetch_alloc_profile() {
    // combine allocs
    // TODO: interleave to preserve ordering
    for (auto& profile : g_alloc_profile.per_thread_profiles) {
        for (const auto& alloc : profile.allocs) {
            g_combined_results.combined_allocs.push_back(alloc);
        }

        profile.allocs.clear();
    }

    return jl_profile_allocs_raw_results_t{
        g_combined_results.combined_allocs.data(),
        g_combined_results.combined_allocs.size(),
    };
}

JL_DLLEXPORT void jl_stop_alloc_profile() {
    g_alloc_profile_enabled = false;
}

JL_DLLEXPORT void jl_free_alloc_profile() {
    // Free any allocs that remain in the per-thread profiles, that haven't
    // been combined yet (which happens in fetch_alloc_profiles()).
    for (auto& profile : g_alloc_profile.per_thread_profiles) {
        for (auto alloc : profile.allocs) {
            free(alloc.backtrace.data);
        }
        profile.allocs.clear();
    }

    // Free the allocs that have been already combined into the combined results object.
    for (auto alloc : g_combined_results.combined_allocs) {
        free(alloc.backtrace.data);
    }

    g_combined_results.combined_allocs.clear();
}

// == callback called into by the outside ==

void _maybe_record_alloc_to_profile(jl_value_t *val, size_t size, jl_datatype_t *type) JL_NOTSAFEPOINT {
    auto& global_profile = g_alloc_profile;
    size_t thread_id = jl_atomic_load_relaxed(&jl_current_task->tid);
    if (thread_id >= global_profile.per_thread_profiles.size())
        return; // ignore allocations on threads started after the alloc-profile started

    auto& profile = global_profile.per_thread_profiles[thread_id];

    auto sample_val = double(rand()) / double(RAND_MAX);
    auto should_record = sample_val <= global_profile.sample_rate;
    if (!should_record) {
        return;
    }

    profile.allocs.emplace_back(jl_raw_alloc_t{
        type,
        get_raw_backtrace(),
        size,
        (void *)jl_current_task,
        cycleclock()
    });
}

}  // extern "C"
