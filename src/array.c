// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  array constructors and primitives
*/
#include <stdlib.h>
#include <string.h>
#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JL_ARRAY_IMPL_NUL 1

#define JL_ARRAY_ALIGN(jl_value, nbytes) LLT_ALIGN(jl_value, nbytes)

static inline void arrayassign_safe(int hasptr, jl_value_t *parent, char *dst, const jl_value_t *src, size_t nb) JL_NOTSAFEPOINT
{
    // array can assume more alignment than a field would normally have
    assert(nb >= jl_datatype_size(jl_typeof(src))); // nb might move some undefined bits, but we should be okay with that
    if (hasptr) {
        size_t nptr = nb / sizeof(void*);
        memmove_refs((void**)dst, (void* const*)src, nptr);
        jl_gc_multi_wb(parent, src);
    }
    else {
        switch (nb) {
        case  0: break;
        case  1: *(uint8_t*)dst  = *(uint8_t*)src;  break;
        case  2: *(uint16_t*)dst = *(uint16_t*)src; break;
        case  4: *(uint32_t*)dst = *(uint32_t*)src; break;
        case  8: *(uint64_t*)dst = *(uint64_t*)src; break;
        case 16:
            memcpy(jl_assume_aligned(dst, 16), jl_assume_aligned(src, 16), 16);
            break;
        default: memcpy(dst, src, nb);
        }
    }
}

static inline void memmove_safe(int hasptr, char *dst, const char *src, size_t nb) JL_NOTSAFEPOINT
{
    if (hasptr)
        memmove_refs((void**)dst, (void**)src, nb / sizeof(void*));
    else
        memmove(dst, src, nb);
}

// array constructors ---------------------------------------------------------
JL_DLLEXPORT char *jl_array_typetagdata(jl_array_t *a) JL_NOTSAFEPOINT
{
    assert(jl_array_isbitsunion(a));
    return ((char*)jl_array_data(a)) + ((jl_array_ndims(a) == 1 ? (a->maxsize - a->offset) : jl_array_len(a)) * a->elsize) + a->offset;
}

STATIC_INLINE jl_value_t *jl_array_owner(jl_array_t *a JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT
{
    if (a->flags.how == 3) {
        a = (jl_array_t*)jl_array_data_owner(a);
        assert(jl_is_string(a) || a->flags.how != 3);
    }
    return (jl_value_t*)a;
}

#if defined(_P64) && defined(UINT128MAX)
typedef __uint128_t wideint_t;
#else
typedef uint64_t wideint_t;
#endif

#define MAXINTVAL (((size_t)-1)>>1)

JL_DLLEXPORT int jl_array_validate_dims(size_t *nel, size_t *tot, uint32_t ndims, size_t *dims, size_t elsz)
{
    size_t i;
    size_t _nel = 1;
    for(i=0; i < ndims; i++) {
        size_t di = dims[i];
        wideint_t prod = (wideint_t)_nel * (wideint_t)di;
        if (prod >= (wideint_t) MAXINTVAL || di >= MAXINTVAL)
            return 1;
        _nel = prod;
    }
    wideint_t prod = (wideint_t)elsz * (wideint_t)_nel;
    if (prod >= (wideint_t) MAXINTVAL)
        return 2;
    *nel = _nel;
    *tot = (size_t)prod;
    return 0;
}

static jl_array_t *_new_array_(jl_value_t *atype, uint32_t ndims, size_t *dims,
                               int8_t isunboxed, int8_t hasptr, int8_t isunion, int8_t zeroinit, size_t elsz)
{
    jl_task_t *ct = jl_current_task;
    size_t i, tot, nel;
    void *data;
    jl_array_t *a;
    assert(isunboxed || elsz == sizeof(void*));
    assert(atype == NULL || isunion == jl_is_uniontype(jl_tparam0(atype)));
    int validated = jl_array_validate_dims(&nel, &tot, ndims, dims, elsz);
    if (validated == 1)
        jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
    else if (validated == 2)
        jl_error("invalid Array size");
    if (isunboxed) {
        if (elsz == 1 && !isunion) {
            // extra byte for all julia allocated byte arrays
            tot++;
        }
        if (isunion) {
            // an extra byte for each isbits union array element, stored after a->maxsize
            tot += nel;
        }
    }

    int ndimwords = jl_array_ndimwords(ndims);
    int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
    if (tot <= ARRAY_INLINE_NBYTES) {
        // align data area
        if (tot >= ARRAY_CACHE_ALIGN_THRESHOLD)
            tsz = JL_ARRAY_ALIGN(tsz, JL_CACHE_BYTE_ALIGNMENT);
        else if (isunboxed && elsz >= 4)
            tsz = JL_ARRAY_ALIGN(tsz, JL_SMALL_BYTE_ALIGNMENT);
        size_t doffs = tsz;
        tsz += tot;
        // jl_array_t is large enough that objects will always be aligned 16
        a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, atype);
        assert(((size_t)a & 15) == 0);
        // No allocation or safepoint allowed after this
        a->flags.how = 0;
        data = (char*)a + doffs;
    }
    else {
        data = jl_gc_managed_malloc(tot);
        // Allocate the Array **after** allocating the data
        // to make sure the array is still young
        a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, atype);
        // No allocation or safepoint allowed after this
        a->flags.how = 2;
        jl_gc_track_malloced_array(ct->ptls, a);
    }
    a->flags.pooled = tsz <= GC_MAX_SZCLASS;

    if (zeroinit)
        memset(data, 0, tot);
    a->data = data;
    if (JL_ARRAY_IMPL_NUL && elsz == 1)
        ((char*)data)[tot - 1] = '\0';
    a->length = nel;
    a->flags.ndims = ndims;
    a->flags.ptrarray = !isunboxed;
    a->flags.hasptr = hasptr;
    a->elsize = elsz;
    a->flags.isshared = 0;
    a->flags.isaligned = 1;
    a->offset = 0;
    if (ndims == 1) {
        a->nrows = nel;
        a->maxsize = nel;
    }
    else if (a->flags.ndims != ndims) {
        jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
    }
    else {
        size_t *adims = &a->nrows;
        for (i = 0; i < ndims; i++)
            adims[i] = dims[i];
    }

    return a;
}

