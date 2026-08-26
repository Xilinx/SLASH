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

class BarFile;

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
 * @warning The originating BarFile must outlive this pointer.
 */
template<class T>
class BarFilePtr {
    /** Typed BAR access requires an object type. */
    static_assert(std::is_object_v<T>, "T must be an object type");
public:
    /** Unqualified element type exposed by this pointer wrapper. */
    using element_type = T;

    /** Volatile pointer type used for MMIO loads and stores. */
    using pointer      = volatile T*;

    /** Non-throwing cleanup action run once when ownership ends. */
    using callback_t   = std::function<void()>;

    /**
     * @brief Transfer pointer and cleanup ownership from @p other.
     *
     * The moved-from pointer becomes null and will not run the callback.
     */
    BarFilePtr(BarFilePtr&& other) noexcept
        : p_(other.p_), cb_(std::move(other.cb_)) {
        other.p_ = nullptr;
        other.cb_ = nullptr;
    }

    /**
     * @brief End the current access session, then adopt @p other.
     *
     * The moved-from pointer becomes null and will not run the callback.
     */
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

    /** Copying would run one cleanup action from multiple owners. */
    BarFilePtr(const BarFilePtr&)            = delete;
    BarFilePtr& operator=(const BarFilePtr&) = delete;

    /**
     * @brief Destructor; runs the callback at most once if present.
     */
    ~BarFilePtr() { run_callback(); }

    /**
     * @brief Return the wrapped volatile element pointer.
     */
    operator pointer() const noexcept { return p_; }

    /**
     * @brief Return the wrapped address without its element type.
     */
    operator volatile void*() const noexcept { return p_; }

    /** Return the wrapped pointer without transferring cleanup ownership. */
    pointer get()        const noexcept { return p_; }

    /** Dereference the wrapped volatile element. */
    volatile T& operator*() const noexcept { return *p_; }

    /** Access a member through the wrapped volatile element pointer. */
    pointer     operator->() const noexcept { return p_; }

    /**
     * @brief Index from the wrapped address.
     *
     * Bounds and alignment remain the caller's responsibility.
     */
    volatile T& operator[](std::size_t i) const noexcept { return p_[i]; }

    /**
     * @brief Return whether the wrapped pointer is non-null.
     */
    explicit operator bool() const noexcept { return p_ != nullptr; }

    /** Compare two wrappers by their raw pointer values. */
    friend bool operator==(const BarFilePtr& a, const BarFilePtr& b) noexcept { return a.p_ == b.p_; }
    friend bool operator!=(const BarFilePtr& a, const BarFilePtr& b) noexcept { return !(a == b); }

    /** Compare the wrapped pointer with null. */
    friend bool operator==(const BarFilePtr& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }
    friend bool operator==(std::nullptr_t, const BarFilePtr& a) noexcept { return a.p_ == nullptr; }
    friend bool operator!=(const BarFilePtr& a, std::nullptr_t) noexcept { return a.p_ != nullptr; }
    friend bool operator!=(std::nullptr_t, const BarFilePtr& a) noexcept { return a.p_ != nullptr; }

    /** Compare the wrapper with a raw volatile pointer value. */
    friend bool operator==(const BarFilePtr& a, pointer p) noexcept { return a.p_ == p; }
    friend bool operator==(pointer p, const BarFilePtr& a) noexcept { return a.p_ == p; }
    friend bool operator!=(const BarFilePtr& a, pointer p) noexcept { return a.p_ != p; }
    friend bool operator!=(pointer p, const BarFilePtr& a) noexcept { return a.p_ != p; }

private:
    friend class BarFile;

    /**
     * @brief Construct a BAR access session for BarFile.
     *
     * @param p  Raw volatile pointer within the BAR mapping.
     * @param cb Callback to run on destruction (e.g., to end a read/write session).
     *
     * @warning @p cb must not throw because cleanup runs from noexcept paths.
     */
    explicit BarFilePtr(pointer p, callback_t cb) noexcept
        : p_(p), cb_(std::move(cb)) {}

    /**
     * @brief Consume and invoke the cleanup action at most once.
     *
     * Move the callback out before invocation so re-entrant destruction cannot
     * invoke the same cleanup twice.
     */
    void run_callback() noexcept {
        if (cb_) {
            auto cb = std::move(cb_);
            cb_ = nullptr;
            cb();
        }
    }

    /** Borrowed volatile mapping address, or null after move. */
    pointer     p_  = nullptr;

    /** Sole owner of the access-session cleanup action. */
    callback_t  cb_ = nullptr;
};

} // namespace vrtd

#endif // VRTD_BAR_FILE_PTR_HPP
