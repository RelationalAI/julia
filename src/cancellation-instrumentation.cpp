/*
 * cancellation_instrumentation.cpp
 *
 *  Created on: 9 Mar 2024
 *      Author: ddeleo
 */
#include "julia.h"
#include "julia_internal.h"

#include <iostream>
#include <unistd.h>

using namespace std;

extern "C" {

static uint64_t g_min_update_interval = 0;
static uint64_t get_min_update_interval() {
    // this is inherently thread-safe, but that's okay as we're only interested on a rough estimate
    // for how many clock cycles correspond to 1 second, more or less.
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

JL_DLLEXPORT bool update_epoch(bool force) {
    jl_task_t *task = jl_current_task;
    uint64_t t0 = task->instr_last_epoch;
    uint64_t t1 = cycleclock();
    if (!force && ((t1 - t0) <= get_min_update_interval())){
        return false;
    } else {
        task->instr_last_epoch = t1;
        return true;
    }
}

} // extern C
