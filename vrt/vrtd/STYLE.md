# vrtd Coding Style

This document describes the coding conventions used throughout vrtd.
They are descriptive — documenting patterns already established in the
codebase — not aspirational. Contributors should follow them for
consistency, clarity, and correctness.

## Language and toolchain

vrtd is **not** written in portable POSIX C. It intentionally uses C11,
GNU compiler extensions (supported by both GCC and Clang), glibc
features, libsystemd, and Linux-specific syscalls. The goal is a modern
systemd daemon that leverages all the tools at our disposal.

The language standard is **C11** with GNU extensions (`-std=gnu11`).
C23 features should not be used unless they are available as GNU
extensions under `-std=gnu11`.

The minimum required versions are those shipped by Ubuntu 22.04 LTS:

| Dependency | Minimum version |
|------------|-----------------|
| CMake | 3.22.1 |
| GCC | 11.4.0 |
| glibc | 2.35 |
| Linux | 5.15.0 |
| libsystemd | 249.11 |

All `.c` source files must start with `#define _GNU_SOURCE` after the
copyright header and before including any other headers.

## Error handling

There is no generally idiomatic way to do error handling in C. Each
project uses its own convention. The following is the convention for
vrtd.

### Return value convention

Functions that can fail return an `int`: `-1` for failure, `0` for
success.

- If a function would naturally be `void` but can fail, make it return
  `int` instead.
- If a function would naturally return a non-`int` type but can fail,
  return `int` and pass the result via a pointer parameter.
- Do not use integers of smaller sizes for the return value. The value
  is returned as a constant, checked immediately, and never used again
  — it will only ever live in a register.
- Do **not** return other negative values as failures. The error
  handling macros check `== -1` specifically.

When a function needs to return a value but can also fail, prefer
the pointer-out pattern:

```c
/* Before: cannot signal failure */
double div(double x, double y)
{
    return x / y;
}

/* After: returns -1 on failure, result via pointer */
int div_safe(double x, double y, double *result)
{
    if (y == 0.0) {
        return -1;
    }

    if (result == NULL) {
        /* Caller does not need the result — not an error. */
        return 0;
    }

    *result = x / y;

    return 0;
}
```

Use this pattern also when `-1` is a valid value for an `int` result.

### The PROPAGATE_ERROR macro family

`utils.h` provides a set of macros that make error handling more
ergonomic. Each macro evaluates an expression, checks the result
against a type-specific failure condition, and — if the check fails —
optionally logs a message via `sd_journal_print` and returns `-1` from
the calling function.

| Macro | Failure condition | Logging |
|-------|-------------------|---------|
| `PROPAGATE_ERROR(expr)` | `== -1` | None |
| `PROPAGATE_ERROR_NULL(expr)` | `== NULL` | None |
| `PROPAGATE_ERROR_SD(expr)` | `< 0` | None |
| `PROPAGATE_ERROR_LOG(expr, LVL, FMT, ...)` | `== -1` | `sd_journal_print` |
| `PROPAGATE_ERROR_NULL_LOG(expr, LVL, FMT, ...)` | `== NULL` | `sd_journal_print` |
| `PROPAGATE_ERROR_STDC_LOG(expr, LVL, FMT, ...)` | `== -1` | Appends `: %m` (strerror) |
| `PROPAGATE_ERROR_NULL_STDC_LOG(expr, LVL, FMT, ...)` | `== NULL` | Appends `: %m` (strerror) |
| `PROPAGATE_ERROR_SD_LOG(expr, LVL, FMT, ...)` | `< 0` | Appends systemd error string |

The three conventions handled are:
- **POSIX** (`== -1`): used by most vrtd internal functions.
- **NULL** (`== NULL`): used for allocation and pointer-returning functions.
- **systemd** (`< 0`): used for `sd_*` library calls that return negative errno.

Example:

```c
int foo(void)
{
    int ret = bar();
    PROPAGATE_ERROR(ret);

    /* bar() succeeded — continue. */

    ret = open_resource();
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to open resource");

    return 0;
}
```

If `bar()` returns `-1`, `foo()` immediately returns `-1`. If
`open_resource()` returns `-1`, `foo()` logs the error (with `errno`
context) and returns `-1`.

### goto-based cleanup

When a function acquires multiple resources and `PROPAGATE_ERROR` alone
cannot release them, use a `goto fail` pattern with a cleanup label:

