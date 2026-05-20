/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file array.h
 * @brief Type-safe dynamic array macros for C.
 *
 * This header provides a macro system for declaring strongly-typed, heap-backed
 * dynamic arrays.  Two flavors are offered:
 *
 *   - @c DECLARE_ARRAY(name, T) -- a plain dynamic array of value type @p T.
 *     Elements are copied by value on push and not individually freed on array
 *     destruction.
 *
 *   - @c DECLARE_OWNING_PTR_ARRAY(name, T, cleanup) -- a dynamic array of
 *     pointer type @p T that @em owns its elements.  On array destruction (or
 *     element removal), each element is passed through the @p cleanup function
 *     to free its resources.
 *
 * Both macros generate an inline API following the naming convention
 * @c name_init, @c name_push, @c name_pop, @c name_resize, @c name_free, etc.
 *
 * @par Capacity strategy
 * The backing buffer grows in powers of two (via @c bit_ceil) to amortize
 * reallocation cost.  A hysteresis check avoids thrashing when the length
 * oscillates around a power-of-two boundary.
 *
 * @par Usage example
 * @code
 *   // Declare a dynamic array of int:
 *   DECLARE_ARRAY(int_array, int)
 *
 *   // Use it:
 *   struct int_array arr = int_array_init();
 *   int_array_push(&arr, 42);
 *   int_array_push(&arr, 7);
 *   printf("len=%zu, first=%d\n", arr.len, arr.d[0]);  // len=2, first=42
 *   int_array_free(&arr);
 *
 *   // Declare an owning pointer array with per-element cleanup:
 *   DECLARE_OWNING_PTR_ARRAY(widget_array, struct widget *, cleanup_widget)
 *
 *   struct widget_array widgets = widget_array_init();
 *   struct widget *w = widget_new();
 *   widget_array_push_move(&widgets, &w);   // w is now NULL, array owns it
 *   widget_array_free(&widgets);            // calls cleanup_widget on each element
 * @endcode
 */

#ifndef VRTD_ARRAY_H
#define VRTD_ARRAY_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>

#include "utils.h"

/**
 * @brief Internal implementation macro shared by DECLARE_ARRAY and DECLARE_OWNING_PTR_ARRAY.
 *
 * Generates the struct definition and common operations: init, resize, push,
 * pop, pop_safe, zero, shrink_to_fit, rm_by_value_impl, and free_impl.
 *
 * @param T_ARRAY Name of the generated array struct.
 * @param T       Element type.
 */