static inline jl_array_t *_new_array(jl_value_t *atype, uint32_t ndims, size_t *dims)
{
    jl_value_t *eltype = jl_tparam0(atype);
    size_t elsz = 0, al = 0;
    if (!jl_is_kind(jl_typeof(eltype)))
        jl_type_error_rt("Array", "element type", (jl_value_t*)jl_type_type, eltype);
    int isunboxed = jl_islayout_inline(eltype, &elsz, &al);
    int isunion = jl_is_uniontype(eltype);
    int hasptr = isunboxed && (jl_is_datatype(eltype) && ((jl_datatype_t*)eltype)->layout->npointers > 0);
    if (!isunboxed) {
        elsz = sizeof(void*);
        al = elsz;
    }
    else {
        elsz = LLT_ALIGN(elsz, al);
    }
    int zi = !isunboxed || hasptr || isunion || (jl_is_datatype(eltype) && ((jl_datatype_t*)eltype)->zeroinit);

    return _new_array_(atype, ndims, dims, isunboxed, hasptr, isunion, zi, elsz);
}

jl_array_t *jl_new_array_for_deserialization(jl_value_t *atype, uint32_t ndims, size_t *dims,
                                             int isunboxed, int hasptr, int isunion, int elsz)
{
    return _new_array_(atype, ndims, dims, isunboxed, hasptr, isunion, 0, (size_t)elsz);
}

#ifndef JL_NDEBUG
static inline int is_ntuple_long(jl_value_t *v)
{
    if (!jl_is_tuple(v))
        return 0;
    jl_value_t *tt = jl_typeof(v);
    size_t i, nfields = jl_nparams(tt);
    for (i = 0; i < nfields; i++) {
        if (jl_tparam(tt, i) != (jl_value_t*)jl_long_type) {
            return 0;
        }
    }
    return 1;
}
#endif

JL_DLLEXPORT jl_array_t *jl_reshape_array(jl_value_t *atype, jl_array_t *data,
                                          jl_value_t *_dims)
{
    jl_task_t *ct = jl_current_task;
    assert(jl_types_equal(jl_tparam0(jl_typeof(data)), jl_tparam0(atype)));

    size_t ndims = jl_nfields(_dims);
    assert(is_ntuple_long(_dims));
    size_t *dims = (size_t*)_dims;
    int ndimwords = jl_array_ndimwords(ndims);
    int tsz = sizeof(jl_array_t) + ndimwords * sizeof(size_t) + sizeof(void*);
    jl_array_t *a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, atype);
    // No allocation or safepoint allowed after this
    // copy data (except dims) from the old object
    a->flags.pooled = tsz <= GC_MAX_SZCLASS;
    a->flags.ndims = ndims;
    a->offset = 0;
    a->data = NULL;
    a->flags.isaligned = data->flags.isaligned;
    a->elsize = data->elsize;
    a->flags.ptrarray = data->flags.ptrarray;
    a->flags.hasptr = data->flags.hasptr;

    // if data is itself a shared wrapper,
    // owner should point back to the original array
    jl_array_t *owner = (jl_array_t*)jl_array_owner(data);
    jl_array_data_owner(a) = (jl_value_t*)owner;

    a->flags.how = 3;
    a->data = data->data;
    a->flags.isshared = 1;
    data->flags.isshared = 1;

    if (ndims == 1) {
        size_t l = dims[0];
        a->length = l;
        a->nrows = l;
        a->maxsize = l;
    }
    else if (a->flags.ndims != ndims) {
        jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
    }
    else {
        size_t *adims = &a->nrows;
        size_t l = 1;
        wideint_t prod;
        for (size_t i = 0; i < ndims; i++) {
            adims[i] = dims[i];
            prod = (wideint_t)l * (wideint_t)adims[i];
            if (prod > (wideint_t) MAXINTVAL)
                jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
            l = prod;
        }
        a->length = l;
    }

    return a;
}

JL_DLLEXPORT jl_array_t *jl_string_to_array(jl_value_t *str)
{
    jl_task_t *ct = jl_current_task;
    jl_array_t *a;

    int ndimwords = jl_array_ndimwords(1);
    int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t) + sizeof(void*);
    a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, jl_array_uint8_type);
    a->flags.pooled = tsz <= GC_MAX_SZCLASS;
    a->flags.ndims = 1;
    a->offset = 0;
    a->data = jl_string_data(str);
    a->flags.isaligned = 0;
    a->elsize = 1;
    a->flags.ptrarray = 0;
    a->flags.hasptr = 0;
    jl_array_data_owner(a) = str;
    a->flags.how = 3;
    a->flags.isshared = 1;
    size_t l = jl_string_len(str);
    a->length = l;
    a->nrows = a->maxsize = l;
    return a;
}

// own_buffer != 0 iff GC should call free() on this pointer eventually
JL_DLLEXPORT jl_array_t *jl_ptr_to_array_1d(jl_value_t *atype, void *data,
                                            size_t nel, int own_buffer)
{
    jl_task_t *ct = jl_current_task;
    jl_array_t *a;
    jl_value_t *eltype = jl_tparam0(atype);

    int isunboxed = jl_stored_inline(eltype);
    if (isunboxed && jl_is_uniontype(eltype))
        jl_exceptionf(jl_argumenterror_type,
                      "unsafe_wrap: unspecified layout for union element type");
    size_t elsz;
    unsigned align;
    if (isunboxed) {
        elsz = jl_datatype_size(eltype);
        align = jl_datatype_align(eltype);
    }
    else {
        align = elsz = sizeof(void*);
    }
    if (((uintptr_t)data) & ((align > JL_HEAP_ALIGNMENT ? JL_HEAP_ALIGNMENT : align) - 1))
        jl_exceptionf(jl_argumenterror_type,
                      "unsafe_wrap: pointer %p is not properly aligned to %u bytes", data, align);

    int ndimwords = jl_array_ndimwords(1);
    int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
    a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, atype);
    // No allocation or safepoint allowed after this
    a->flags.pooled = tsz <= GC_MAX_SZCLASS;
    a->data = data;
    a->length = nel;
    a->elsize = LLT_ALIGN(elsz, align);
    a->flags.ptrarray = !isunboxed;
    a->flags.hasptr = isunboxed && (jl_is_datatype(eltype) && ((jl_datatype_t*)eltype)->layout->npointers > 0);
    a->flags.ndims = 1;
    a->flags.isshared = 1;
    a->flags.isaligned = 0;  // TODO: allow passing memalign'd buffers
    if (own_buffer) {
        a->flags.how = 2;
        jl_gc_track_malloced_array(ct->ptls, a);
        jl_gc_count_allocd(nel*elsz + (elsz == 1 ? 1 : 0));
    }
    else {
        a->flags.how = 0;
    }

    a->nrows = nel;
    a->maxsize = nel;
    a->offset = 0;
    return a;
}