```c
int multi_resource_init(struct thing *t)
{
    /* Zero-initialise so cleanup is safe on early failure. */
    *t = (struct thing) {
        .fd = -1,
        .buf = NULL,
        .resource_acquired = false,
    };

    int ret = acquire_first(t);
    if (ret == -1) {
        goto fail;
    }

    t->resource_acquired = true;

    ret = acquire_second(t);
    if (ret == -1) {
        goto fail;
    }

    return 0;

fail:
    cleanup_thing(t);
    return -1;
}
```

The cleanup function inspects flags and sentinel values (like `fd == -1`
or `resource_acquired == false`) to safely skip resources that were
never acquired.

## Resource management (RAII in C)

### The `_cleanup_` macro

vrtd uses `__attribute__((cleanup))` for automatic resource cleanup
when variables go out of scope — RAII-style resource management in C:

```c
#define _cleanup_(FOO) __attribute__((cleanup(FOO)))
```

When a variable annotated with `_cleanup_(fn)` goes out of scope, the
compiler calls `fn` with a pointer to that variable.

### Standard cleanup functions

| Function | Type | Action |
|----------|------|--------|
| `cleanup_free` | `void *` | `free()` + NULL |
| `cleanup_argv` | `char **` | Frees each string, then the array, + NULL |

Module-specific cleanup functions follow the naming convention:
- `cleanup_<type>(T *)` — primary cleanup (e.g. `cleanup_buffer`,
  `cleanup_role`)
- `cleanup_<type>p(T **)` — indirect variant for use with `_cleanup_`;
  calls `cleanup_<type>(*p)` and NULLs `*p`

### Usage

```c
_cleanup_(cleanup_free)
char *name = NULL;

int ret = asprintf(&name, "device-%u", id);
PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Allocation failed");

/* name is automatically freed when this scope exits,
   whether by return, goto, or reaching the closing brace. */
```

## Ownership conventions

### Struct member annotations

Annotate struct members with comments to document who is responsible
for freeing a resource:

```c
struct buffer {
    struct slash_qdma *qdma;           /* non-owning */
    struct device_memory_map *map;     /* non-owning */
    uint64_t client_id;                /* owning connection id */
    int fd;                            /* owning (must be closed) */
};
```

- `/* non-owning */` — borrowed reference; the struct must not free it.
- `/* owning */` — the struct is responsible for releasing it.

### Partial initialization tracking

When a constructor acquires multiple resources, use boolean flags to
track which resources were successfully created:

```c
struct buffer {
    /* ... */
    bool allocation_valid;   /* address-space allocation exists */
    bool qpair_created;      /* QDMA queue pair exists */
};
```

Zero-initialize the struct (via designated initializers) before
beginning the multi-step setup. The cleanup function checks the flags
to skip resources that were never acquired:

```c
*buf = (struct buffer) {
    .fd = -1,
    .allocation_valid = false,
    .qpair_created = false,
};
```

### Ownership transfer

The `push_move` function (generated by `DECLARE_OWNING_PTR_ARRAY`)
transfers ownership of a pointer into the array by NULLing the source.
This prevents double-free:

```c
struct widget *w = widget_new();
widget_array_push_move(&widgets, &w);
/* w is now NULL — only the array owns the widget. */
```

## Type-safe generic data structures

`array.h` provides macro-generated dynamic arrays in two flavors.

### `DECLARE_ARRAY(name, T)`

Declares a value-type dynamic array. Elements are copied on push and
not individually freed on array destruction.

```c
DECLARE_ARRAY(int_array, int)

struct int_array arr = int_array_init();
int_array_push(&arr, 42);
printf("len=%zu, first=%d\n", arr.len, arr.d[0]);
int_array_free(&arr);
```

### `DECLARE_OWNING_PTR_ARRAY(name, T, cleanup)`

Declares a pointer-type dynamic array that owns its elements. On
destruction (or element removal), each element is passed through the
cleanup function.

```c
DECLARE_OWNING_PTR_ARRAY(widget_array, struct widget *, cleanup_widget)

struct widget_array widgets = widget_array_init();
struct widget *w = widget_new();
widget_array_push_move(&widgets, &w);   /* w is now NULL */
widget_array_free(&widgets);            /* calls cleanup_widget on each */
```

### Generated API

Both macros generate: `_init`, `_push`, `_pop`, `_pop_safe`, `_resize`,
`_shrink_to_fit`, `_zero`, `_free`.

`DECLARE_OWNING_PTR_ARRAY` additionally generates: `_push_move`,
`_rm_by_reference`.

### Pre-declared array types

| Type | Element | Flavor |
|------|---------|--------|
| `int_array` | `int` | value |
| `uint_array` | `unsigned int` | value |
| `gid_t_array` | `gid_t` | value |
| `str_array` | `char *` | owning (freed with `free`) |