// DECLARE_ARRAY declares a dynamic array of type T.
//
// Access is done directly via a.d[i] and a.len. The 0-value is the 0-len array.
//
#define DECLARE_ARRAY_IMPL(T_ARRAY, T) \
    struct T_ARRAY { \
        /** @brief Pointer to the heap-allocated element storage. */ \
        T *d; \
        /** @brief Number of elements currently in the array. */ \
        size_t len; \
        /** @brief Allocated capacity (always a power of two, or zero). */ \
        size_t cap; \
    }; \
    \
    /** @brief Return a zero-initialized (empty) array. */ \
    static inline \
    struct T_ARRAY T_ARRAY##_init(void) { \
        return (struct T_ARRAY) { \
            .d = NULL, \
            .len = 0, \
            .cap = 0, \
        }; \
    } \
    \
    /** @brief Resize the array to exactly @p len elements, reallocating if needed.
     *  @param arr The array to resize.
     *  @param len The new length.
     *  @return 0 on success, -1 on allocation failure. */ \
    static inline NODISCARD \
    int T_ARRAY##_resize(struct T_ARRAY *arr, size_t len) \
    { \
        size_t cap = likely(len > 0) ? bit_ceil(len) : 0; \
        T *d; \
    \
        /* Don't constantly reallocate for add-remove 1024-1025 elements */ \
        /* This may reallocate unnecessarily (once) for tightened arrays but this is fine */ \
        /* Tighthening should only be done for arrays that will keep their size for a long time */ \
        /* Otherwise we'd have to complicate this hot comparison */ \
        if (cap == arr->cap || cap == (arr->cap >> 1)) { \
            arr->len = len; \
            return 0; \
        } \
    \
        d = (T *) reallocarray(arr->d, cap, sizeof(T)); \
        if (unlikely(d == NULL && cap != 0)) { \
            return -1; \
        } \
    \
        arr->d = d; \
        arr->len = len; \
        arr->cap = cap; \
    \
        return 0; \
    } \
    \
    /** @brief Append an element to the end of the array.
     *  @param arr The array to push to.
     *  @param v   The value to append.
     *  @return 0 on success, -1 on allocation failure. */ \
    static inline NODISCARD \
    int T_ARRAY##_push(struct T_ARRAY *arr, T v) { \
        if (unlikely(T_ARRAY##_resize(arr, arr->len + 1) == -1)) { \
            return -1; \
        } \
        arr->d[arr->len - 1] = v; \
        return 0; \
    } \
    \
    /** @brief Remove and optionally return the last element.
     *  @param arr The array to pop from.
     *  @param out If non-NULL, receives the removed element.
     *  @return 0 on success, -1 if the array is empty or resize fails. */ \
    static inline int T_ARRAY##_pop(struct T_ARRAY *arr, T *out) { \
        if (arr->len == 0) { \
            return -1; \
        } \
    \
        if (out != NULL) { \
            *out = arr->d[arr->len - 1]; \
        } \
    \
        return T_ARRAY##_resize(arr, arr->len - 1); \
    } \
    \
    /** @brief Remove and optionally return the last element (no-op if empty, no resize).
     *  @param arr The array to pop from.
     *  @param out If non-NULL, receives the removed element. */ \
    static inline void T_ARRAY##_pop_safe(struct T_ARRAY *arr, T *out) { \
        if (arr->len == 0) { \
            return; \
        } \
    \
        if (out != NULL) { \
            *out = arr->d[arr->len - 1]; \
        } \
    \
        arr->len--; \
    } \
    \
    /** @brief Zero all bytes in the allocated capacity (not just len elements).
     *  @param arr The array to zero. */ \
    static inline \
    void T_ARRAY##_zero(struct T_ARRAY *arr) \
    { \
        memset(arr->d, 0, arr->cap * sizeof(T)); \
    } \
    \
    /** @brief Reallocate backing storage to exactly fit the current length.
     *  @param arr The array to shrink.
     *  @return 0 on success, -1 on allocation failure. */ \
    static inline \
    int T_ARRAY##_shrink_to_fit(struct T_ARRAY *arr) \
    { \
        T *d; \
    \
        d = (T *) reallocarray(arr->d, arr->len, sizeof(T)); \
        if (unlikely(d == NULL && arr->len != 0)) { \
            return -1; \
        } \
    \
        arr->d = d; \
        arr->cap = arr->len; \
    \
        return 0; \
    } \
    \
    /** @brief Remove all occurrences of @p value from the array (compacting). Internal impl.
     *  @param arr   The array to compact.
     *  @param value The value to remove. */ \
    static inline \
    void T_ARRAY##_rm_by_value_impl(struct T_ARRAY *arr, T value) \
    { \
        size_t j = 0; \
        for (size_t i = 0; i < arr->len; i++) { \
            if (arr->d[i] == value) { \
                continue; \
            } \
            \
            arr->d[j++] = arr->d[i]; \
        } \
        \
        arr->len = j; \
    } \
    \
    /** @brief Free the backing storage and reset the array to empty. Internal impl.
     *  @param arr The array to free. */ \
    static inline \
    void T_ARRAY##_free_impl(struct T_ARRAY *arr) \
    { \
        free(arr->d); \
    \
        arr->d = NULL; \
        arr->len = 0; \
        arr->cap = 0; \
    }

/**
 * @brief Declare a non-owning dynamic array of value type @p T.
 *
 * Generates struct @p T_ARRAY and inline functions: _init, _resize, _push,
 * _pop, _pop_safe, _zero, _shrink_to_fit, _free, _freep, _rm_by_value.
 *
 * @param T_ARRAY Name for the generated array struct.
 * @param T       Element type (must be copyable by assignment).
 */