JL_DLLEXPORT jl_array_t *jl_ptr_to_array(jl_value_t *atype, void *data,
                                         jl_value_t *_dims, int own_buffer)
{
    jl_task_t *ct = jl_current_task;
    size_t nel = 1;
    jl_array_t *a;
    size_t ndims = jl_nfields(_dims);
    wideint_t prod;
    assert(is_ntuple_long(_dims));
    size_t *dims = (size_t*)_dims;
    for (size_t i = 0; i < ndims; i++) {
        prod = (wideint_t)nel * (wideint_t)dims[i];
        if (prod > (wideint_t) MAXINTVAL)
            jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
        nel = prod;
    }
    if (__unlikely(ndims == 1))
        return jl_ptr_to_array_1d(atype, data, nel, own_buffer);
    jl_value_t *eltype = jl_tparam0(atype);

    int isunboxed = jl_stored_inline(eltype);
    if (isunboxed && jl_is_uniontype(eltype))
        jl_exceptionf(jl_argumenterror_type,
                      "unsafe_wrap: unspecified layout for union element type");
    size_t elsz;
    unsigned align;
    if (isunboxed) {
        elsz = jl_datatype_size(eltype);
        align = jl_datatype_align(eltype);
    }
    else {
        align = elsz = sizeof(void*);
    }
    if (((uintptr_t)data) & ((align > JL_HEAP_ALIGNMENT ? JL_HEAP_ALIGNMENT : align) - 1))
        jl_exceptionf(jl_argumenterror_type,
                      "unsafe_wrap: pointer %p is not properly aligned to %u bytes", data, align);

    int ndimwords = jl_array_ndimwords(ndims);
    int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
    a = (jl_array_t*)jl_gc_alloc(ct->ptls, tsz, atype);
    // No allocation or safepoint allowed after this
    a->flags.pooled = tsz <= GC_MAX_SZCLASS;
    a->data = data;
    a->length = nel;
    a->elsize = LLT_ALIGN(elsz, align);
    a->flags.ptrarray = !isunboxed;
    a->flags.hasptr = isunboxed && (jl_is_datatype(eltype) && ((jl_datatype_t*)eltype)->layout->npointers > 0);
    a->flags.ndims = ndims;
    a->offset = 0;
    a->flags.isshared = 1;
    a->flags.isaligned = 0;
    if (own_buffer) {
        a->flags.how = 2;
        jl_gc_track_malloced_array(ct->ptls, a);
        jl_gc_count_allocd(nel*elsz + (elsz == 1 ? 1 : 0));
    }
    else {
        a->flags.how = 0;
    }

    assert(ndims != 1); // handled above
    if (a->flags.ndims != ndims)
        jl_exceptionf(jl_argumenterror_type, "invalid Array dimensions");
    memcpy(&a->nrows, dims, ndims * sizeof(size_t));
    return a;
}

JL_DLLEXPORT jl_array_t *jl_new_array(jl_value_t *atype, jl_value_t *_dims)
{
    size_t ndims = jl_nfields(_dims);
    assert(is_ntuple_long(_dims));
    return _new_array(atype, ndims, (size_t*)_dims);
}

JL_DLLEXPORT jl_array_t *jl_alloc_array_1d(jl_value_t *atype, size_t nr)
{
    return _new_array(atype, 1, &nr);
}

JL_DLLEXPORT jl_array_t *jl_alloc_array_2d(jl_value_t *atype, size_t nr,
                                           size_t nc)
{
    size_t d[2] = {nr, nc};
    return _new_array(atype, 2, &d[0]);
}

JL_DLLEXPORT jl_array_t *jl_alloc_array_3d(jl_value_t *atype, size_t nr,
                                           size_t nc, size_t z)
{
    size_t d[3] = {nr, nc, z};
    return _new_array(atype, 3, &d[0]);
}

JL_DLLEXPORT jl_array_t *jl_pchar_to_array(const char *str, size_t len)
{
    jl_array_t *a = jl_alloc_array_1d(jl_array_uint8_type, len);
    memcpy(a->data, str, len);
    return a;
}

uv_mutex_t array_to_string_print_lock;

void jl_set_in_flight_bit_for_array_to_string(jl_array_t *a)
{
    uintptr_t msk = (1 << ARRAY_TO_STRING_IN_FLIGHT_BIT_OFFSET);
    uintptr_t header = jl_atomic_fetch_or((_Atomic(uintptr_t) *)jl_astaggedvalue(a), msk);
    if (header & msk) {
        uv_mutex_lock(&array_to_string_print_lock);
        // Race detected... Someone already set the in-flight bit.
        jl_safe_printf("Race detected... Someone already set the in-flight bit.\n");
        jlbacktracet(jl_current_task);
        uv_mutex_unlock(&array_to_string_print_lock);
    }
}

void jl_reset_in_flight_bit_for_array_to_string(jl_array_t *a)
{
    uintptr_t msk = (1 << ARRAY_TO_STRING_IN_FLIGHT_BIT_OFFSET);
    uintptr_t header = jl_atomic_fetch_and((_Atomic(uintptr_t) *)jl_astaggedvalue(a), ~msk);
    if (!(header & msk)) {
        uv_mutex_lock(&array_to_string_print_lock);
        // Race detected... Someone reset the in-flight bit before we could.
        jl_safe_printf("Race detected... Someone reset the in-flight bit before we could.\n");
        jlbacktracet(jl_current_task);
        uv_mutex_unlock(&array_to_string_print_lock);
    }
}

JL_DLLEXPORT jl_value_t *jl_array_to_string(jl_array_t *a)
{
    jl_set_in_flight_bit_for_array_to_string(a);
    size_t len = jl_array_len(a);
    if (len == 0) {
        // this may seem like purely an optimization (which it also is), but it
        // also ensures that calling `String(a)` doesn't corrupt a previous
        // string also created the same way, where `a = StringVector(_)`.
        jl_reset_in_flight_bit_for_array_to_string(a);
        return jl_an_empty_string;
    }
    if (a->flags.how == 3 && a->offset == 0 && a->elsize == 1 &&
        (jl_array_ndims(a) != 1 ||
         ((a->maxsize + sizeof(void*) + 1 <= GC_MAX_SZCLASS) == (len + sizeof(void*) + 1 <= GC_MAX_SZCLASS)))) {
        jl_value_t *o = jl_array_data_owner(a);
        if (jl_is_string(o)) {
            a->flags.isshared = 1;
            *(size_t*)o = len;
            a->nrows = 0;
            a->length = 0;
            a->maxsize = 0;
            jl_reset_in_flight_bit_for_array_to_string(a);
            return o;
        }
    }
    a->nrows = 0;
    a->length = 0;
    jl_reset_in_flight_bit_for_array_to_string(a);
    return jl_pchar_to_string((const char*)jl_array_data(a), len);
}

