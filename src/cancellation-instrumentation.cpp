/*
 * cancellation_instrumentation.cpp
 *
 *  Created on: 9 Mar 2024
 *      Author: ddeleo
 */
#include "julia.h"
#include "julia_internal.h"
#include "threading.h"

#include <cassert>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

using namespace std;

extern "C" { extern double jl_clock_now(void); }
extern jl_raw_backtrace_t get_raw_backtrace();
extern "C" { void jl_rec_backtrace(jl_task_t *t); }

struct jl_ccinstr_entry {
    double m_timestamp; // when the last backtrace has been captured, in seconds since the epoch
    jl_raw_backtrace_t m_backtrace; // recorded backtrace
};

// The stacktraces recorded so far
struct jl_ccinstr_entries{
    jl_ccinstr_entry* m_entries;
    size_t num_entries;
} ;


namespace {

struct CCInstrumentation {
    uint64_t m_last_cancellation_cpu_clock; // last time a cancellation check was performed (cpu clock)
    jl_value_t* m_cancellation_context; // current cancellation context
    vector<jl_ccinstr_entry> m_backtraces;

    CCInstrumentation(): m_last_cancellation_cpu_clock(0), m_cancellation_context(nullptr) {

    }
};

#define CCINSTR reinterpret_cast<CCInstrumentation*>(task->cc_instrumentation)

} // anonymous namespace