## GNU extensions

The following GCC/Clang extensions are used throughout vrtd:

| Extension | vrtd usage | Purpose |
|-----------|-----------|---------|
| `__attribute__((cleanup))` | `_cleanup_(fn)` | RAII-style automatic cleanup |
| `__attribute__((warn_unused_result))` | `NODISCARD` | Force callers to check return values |
| `__auto_type` | `PROPAGATE_ERROR_*`, `max`, `min` | Type inference without double-evaluation |
| Statement expressions `({ })` | `max(a,b)`, `min(a,b)`, `PROPAGATE_ERROR_*` | Multi-statement macros that yield a value |
| `__builtin_expect` | `likely(x)`, `unlikely(x)` | Branch prediction hints |
| `__builtin_clz`, `__builtin_clzll` | `bit_ceil_u32`, `bit_ceil_u64` | Count leading zeros for power-of-two rounding |
| `_Generic` | `bit_ceil(n)` | Type-generic dispatch (C11, used with GCC builtins) |
| `##__VA_ARGS__` | `PROPAGATE_ERROR_*_LOG` | Suppress trailing comma with zero variadic args |
| `_GNU_SOURCE` glibc | `reallocarray`, `asprintf`, `strerrordesc_np`, `%m` | Extended libc functions |

## Naming conventions

| Element | Convention | Examples |
|---------|-----------|----------|
| Functions, variables | `snake_case` | `buffer_create`, `client_id` |
| Module prefixes | `<module>_` | `buffer_`, `allocator_`, `auth_`, `config_`, `cleanup_` |
| Cleanup functions | `cleanup_<type>` / `cleanup_<type>p` | `cleanup_buffer`, `cleanup_bufferp` |
| Macros, constants | `UPPER_CASE` | `PROPAGATE_ERROR`, `LOG`, `HBM_REGIONS` |
| Struct types | `struct snake_case` (no typedefs) | `struct buffer`, `struct client` |
| Enum values | `UPPER_CASE` with type prefix | `ALLOCATION_TYPE_DDR`, `ALLOCATION_RESULT_SUCCESS` |
| Header guards | `VRTD_<NAME>_H` | `VRTD_UTILS_H`, `VRTD_BUFFER_H` |

## Formatting

- **Indentation**: 4 spaces. No tabs.
- **Control flow braces**: opening brace on the same line.
  ```c
  if (ret == -1) {
      return -1;
  }
  ```
- **Function definition braces**: opening brace on its own line.
  ```c
  int buffer_create(struct buffer *buf)
  {
      /* ... */
  }
  ```
  Short `static inline` helpers in headers may use same-line braces.
- **Designated initializers**: always used for struct initialization.
  ```c
  *buf = (struct buffer) {
      .fd = -1,
      .allocation_valid = false,
  };
  ```

## File structure

### Source files (`.c`)

1. MIT license header (block comment)
2. Doxygen `@file` / `@brief` comment
3. `#define _GNU_SOURCE`
4. Local (project) headers with quotes: `"buffer.h"`, `"utils.h"`
5. System headers with angle brackets: `<stdlib.h>`, `<errno.h>`
6. systemd headers: `<systemd/sd-journal.h>`, `<systemd/sd-event.h>`
7. File-scope constants and static prototypes
8. Function implementations

### Header files (`.h`)

1. MIT license header
2. Doxygen `@file` / `@brief` / `@section` documentation
3. Include guard (`#ifndef VRTD_<NAME>_H`)
4. Includes (same ordering as `.c`)
5. Constants and macro definitions
6. Type definitions (structs, enums)
7. Function declarations with Doxygen `@brief` / `@param` / `@return`
8. Static inline implementations (cleanup helpers, utilities)
9. `#endif`

## Logging

All daemon logging goes through the `LOG` macro, which is shorthand for
`sd_journal_print` (cast to `void` to suppress unused-result warnings):

```c
LOG(LOG_ERR, "Failed to open device %s", path);
LOG(LOG_DEBUG, "Buffer allocated: addr=0x%llx size=%llu", addr, size);
```

Log levels follow the syslog convention: `LOG_CRIT`, `LOG_ERR`,
`LOG_WARNING`, `LOG_INFO`, `LOG_DEBUG`.

Prefer the `_LOG` variants of the `PROPAGATE_ERROR` macros over a
manual `LOG` + `return -1` pair when the error propagation pattern
fits — they are more concise and harder to get wrong.