JL_DLLEXPORT jl_value_t *jl_alloc_string(size_t len)
{
    if (len == 0)
        return jl_an_empty_string;
    size_t sz = sizeof(size_t) + len + 1; // add space for trailing \nul protector and size
    if (sz < len) // overflow
        jl_throw(jl_memory_exception);
    jl_task_t *ct = jl_current_task;
    jl_value_t *s;
    jl_ptls_t ptls = ct->ptls;
    const size_t allocsz = sz + sizeof(jl_taggedvalue_t);
    if (sz <= GC_MAX_SZCLASS) {
        int pool_id = jl_gc_szclass_align8(allocsz);
        jl_gc_pool_t *p = &ptls->gc_tls.heap.norm_pools[pool_id];
        int osize = jl_gc_sizeclasses[pool_id];
        // We call `jl_gc_pool_alloc_noinline` instead of `jl_gc_pool_alloc` to avoid double-counting in
        // the Allocations Profiler. (See https://github.com/JuliaLang/julia/pull/43868 for more details.)
        s = jl_gc_pool_alloc_noinline(ptls, (char*)p - (char*)ptls, osize);
    }
    else {
        if (allocsz < sz) // overflow in adding offs, size was "negative"
            jl_throw(jl_memory_exception);
        s = jl_gc_big_alloc_noinline(ptls, allocsz);
    }
    jl_set_typetagof(s, jl_string_tag, 0);
    maybe_record_alloc_to_profile(s, len, jl_string_type);
    *(size_t*)s = len;
    jl_string_data(s)[len] = 0;
    return s;
}

JL_DLLEXPORT jl_value_t *jl_pchar_to_string(const char *str, size_t len)
{
    jl_value_t *s = jl_alloc_string(len);
    if (len > 0)
        memcpy(jl_string_data(s), str, len);
    return s;
}

JL_DLLEXPORT jl_value_t *jl_cstr_to_string(const char *str)
{
    return jl_pchar_to_string(str, strlen(str));
}

JL_DLLEXPORT jl_array_t *jl_alloc_vec_any(size_t n)
{
    return jl_alloc_array_1d(jl_array_any_type, n);
}

JL_DLLEXPORT jl_value_t *jl_apply_array_type(jl_value_t *type, size_t dim)
{
    jl_value_t *boxed_dim = jl_box_long(dim);
    JL_GC_PUSH1(&boxed_dim);
    jl_value_t *ret = jl_apply_type2((jl_value_t*)jl_array_type, type, boxed_dim);
    JL_GC_POP();
    return ret;
}

// array primitives -----------------------------------------------------------