#define DECLARE_ARRAY(T_ARRAY, T) \
    DECLARE_ARRAY_IMPL(T_ARRAY, T) \
    \
    /** @brief Free the array's backing storage and reset to empty.
     *  @param arr The array to free. */ \
    static inline \
    void T_ARRAY##_free(struct T_ARRAY *arr) \
    { \
        T_ARRAY##_free_impl(arr); \
    } \
\
    /** @brief Cleanup helper for use with __attribute__((cleanup)).
     *  @param arr Address of a struct T_ARRAY pointer. */ \
    static inline \
    void T_ARRAY##_freep(struct T_ARRAY **arr) \
    { \
        T_ARRAY##_free(*arr); \
        *arr = NULL; \
    } \
    \
    /** @brief Remove all occurrences of @p value from the array (compacting, no cleanup).
     *  @param arr   The array to compact.
     *  @param value The value to remove. */ \
    static inline \
    void T_ARRAY##_rm_by_value(struct T_ARRAY *arr, T value) \
    { \
        T_ARRAY##_rm_by_value_impl(arr, value); \
    }

/**
 * @brief Declare an owning dynamic array of pointer type @p T with per-element cleanup.
 *
 * Like DECLARE_ARRAY, but additionally generates:
 *   - @c _push_move: transfers ownership of a pointer into the array (NULLs the source).
 *   - @c _free: calls @p CLEANUP on every element before freeing the storage.
 *   - @c _rm_by_reference: removes an element by pointer equality, calling @p CLEANUP on it.
 *
 * @param T_ARRAY Name for the generated array struct.
 * @param T       Element type (must be a pointer type).
 * @param CLEANUP Function called on each element during destruction, signature: void CLEANUP(T).
 */
#define DECLARE_OWNING_PTR_ARRAY(T_ARRAY, T, CLEANUP) \
    DECLARE_ARRAY_IMPL(T_ARRAY, T) \
    \
    /** @brief Transfer ownership of @p *ptr into the array.  Sets *ptr to NULL on success.
     *  @param arr The array to push to.
     *  @param ptr Address of the pointer to move in (set to NULL on success).
     *  @return 0 on success, -1 on allocation failure (*ptr unchanged). */ \
    static inline \
    int T_ARRAY##_push_move(struct T_ARRAY *arr, T *ptr) \
    { \
        int ret = T_ARRAY##_push(arr, *ptr); \
        if (unlikely(ret == -1)) { \
            return -1; \
        } \
    \
        *ptr = NULL; \
    \
        return 0; \
    } \
    \
    /** @brief Free the array, calling CLEANUP on each element first.
     *  @param arr The array to free. */ \
    static inline \
    void T_ARRAY##_free(struct T_ARRAY *arr) \
    { \
        for (size_t i = 0; i < arr->len; ++i) { \
            CLEANUP(arr->d[i]); \
        } \
    \
        T_ARRAY##_free_impl(arr); \
    } \
    \
    /** @brief Remove an element by pointer equality, calling CLEANUP on it.  Compacts the array.
     *  @param arr   The array to remove from.
     *  @param value The pointer to find and remove. */ \
    static inline \
    void T_ARRAY##_rm_by_reference(struct T_ARRAY *arr, T value) \
    { \
        size_t j = 0; \
        for (size_t i = 0; i < arr->len; i++) { \
            if (arr->d[i] == value) { \
                CLEANUP(arr->d[i]);  \
                continue; \
            } \
            arr->d[j++] = arr->d[i]; \
        } \
        arr->len = j; \
    }

/* Pre-declared array types used throughout the daemon. */

/** @brief Dynamic array of int values. */
DECLARE_ARRAY(int_array, int)
/** @brief Dynamic array of unsigned int values. */
DECLARE_ARRAY(uint_array, unsigned int)
/** @brief Dynamic array of gid_t values (POSIX group IDs). */
DECLARE_ARRAY(gid_t_array, gid_t)
/** @brief Owning dynamic array of heap-allocated strings (freed with free()). */
DECLARE_OWNING_PTR_ARRAY(str_array, char *, free)

#endif // VRTD_ARRAY_H
