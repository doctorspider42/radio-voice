#pragma once

#include <unknwn.h>
#include <utility>

namespace rv::audio {

/// Minimal intrusive COM smart pointer.
///
/// Hand-rolled rather than using WRL or _com_ptr_t: those headers are not
/// uniformly available across the MSVC and MinGW toolchains this project
/// supports, and the required surface here is three methods.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}

    ComPtr(const ComPtr& other) : ptr_(other.ptr_)
    {
        if (ptr_)
            ptr_->AddRef();
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& other)
    {
        if (this != &other) {
            if (other.ptr_)
                other.ptr_->AddRef();
            reset();
            ptr_ = other.ptr_;
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void reset()
    {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T*  get() const noexcept { return ptr_; }
    T*  operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    /// For the out-parameter of a creation call. Releases any current value
    /// first so the idiom cannot leak.
    T** put()
    {
        reset();
        return &ptr_;
    }

    void** putVoid()
    {
        reset();
        return reinterpret_cast<void**>(&ptr_);
    }

    T* detach() noexcept
    {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    void attach(T* p) noexcept
    {
        reset();
        ptr_ = p;
    }

private:
    T* ptr_ = nullptr;
};

} // namespace rv::audio