JL_DLLEXPORT jl_value_t *jl_ptrarrayref(jl_array_t *a JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT
{
    assert(i < jl_array_len(a));
    assert(a->flags.ptrarray);
    jl_value_t *elt = jl_atomic_load_relaxed(((_Atomic(jl_value_t*)*)a->data) + i);
    if (elt == NULL)
        jl_throw(jl_undefref_exception);
    return elt;
}


JL_DLLEXPORT jl_value_t *jl_arrayref(jl_array_t *a, size_t i)
{
    if (a->flags.ptrarray)
        return jl_ptrarrayref(a, i);
    assert(i < jl_array_len(a));
    jl_value_t *eltype = (jl_value_t*)jl_tparam0(jl_typeof(a));
    if (jl_is_uniontype(eltype)) {
        // isbits union selector bytes are always stored directly after the last array element
        uint8_t sel = jl_array_typetagdata(a)[i];
        eltype = jl_nth_union_component(eltype, sel);
        if (jl_is_datatype_singleton((jl_datatype_t*)eltype))
            return ((jl_datatype_t*)eltype)->instance;
    }
    jl_value_t *r = undefref_check((jl_datatype_t*)eltype, jl_new_bits(eltype, &((char*)a->data)[i * a->elsize]));
    if (__unlikely(r == NULL))
        jl_throw(jl_undefref_exception);
    return r;
}

JL_DLLEXPORT int jl_array_isassigned(jl_array_t *a, size_t i)
{
    if (a->flags.ptrarray) {
        return jl_atomic_load_relaxed(((_Atomic(jl_value_t*)*)jl_array_data(a)) + i) != NULL;
    }
    else if (a->flags.hasptr) {
         jl_datatype_t *eltype = (jl_datatype_t*)jl_tparam0(jl_typeof(a));
         assert(eltype->layout->first_ptr >= 0);
         jl_value_t **elem = (jl_value_t**)((char*)a->data + i * a->elsize);
         return elem[eltype->layout->first_ptr] != NULL;
    }
    return 1;
}

JL_DLLEXPORT void jl_arrayset(jl_array_t *a JL_ROOTING_ARGUMENT, jl_value_t *rhs JL_ROOTED_ARGUMENT JL_MAYBE_UNROOTED, size_t i)
{
    assert(i < jl_array_len(a));
    jl_value_t *eltype = jl_tparam0(jl_typeof(a));
    if (eltype != (jl_value_t*)jl_any_type) {
        JL_GC_PUSH1(&rhs);
        if (!jl_isa(rhs, eltype))
            jl_type_error("arrayset", eltype, rhs);
        JL_GC_POP();
    }
    if (!a->flags.ptrarray) {
        int hasptr;
        if (jl_is_uniontype(eltype)) {
            uint8_t *psel = &((uint8_t*)jl_array_typetagdata(a))[i];
            unsigned nth = 0;
            if (!jl_find_union_component(eltype, jl_typeof(rhs), &nth))
                assert(0 && "invalid arrayset to isbits union");
            *psel = nth;
            if (jl_is_datatype_singleton((jl_datatype_t*)jl_typeof(rhs)))
                return;
            hasptr = 0;
        }
        else {
            hasptr = a->flags.hasptr;
        }
        arrayassign_safe(hasptr, jl_array_owner(a), &((char*)a->data)[i * a->elsize], rhs, a->elsize);
    }
    else {
        jl_atomic_store_release(((_Atomic(jl_value_t*)*)a->data) + i, rhs);
        jl_gc_wb(jl_array_owner(a), rhs);
    }
}

JL_DLLEXPORT void jl_arrayunset(jl_array_t *a, size_t i)
{
    if (i >= jl_array_len(a))
        jl_bounds_error_int((jl_value_t*)a, i + 1);
    if (a->flags.ptrarray)
        jl_atomic_store_relaxed(((_Atomic(jl_value_t*)*)a->data) + i, NULL);
    else if (a->flags.hasptr) {
        size_t elsize = a->elsize;
        jl_assume(elsize >= sizeof(void*) && elsize % sizeof(void*) == 0);
        memset((char*)a->data + elsize * i, 0, elsize);
    }
}

// at this size and bigger, allocate resized array data with malloc directly
// instead of managing them separately as gc objects
#define MALLOC_THRESH 1048576

// Resize the buffer to a max size of `newlen`
// The buffer can either be newly allocated or realloc'd, the return
// value is 1 if a new buffer is allocated and 0 if it is realloc'd.
// the caller needs to take care of moving the data from the old buffer
// to the new one if necessary.
// When this function returns, the `->data` pointer always points to
// the **beginning** of the new buffer.
static int NOINLINE array_resize_buffer(jl_array_t *a, size_t newlen)
{
    jl_task_t *ct = jl_current_task;
    assert(!a->flags.isshared || a->flags.how == 3);
    size_t elsz = a->elsize;
    size_t nbytes = newlen * elsz;
    size_t oldnbytes = a->maxsize * elsz;
    size_t oldoffsnb = a->offset * elsz;
    size_t oldlen = a->nrows;
    int isbitsunion = jl_array_isbitsunion(a);
    assert(nbytes >= oldnbytes);
    if (elsz == 1 && !isbitsunion) {
        nbytes++;
        oldnbytes++;
    }
    if (isbitsunion) {
        nbytes += newlen;
        oldnbytes += a->maxsize;
    }
    int newbuf = 0;
    if (a->flags.how == 2) {
        // already malloc'd - use realloc
        char *olddata = (char*)a->data - oldoffsnb;
        a->data = jl_gc_managed_realloc(olddata, nbytes, oldnbytes,
                                        a->flags.isaligned, (jl_value_t*)a);
    }
    else if (a->flags.how == 3 && jl_is_string(jl_array_data_owner(a)) && !isbitsunion) {
        // if data is in a String, keep it that way
        jl_value_t *s;
        if (a->flags.isshared) {
            s = jl_alloc_string(nbytes - (elsz == 1));
            newbuf = 1;
        }
        else {
            s = jl_gc_realloc_string(jl_array_data_owner(a), nbytes - (elsz == 1));
        }
        jl_array_data_owner(a) = s;
        jl_gc_wb(a, s);
        a->data = jl_string_data(s);
    }
    else {
        newbuf = 1;
        if (nbytes >= MALLOC_THRESH) {
            a->data = jl_gc_managed_malloc(nbytes);
            jl_gc_track_malloced_array(ct->ptls, a);
            a->flags.how = 2;
            a->flags.isaligned = 1;
        }
        else {
            a->data = jl_gc_alloc_buf(ct->ptls, nbytes);
            a->flags.how = 1;
            jl_gc_wb_buf(a, a->data, nbytes);
        }
    }
    if (JL_ARRAY_IMPL_NUL && elsz == 1 && !isbitsunion)
        memset((char*)a->data + oldnbytes - 1, 0, nbytes - oldnbytes + 1);
    (void)oldlen;
    assert(oldlen == a->nrows &&
           "Race condition detected: recursive resizing on the same array.");
    a->flags.isshared = 0;
    a->maxsize = newlen;
    return newbuf;
}

static void NOINLINE array_try_unshare(jl_array_t *a)
{
    if (a->flags.isshared) {
        if (a->flags.how != 3)
            jl_error("cannot resize array with shared data");
        // allow resizing when data is shared with a String
        if (jl_is_string(jl_array_data_owner(a)))
            return;
        assert(a->offset == 0);
        size_t len = a->maxsize;
        size_t nbytes = len * a->elsize;
        if (jl_array_isbitsunion(a)) {
            nbytes += len;
        }
        char *olddata = (char*)a->data;
        int newbuf = array_resize_buffer(a, len);
        assert(newbuf);
        (void)newbuf;
        memcpy(a->data, olddata, nbytes);
    }
}

size_t overallocation(size_t maxsize)
{
    if (maxsize < 8)
        return 8;
    // compute maxsize = maxsize + 4*maxsize^(7/8) + maxsize/8
    // for small n, we grow faster than O(n)
    // for large n, we grow at O(n/8)
    // and as we reach O(memory) for memory>>1MB,
    // this means we end by adding about 10% of memory each time
    int exp2 = sizeof(maxsize) * 8 -
#ifdef _P64
        __builtin_clzll(maxsize);
#else
        __builtin_clz(maxsize);
#endif
    maxsize += ((size_t)1 << (exp2 * 7 / 8)) * 4 + maxsize / 8;
    return maxsize;
}

STATIC_INLINE void jl_array_grow_at_beg(jl_array_t *a, size_t idx, size_t inc,
                                        size_t n)
{
    // designed to handle the case of growing and shrinking at both ends
    if (__unlikely(a->flags.isshared)) {
        if (a->flags.how != 3)
            jl_error("cannot resize array with shared data");
        if (inc == 0) {
            // If inc > 0, it will always trigger the slow path and unshare the
            // buffer
            array_try_unshare(a);
            return;
        }
    }
    size_t newnrows = n + inc;
    size_t elsz = a->elsize;
    size_t nbinc = inc * elsz;
    char *data = (char*)a->data;
    char *newdata;
    char *typetagdata = NULL;
    char *newtypetagdata = NULL;
    int isbitsunion = jl_array_isbitsunion(a);
    if (isbitsunion) typetagdata = jl_array_typetagdata(a);
    if (a->offset >= inc) {
        // already have enough space in a->offset
        newdata = data - nbinc;
        a->offset -= inc;
        if (isbitsunion) newtypetagdata = typetagdata - inc;
        if (idx > 0) {
            // inserting new elements after 1st element
            memmove_safe(a->flags.hasptr, newdata, data, idx * elsz);
            if (isbitsunion) {
                memmove(newtypetagdata, typetagdata, idx);
                memset(newtypetagdata + idx, 0, inc);
            }
        }
    }
    else {
        // not enough room for requested growth from existing a->offset
        size_t oldoffset = a->offset;
        size_t oldoffsnb = oldoffset * elsz;
        size_t oldmaxsize = a->maxsize;
        size_t nb1 = idx * elsz;
        if (inc > (a->maxsize - n) / 2 - (a->maxsize - n) / 20) {
            // not enough room for requested growth from end of array
            size_t newlen = inc * 2;
            while (n + 2 * inc > newlen - a->offset)
                newlen *= 2;
            size_t newmaxsize = overallocation(a->maxsize);
            if (newlen < newmaxsize)
                newlen = newmaxsize;
            size_t newoffset = (newlen - newnrows) / 2;
            if (!array_resize_buffer(a, newlen)) {
                data = (char*)a->data + oldoffsnb;
            }
            newdata = (char*)a->data + newoffset * elsz;
            if (isbitsunion) {
                typetagdata = data + (oldmaxsize - oldoffset) * elsz + oldoffset;
                newtypetagdata = newdata + (a->maxsize - newoffset) * elsz + newoffset;
                memmove(newtypetagdata, typetagdata, idx);
                memset(newtypetagdata + idx, 0, inc);
                memmove(newtypetagdata + idx + inc, typetagdata + idx, n - idx);
            }
            // We could use memcpy if resizing allocates a new buffer,
            // hopefully it's not a particularly important optimization.
            if (idx > 0 && newdata < data) {
                memmove_safe(a->flags.hasptr, newdata, data, nb1);
            }
            memmove_safe(a->flags.hasptr, newdata + nbinc + nb1, data + nb1, n * elsz - nb1);
            if (idx > 0 && newdata > data) {
                memmove_safe(a->flags.hasptr, newdata, data, nb1);
            }
            a->offset = newoffset;
        }
        else {
            // use extra space between a->nrows & a->maxsize
            a->offset = (a->maxsize - newnrows) / 2;
            newdata = data - oldoffsnb + a->offset * elsz;
            if (isbitsunion) newtypetagdata = newdata + (a->maxsize - a->offset) * elsz + a->offset;
            if (idx > 0 && newdata < data) {
                memmove_safe(a->flags.hasptr, newdata, data, nb1);
                if (isbitsunion) {
                    memmove(newtypetagdata, typetagdata, idx);
                    memset(newtypetagdata + idx, 0, inc);
                }
            }
            memmove_safe(a->flags.hasptr, newdata + nbinc + nb1, data + nb1, n * elsz - nb1);
            if (isbitsunion) memmove(newtypetagdata + idx + inc, typetagdata + idx, n - idx);
            if (idx > 0 && newdata > data) {
                memmove_safe(a->flags.hasptr, newdata, data, nb1);
                if (isbitsunion) {
                    memmove(newtypetagdata, typetagdata, idx);
                    memset(newtypetagdata + idx, 0, inc);
                }
            }
        }
    }
    a->length = newnrows;
    a->nrows = newnrows;
    a->data = newdata;
    if (jl_is_array_zeroinit(a)) {
        memset(newdata + idx * elsz, 0, nbinc);
    }
    if (newtypetagdata) {
        memset(newtypetagdata + idx, 0, inc);
    }
}

STATIC_INLINE void jl_array_grow_at_end(jl_array_t *a, size_t idx,
                                        size_t inc, size_t n)
{
    // optimized for the case of only growing and shrinking at the end
    if (__unlikely(a->flags.isshared)) {
        if (a->flags.how != 3)
            jl_error("cannot resize array with shared data");
        if (inc == 0) {
            // If inc > 0, it will always trigger the slow path and unshare the
            // buffer
            array_try_unshare(a);
            return;
        }
    }
    size_t elsz = a->elsize;
    char *data = (char*)a->data;
    char *typetagdata;
    char *newtypetagdata;
    int isbitsunion = jl_array_isbitsunion(a);
    if (isbitsunion) typetagdata = jl_array_typetagdata(a);
    int has_gap = n > idx;
    size_t reqmaxsize = a->offset + n + inc;
    if (__unlikely(reqmaxsize > a->maxsize)) {
        size_t nb1 = idx * elsz;
        size_t nbinc = inc * elsz;
        // grow either by our computed overallocation factor or exactly the requested size,
        // whichever is larger
        size_t newmaxsize = overallocation(a->maxsize);
        if (newmaxsize < reqmaxsize)
            newmaxsize = reqmaxsize;
        size_t oldmaxsize = a->maxsize;
        int newbuf = array_resize_buffer(a, newmaxsize);
        char *newdata = (char*)a->data + a->offset * elsz;
        if (isbitsunion) newtypetagdata = newdata + (a->maxsize - a->offset) * elsz + a->offset;
        if (newbuf) {
            memcpy(newdata, data, nb1);
            if (isbitsunion) {
                memcpy(newtypetagdata, typetagdata, idx);
                if (has_gap) memcpy(newtypetagdata + idx + inc, typetagdata + idx, n - idx);
                memset(newtypetagdata + idx, 0, inc);
            }
            if (has_gap) memcpy(newdata + nb1 + nbinc, data + nb1, n * elsz - nb1);
        }
        else {
            if (isbitsunion) {
                typetagdata = newdata + (oldmaxsize - a->offset) * elsz + a->offset;
                if (has_gap) memmove(newtypetagdata + idx + inc, typetagdata + idx, n - idx);
                memmove(newtypetagdata, typetagdata, idx);
                memset(newtypetagdata + idx, 0, inc);
            }
            if (has_gap) memmove_safe(a->flags.hasptr, newdata + nb1 + nbinc, newdata + nb1, n * elsz - nb1);
        }
        a->data = data = newdata;
    }
    else if (has_gap) {
        if (isbitsunion) {
            memmove(typetagdata + idx + inc, typetagdata + idx, n - idx);
            memset(typetagdata + idx, 0, inc);
        }
        size_t nb1 = idx * elsz;
        memmove_safe(a->flags.hasptr, data + nb1 + inc * elsz, data + nb1, n * elsz - nb1);
    }
    else {
        // there was enough room for requested growth already in a->maxsize
        if (isbitsunion)
            memset(typetagdata + idx, 0, inc);
    }
    size_t newnrows = n + inc;
    a->length = newnrows;
    a->nrows = newnrows;
    if (jl_is_array_zeroinit(a)) {
        memset(data + idx * elsz, 0, inc * elsz);
    }
}

JL_DLLEXPORT void jl_array_grow_at(jl_array_t *a, ssize_t idx, size_t inc)
{
    // No need to explicitly unshare.
    // Shared arrays are guaranteed to trigger the slow path for growing.
    size_t n = jl_array_nrows(a);
    if (idx < 0 || idx > n)
        jl_bounds_error_int((jl_value_t*)a, idx + 1);
    if (idx + 1 < n / 2) {
        jl_array_grow_at_beg(a, idx, inc, n);
    }
    else {
        jl_array_grow_at_end(a, idx, inc, n);
    }
}

JL_DLLEXPORT void jl_array_grow_end(jl_array_t *a, size_t inc)
{
    size_t n = jl_array_nrows(a);
    jl_array_grow_at_end(a, n, inc, n);
}

JL_DLLEXPORT void jl_array_grow_beg(jl_array_t *a, size_t inc)
{
    size_t n = jl_array_nrows(a);
    jl_array_grow_at_beg(a, 0, inc, n);
}

STATIC_INLINE void jl_array_shrink(jl_array_t *a, size_t dec)
{
    //if we don't manage this array return
    if (a->flags.how == 0) return;

    size_t elsz = a->elsize;
    size_t newbytes = (a->maxsize - dec) * a->elsize;
    size_t oldnbytes = (a->maxsize) * a->elsize;
    int isbitsunion = jl_array_isbitsunion(a);
    if (isbitsunion) {
        newbytes += a->maxsize - dec;
        oldnbytes += a->maxsize;
    }

    if (elsz == 1 && !isbitsunion) {
        newbytes++;
        oldnbytes++;
    }
    char *originalptr = ((char*) a->data) - a->offset * a->elsize;
    if (a->flags.how == 1) {
        //this is a julia-allocated buffer that needs to be marked
        char *typetagdata;
        char *newtypetagdata;
        if (isbitsunion) {
            typetagdata = (char*)malloc_s(a->nrows);
            memcpy(typetagdata, jl_array_typetagdata(a), a->nrows);
        }
        jl_task_t *ct = jl_current_task;
        char *originaldata = (char*) a->data - a->offset * a->elsize;
        char *newdata = (char*)jl_gc_alloc_buf(ct->ptls, newbytes);
        jl_gc_wb_buf(a, newdata, newbytes);
        a->maxsize -= dec;
        if (isbitsunion) {
            newtypetagdata = jl_array_typetagdata(a);
            memcpy(newtypetagdata, typetagdata, a->nrows);
            free(typetagdata);
        }
        memcpy(newdata, originaldata, newbytes);
        a->data = newdata + a->offset * elsz;
    }
    else if (a->flags.how == 2) {
        //malloc-allocated pointer this array object manages
        char *typetagdata;
        char *newtypetagdata;
        if (isbitsunion) {
            typetagdata = (char*)malloc_s(a->nrows);
            memcpy(typetagdata, jl_array_typetagdata(a), a->nrows);
        }
        size_t oldoffsnb = a->offset * elsz;
        a->data = ((char*)jl_gc_managed_realloc(originalptr, newbytes, oldnbytes,
                a->flags.isaligned, (jl_value_t*) a)) + oldoffsnb;
        a->maxsize -= dec;
        if (isbitsunion) {
            newtypetagdata = jl_array_typetagdata(a);
            memcpy(newtypetagdata, typetagdata, a->nrows);
            free(typetagdata);
        }
    }
    else if (a->flags.how == 3) {
        //this has has a pointer to the object that owns the data
    }
}

static size_t jl_array_limit_offset(jl_array_t *a, size_t offset)
{
    // make sure offset doesn't grow forever due to deleting at beginning
    // and growing at end
    if (offset >= 13 * a->maxsize / 20)
        offset = 17 * (a->maxsize - a->nrows) / 100;
#ifdef _P64
    while (offset > (size_t)UINT32_MAX) {
        offset /= 2;
    }
#endif
    return offset;
}

STATIC_INLINE void jl_array_del_at_beg(jl_array_t *a, size_t idx, size_t dec,
                                       size_t n)
{
    // no error checking
    // assume inbounds, assume unshared
    size_t elsz = a->elsize;
    size_t offset = a->offset;
    int isbitsunion = jl_array_isbitsunion(a);
    offset += dec;
    a->length = n - dec;
    a->nrows = n - dec;
    size_t newoffs = jl_array_limit_offset(a, offset);
    assert(newoffs <= offset);
    size_t nbdec = dec * elsz;
    if (__unlikely(newoffs != offset) || idx > 0) {
        char *olddata = (char*)a->data;
        char *newdata = olddata - (a->offset - newoffs) * elsz;
        char *typetagdata;
        char *newtypetagdata;
        if (isbitsunion) {
            typetagdata = jl_array_typetagdata(a);
            newtypetagdata = typetagdata - (a->offset - newoffs);
        }

        size_t nb1 = idx * elsz; // size in bytes of the first block
        size_t nbtotal = a->nrows * elsz; // size in bytes of the new array
        // Implicit '\0' for byte arrays
        if (elsz == 1 && !isbitsunion)
            nbtotal++;
        if (idx > 0) {
            memmove_safe(a->flags.hasptr, newdata, olddata, nb1);
            if (isbitsunion) memmove(newtypetagdata, typetagdata, idx);
        }
        // Move the rest of the data if the offset changed
        if (newoffs != offset) {
            memmove_safe(a->flags.hasptr, newdata + nb1, olddata + nb1 + nbdec, nbtotal - nb1);
            if (isbitsunion) memmove(newtypetagdata + idx, typetagdata + idx + dec, a->nrows - idx);
        }
        a->data = newdata;
    }
    else {
        char *data = (char*)a->data;
        a->data = data + nbdec;
    }
    a->offset = newoffs;
}

STATIC_INLINE void jl_array_del_at_end(jl_array_t *a, size_t idx, size_t dec,
                                       size_t n)
{
    // no error checking
    // assume inbounds, assume unshared
    char *data = (char*)a->data;
    size_t elsz = a->elsize;
    int isbitsunion = jl_array_isbitsunion(a);
    size_t last = idx + dec;
    if (n > last) {
        memmove_safe(a->flags.hasptr, data + idx * elsz, data + last * elsz, (n - last) * elsz);
        if (isbitsunion) {
            char *typetagdata = jl_array_typetagdata(a);
            memmove(typetagdata + idx, typetagdata + last, n - last);
        }
    }
    n -= dec;
    if (elsz == 1 && !isbitsunion)
        data[n] = 0;
    a->nrows = n;
    a->length = n;
}

JL_DLLEXPORT void jl_array_del_at(jl_array_t *a, ssize_t idx, size_t dec)
{
    size_t n = jl_array_nrows(a);
    size_t last = idx + dec;
    if (__unlikely(idx < 0))
        jl_bounds_error_int((jl_value_t*)a, idx + 1);
    if (__unlikely(last > n))
        jl_bounds_error_int((jl_value_t*)a, last);
    // The unsharing needs to happen before we modify the buffer
    if (__unlikely(a->flags.isshared))
        array_try_unshare(a);
    if (idx < n - last) {
        jl_array_del_at_beg(a, idx, dec, n);
    }
    else {
        jl_array_del_at_end(a, idx, dec, n);
    }
}

JL_DLLEXPORT void jl_array_del_beg(jl_array_t *a, size_t dec)
{
    size_t n = jl_array_nrows(a);
    if (__unlikely(dec > n))
        jl_bounds_error_int((jl_value_t*)a, dec);
    if (__unlikely(a->flags.isshared))
        array_try_unshare(a);
    if (dec == 0)
        return;
    jl_array_del_at_beg(a, 0, dec, n);
}

JL_DLLEXPORT void jl_array_del_end(jl_array_t *a, size_t dec)
{
    size_t n = jl_array_nrows(a);
    if (__unlikely(n < dec))
        jl_bounds_error_int((jl_value_t*)a, 0);
    if (__unlikely(a->flags.isshared))
        array_try_unshare(a);
    if (dec == 0)
        return;
    jl_array_del_at_end(a, n - dec, dec, n);
}

JL_DLLEXPORT void jl_array_sizehint(jl_array_t *a, size_t sz)
{
    size_t n = jl_array_nrows(a);

    size_t min = a->offset + a->length;
    sz = (sz < min) ? min : sz;

    if (sz <= a->maxsize) {
        size_t dec = a->maxsize - sz;
        //if we don't save at least an eighth of maxsize then its not worth it to shrink
        if (dec <= a->maxsize / 8) return;
        jl_array_shrink(a, dec);
    }
    else {
        size_t inc = sz - n;
        jl_array_grow_end(a, inc);

        a->nrows = n;
        a->length = n;
    }
}

JL_DLLEXPORT jl_array_t *jl_array_copy(jl_array_t *ary)
{
    size_t elsz = ary->elsize;
    size_t len = jl_array_len(ary);
    int isunion = jl_is_uniontype(jl_tparam0(jl_typeof(ary)));
    jl_array_t *new_ary = _new_array_(jl_typeof(ary), jl_array_ndims(ary),
                                      &ary->nrows, !ary->flags.ptrarray,
                                      ary->flags.hasptr, isunion, 0, elsz);
    memcpy(new_ary->data, ary->data, len * elsz);
    // ensure isbits union arrays copy their selector bytes correctly
    if (jl_array_isbitsunion(ary))
        memcpy(jl_array_typetagdata(new_ary), jl_array_typetagdata(ary), len);
    return new_ary;
}

// Copy element by element until we hit a young object, at which point
// we can finish by using `memmove`.
static NOINLINE ssize_t jl_array_ptr_copy_forward(jl_value_t *owner,
                                                  void **src_p, void **dest_p,
                                                  ssize_t n) JL_NOTSAFEPOINT
{
    _Atomic(void*) *src_pa = (_Atomic(void*)*)src_p;
    _Atomic(void*) *dest_pa = (_Atomic(void*)*)dest_p;
    for (ssize_t i = 0; i < n; i++) {
        void *val = jl_atomic_load_relaxed(src_pa + i);
        jl_atomic_store_release(dest_pa + i, val);
        // `val` is young or old-unmarked
        if (val && !(jl_astaggedvalue(val)->bits.gc & GC_MARKED)) {
            jl_gc_queue_root(owner);
            return i;
        }
    }
    return n;
}

static NOINLINE ssize_t jl_array_ptr_copy_backward(jl_value_t *owner,
                                                   void **src_p, void **dest_p,
                                                   ssize_t n) JL_NOTSAFEPOINT
{
    _Atomic(void*) *src_pa = (_Atomic(void*)*)src_p;
    _Atomic(void*) *dest_pa = (_Atomic(void*)*)dest_p;
    for (ssize_t i = 0; i < n; i++) {
        void *val = jl_atomic_load_relaxed(src_pa + n - i - 1);
        jl_atomic_store_release(dest_pa + n - i - 1, val);
        // `val` is young or old-unmarked
        if (val && !(jl_astaggedvalue(val)->bits.gc & GC_MARKED)) {
            jl_gc_queue_root(owner);
            return i;
        }
    }
    return n;
}

// Unsafe, assume inbounds and that dest and src have the same eltype
JL_DLLEXPORT void jl_array_ptr_copy(jl_array_t *dest, void **dest_p,
                                    jl_array_t *src, void **src_p, ssize_t n) JL_NOTSAFEPOINT
{
    assert(dest->flags.ptrarray && src->flags.ptrarray);
    jl_value_t *owner = jl_array_owner(dest);
    // Destination is old and doesn't refer to any young object
    if (__unlikely(jl_astaggedvalue(owner)->bits.gc == GC_OLD_MARKED)) {
        jl_value_t *src_owner = jl_array_owner(src);
        // Source is young or being promoted or might refer to young objects
        // (i.e. source is not an old object that doesn't have wb triggered)
        if (jl_astaggedvalue(src_owner)->bits.gc != GC_OLD_MARKED) {
            ssize_t done;
            if (dest_p < src_p || dest_p > src_p + n) {
                done = jl_array_ptr_copy_forward(owner, src_p, dest_p, n);
                dest_p += done;
                src_p += done;
            }
            else {
                done = jl_array_ptr_copy_backward(owner, src_p, dest_p, n);
            }
            n -= done;
        }
    }
    memmove_refs(dest_p, src_p, n);
}

JL_DLLEXPORT void jl_array_ptr_1d_push(jl_array_t *a, jl_value_t *item)
{
    assert(jl_typetagis(a, jl_array_any_type));
    jl_array_grow_end(a, 1);
    size_t n = jl_array_nrows(a);
    jl_array_ptr_set(a, n - 1, item);
}

JL_DLLEXPORT void jl_array_ptr_1d_append(jl_array_t *a, jl_array_t *a2)
{
    assert(jl_typetagis(a, jl_array_any_type));
    assert(jl_typetagis(a2, jl_array_any_type));
    size_t i;
    size_t n = jl_array_nrows(a);
    size_t n2 = jl_array_nrows(a2);
    jl_array_grow_end(a, n2);
    for (i = 0; i < n2; i++) {
        jl_array_ptr_set(a, n + i, jl_array_ptr_ref(a2, i));
    }
}

JL_DLLEXPORT jl_value_t *(jl_array_data_owner)(jl_array_t *a) JL_NOTSAFEPOINT
{
    return jl_array_data_owner(a);
}

STATIC_INLINE int jl_has_implicit_byte_owned(jl_array_t *a)
{
    assert(a->flags.how != 3);
    if (!a->flags.isshared)
        return 1;
    return a->flags.how == 1;
}

STATIC_INLINE int jl_has_implicit_byte(jl_array_t *a)
{
    // * unshared:
    //   * how: 0-2
    //     We own and allocated the data.
    //     It should have the extra byte.
    // * shared:
    //   * how: 0, 2
    //     The data might come from external source without implicit NUL byte.
    //     There could be an entra byte for a `reinterpreted` array
    //     but that should be unlikely for strings.
    //   * how: 1
    //     We allocated the data with the extra byte.
    //   * how: 3
    //     We should check the owner.
    if (a->flags.how == 3) {
        a = (jl_array_t*)jl_array_data_owner(a);
        if (jl_is_string(a)) return 1;
        return a->elsize == 1 && jl_has_implicit_byte_owned(a);
    }
    return jl_has_implicit_byte_owned(a);
}

// Create an array with the same content
JL_DLLEXPORT jl_array_t *jl_array_cconvert_cstring(jl_array_t *a)
{
    assert(jl_typeof(a) == jl_array_uint8_type);
    if (!jl_has_implicit_byte(a))
        a = jl_array_copy(a);
    ((char*)a->data)[a->nrows] = 0;
    return a;
}

#ifdef __cplusplus
}
#endif