extern "C" {

// max interval between two consecutive cancellation points before emitting a trace, in seconds
JL_DLLEXPORT double jl_ccinstr_max_interval = 120.0;

// interval when to record a backtrace triggered by a gc pass, in seconds
JL_DLLEXPORT double jl_ccinstr_gc_interval = 30.0;

static uint64_t g_min_update_interval = 0;
static uint64_t get_min_update_interval() {
    // this is inherently not thread-safe, but that's okay as we're only interested on a rough estimate
    // of how many clock cycles correspond to 1 second.
    if (g_min_update_interval == 0) {
        // sleep can be interrupted by a signal before the expected sleep time. In this case it will
        // return with a value != 0 and we repeat the measurement.
        int rc = 0;
        do {
            uint64_t t0 = cycleclock();
            rc = sleep(1); // 1 second
            uint64_t t1 = cycleclock();
            g_min_update_interval = t1 - t0;
        } while (rc != 0);
    }
    return g_min_update_interval;
}

static bool update_epoch(jl_task_t* task, bool force) {
    uint64_t t0 = CCINSTR->m_last_cancellation_cpu_clock;
    uint64_t t1 = cycleclock();
    if (!force && ((t1 - t0) <= get_min_update_interval())){
        return false;
    } else {
        CCINSTR->m_last_cancellation_cpu_clock = t1;
        return true;
    }
}

// Append a backtrace for the current task invoking this routine
static void ccinstr_append_backtrace(double time = jl_clock_now()){
    jl_task_t* task = jl_current_task;
    jl_ccinstr_entry entry {time, get_raw_backtrace() };
    CCINSTR->m_backtraces.push_back(entry);
}

static void ccinstr_append_backtrace_task(jl_task_t* task, double time = jl_clock_now()){
    jl_task_t *ct = jl_current_task;
    jl_ptls_t ptls = ct->ptls;
    jl_rec_backtrace(task);
    size_t bt_size = ptls->bt_size;
    size_t bt_bytes = bt_size * sizeof(jl_bt_element_t);
    jl_bt_element_t *bt_data = (jl_bt_element_t*) malloc_s(bt_bytes);
    memcpy(bt_data, ptls->bt_data, bt_bytes);
    jl_raw_backtrace_t backtrace { bt_data, bt_size };
    jl_ccinstr_entry entry {time, backtrace };
    CCINSTR->m_backtraces.push_back(entry);
}

static void ccinstr_clear_backtraces(jl_task_t* task){
    auto& backtraces = CCINSTR->m_backtraces;
    for(auto& entry: backtraces){
        free(entry.m_backtrace.data);
    }
    backtraces.clear();
}

void ccinstr_initialize_task(jl_task_t* task){
    task->cc_instrumentation = new CCInstrumentation();
}

void ccinstr_finalize_task(jl_task_t* task){
    if (CCINSTR != nullptr){
        ccinstr_clear_backtraces(task);
        delete CCINSTR;
    }

    task->cc_instrumentation = nullptr;
}

JL_DLLEXPORT bool jl_ccinstr_record_cancellation_point(jl_value_t* abstract_cancellation_context, bool force){
    jl_task_t* task = jl_current_task;
    if (CCINSTR == nullptr) return false; // mmh

    // reset the current cancellation context
    if (CCINSTR->m_cancellation_context == nullptr || CCINSTR->m_cancellation_context != abstract_cancellation_context){
        CCINSTR->m_cancellation_context = abstract_cancellation_context;
        CCINSTR->m_last_cancellation_cpu_clock = cycleclock();
        ccinstr_clear_backtraces(task);
        ccinstr_append_backtrace();
        return false;
    }

    // check we're not recording too many stacktraces: only 1 each second for a given task
    bool can_update = update_epoch(task, force);
    if(!can_update) return false;

    // shall we emit a warning ?
    assert(CCINSTR->m_backtraces.size() > 0 && "At least the first backtrace should be present");
    double t0 = CCINSTR->m_backtraces[0].m_timestamp;
    double t1 = jl_clock_now();
    if (t1 - t0 >= jl_ccinstr_max_interval){
        ccinstr_append_backtrace(t1); // current position
        return true;
    }

    // reset the captured backtraces and insert the current one
    //cout << "[jl_ccinstr_record_cancellation_point] task: " << task << ", t0: " << t0 << ", t1: " << t1 << ", diff: " << (t1 - t0) << endl;
    ccinstr_clear_backtraces(task);
    ccinstr_append_backtrace();
    return false;
}

JL_DLLEXPORT jl_ccinstr_entries jl_ccinstr_fetch_backtraces(){
    jl_task_t* task = jl_current_task;
    if (CCINSTR == nullptr)
        return jl_ccinstr_entries{ nullptr, 0 };


    return jl_ccinstr_entries{
        CCINSTR->m_backtraces.data(),
        CCINSTR->m_backtraces.size()
    };
}

JL_DLLEXPORT void jl_ccinstr_reset_cancellation_point(){
    jl_task_t* task = jl_current_task;
    if (CCINSTR != nullptr){
        CCINSTR->m_cancellation_context = nullptr;
    }
}


// Append a backtrace for the given task only if force == true or at least `jl_ccinstr_gc_interval` have passed since the last
// backtrace was recorded.
JL_DLLEXPORT void jl_ccinstr_record_backtrace_for_task(jl_task_t* task, bool force){
    if (CCINSTR == nullptr || CCINSTR->m_backtraces.size() == 0) return; // no instrumentation

    double t0 = CCINSTR->m_backtraces[CCINSTR->m_backtraces.size() -1].m_timestamp;
    double t1 = jl_clock_now();
    if (!force && (t1 - t0) < jl_ccinstr_gc_interval) return;
    ccinstr_append_backtrace_task(task, t1);
}

// based on jl_print_task_backtraces
extern int gc_first_tid;
static mutex g_mutex;
JL_DLLEXPORT void jl_ccinstr_record_all_backtraces(int /* bool, int for compatibility with C */ force){
    lock_guard<mutex> lock(g_mutex);

    size_t nthreads = jl_atomic_load_acquire(&jl_n_threads);
    jl_ptls_t *allstates = jl_atomic_load_relaxed(&jl_all_tls_states);
    for (size_t i = 0; i < nthreads; i++) {
        // skip GC threads since they don't have tasks
        if (gc_first_tid <= i && i < gc_first_tid + jl_n_gcthreads) {
            continue;
        }
        jl_ptls_t ptls2 = allstates[i];
        if (ptls2 == NULL) {
            continue;
        }
        small_arraylist_t *live_tasks = &ptls2->heap.live_tasks;
        size_t n = mtarraylist_length(live_tasks);
        int t_state = JL_TASK_STATE_DONE;
        jl_task_t *t = ptls2->root_task;

        // root task
        if (t != NULL && t->stkbuf != NULL){
            t_state = jl_atomic_load_relaxed(&t->_state);
            if (t_state != JL_TASK_STATE_DONE) {
                jl_ccinstr_record_backtrace_for_task(t, force);
            }
        }

        // further tasks
        for (size_t j = 0; j < n; j++) {
            jl_task_t *t = (jl_task_t*)mtarraylist_get(live_tasks, j);
            if (t == NULL || t->stkbuf == NULL) continue;
            int t_state = jl_atomic_load_relaxed(&t->_state);
            if (t_state == JL_TASK_STATE_DONE) continue;
            jl_ccinstr_record_backtrace_for_task(t, force);
        }
    }
}

} // extern C
