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

#ifndef VRTD_BAR_FILE_PTR_HPP
#define VRTD_BAR_FILE_PTR_HPP

#include <functional>
#include <type_traits>
#include <cstddef>
#include <utility>

namespace vrtd {

/**
 * @brief Move-only RAII pointer for BAR memory access sessions.
 *
 * A @c BarFilePtr<T> behaves like a @c volatile T* while it is alive and
 * runs a stored callback when destroyed exactly once (used to end the
 * read/write session started by @c BarFile::getPtr()).
 *
 * @tparam T Object type for element access (must satisfy @c std::is_object_v).
 *
 * @warning Not thread-safe. Intended to be short-lived and used on a
 *          single thread that owns the corresponding @c BarFile operation.
 */
template<class T>
class BarFilePtr {
    static_assert(std::is_object_v<T>, "T must be an object type");
public:
    using element_type = T;
    using pointer      = volatile T*;
    using callback_t   = std::function<void()>;

    /**
     * @brief Construct from a raw volatile pointer and optional destructor callback.
     *
     * @param p  Raw volatile pointer within the BAR mapping.
     * @param cb Callback to run on destruction (e.g., to end a read/write session).
     */
    explicit BarFilePtr(pointer p = nullptr, callback_t cb = {}) noexcept
        : p_(p), cb_(std::move(cb)) {}

    // move-only (ensures callback runs at most once)
    BarFilePtr(BarFilePtr&& other) noexcept
        : p_(other.p_), cb_(std::move(other.cb_)) {
        other.p_ = nullptr;
        other.cb_ = nullptr;
    }
    BarFilePtr& operator=(BarFilePtr&& other) noexcept {
        if (this != &other) {
            run_callback();
            p_  = other.p_;
            cb_ = std::move(other.cb_);
            other.p_ = nullptr;
            other.cb_ = nullptr;
        }
        return *this;
    }

    BarFilePtr(const BarFilePtr&)            = delete;
    BarFilePtr& operator=(const BarFilePtr&) = delete;

    /**
     * @brief Destructor; runs the callback at most once if present.
     */
    ~BarFilePtr() { run_callback(); }

    // ---- implicit conversions (only these two) ----
    /**
     * @brief Implicit conversion to volatile T*.
     */
    operator pointer() const noexcept { return p_; }
    /**
     * @brief Implicit conversion to volatile void*.
     */
    operator volatile void*() const noexcept { return p_; }

    // ---- pointer-like interface ----
    pointer get()        const noexcept { return p_; }
    volatile T& operator*() const noexcept { return *p_; }
    pointer     operator->() const noexcept { return p_; }

    // index (useful for arrays / pointer arithmetic)
    volatile T& operator[](std::size_t i) const noexcept { return p_[i]; }

    /**
     * @brief returns true if non-null.
     */
    explicit operator bool() const noexcept { return p_ != nullptr; }

    // comparisons
    friend bool operator==(const BarFilePtr& a, const BarFilePtr& b) noexcept { return a.p_ == b.p_; }
    friend bool operator!=(const BarFilePtr& a, const BarFilePtr& b) noexcept { return !(a == b); }
    friend bool operator==(const BarFilePtr& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }
    friend bool operator==(std::nullptr_t, const BarFilePtr& a) noexcept { return a.p_ == nullptr; }
    friend bool operator!=(const BarFilePtr& a, std::nullptr_t) noexcept { return a.p_ != nullptr; }
    friend bool operator!=(std::nullptr_t, const BarFilePtr& a) noexcept { return a.p_ != nullptr; }

    // optional: compare directly with a raw volatile T*
    friend bool operator==(const BarFilePtr& a, pointer p) noexcept { return a.p_ == p; }
    friend bool operator==(pointer p, const BarFilePtr& a) noexcept { return a.p_ == p; }
    friend bool operator!=(const BarFilePtr& a, pointer p) noexcept { return a.p_ != p; }
    friend bool operator!=(pointer p, const BarFilePtr& a) noexcept { return a.p_ != p; }

private:
    void run_callback() noexcept {
        if (cb_) {
            auto cb = std::move(cb_);
            cb_ = nullptr;      // ensure single fire
            cb();
        }
    }

    pointer     p_  = nullptr;
    callback_t  cb_ = nullptr;
};

} // namsepace vrtd

#endif // VRTD_BAR_FILE_PTR_HPP
