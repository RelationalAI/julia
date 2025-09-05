// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "julia.h"
#include "julia_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_FIELD_LOG_MAX_BT_SIZE (1<<16)
#define GC_FIELD_LOG_MAX_ENTRIES (1<<22)
typedef struct _gc_field_log_entry_t {
    jl_value_t **pfield;
    jl_bt_element_t bt[GC_FIELD_LOG_MAX_BT_SIZE];
} gc_field_log_entry_t;

uv_mutex_t gc_field_log_lock;
size_t gc_field_log_next_entry;
gc_field_log_entry_t gc_field_log[GC_FIELD_LOG_MAX_ENTRIES];

int gc_field_logger_should_log(jl_value_t *parent, jl_value_t **child)
{
    return 1;
}
void gc_field_logger_action(jl_value_t *parent, jl_value_t **child)
{
    uv_mutex_lock(&gc_field_log_lock);
    // size_t entry_idx = gc_field_log_next_entry++;
    // if (entry_idx >= GC_FIELD_LOG_MAX_ENTRIES) {
    //     jl_safe_printf("gc_field_logger_log: too many entries, increase GC_FIELD_LOG_MAX_ENTRIES\n");
    //     abort();
    // }
    // gc_field_log_entry_t *entry = &gc_field_log[entry_idx];
    // entry->pfield = child;
    // jl_record_backtrace(jl_current_task, entry->bt, GC_FIELD_LOG_MAX_BT_SIZE, 0);
    uv_mutex_unlock(&gc_field_log_lock);
}
void gc_field_logger_append_entry(jl_value_t *parent, jl_value_t **child)
{
    if (!gc_field_logger_should_log(parent, child)) {
        return;
    }
    gc_field_logger_action(parent, child);
}
void gc_reset_field_logger(void)
{
    uv_mutex_lock(&gc_field_log_lock);
    gc_field_log_next_entry = 0;
    uv_mutex_unlock(&gc_field_log_lock);
}

#ifdef __cplusplus
}
#endif
