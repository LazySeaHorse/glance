// Minimal COM smart pointer (mingw-friendly, no WRL dependency).
#pragma once
#include <windows.h>
#include <cstddef>

template <class T>
class ComPtr {
public:
    ComPtr() : p_(nullptr) {}
    ComPtr(std::nullptr_t) : p_(nullptr) {}
    ComPtr(T* p) : p_(p) { if (p_) p_->AddRef(); }
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) {
            if (o.p_) o.p_->AddRef();
            reset();
            p_ = o.p_;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) { reset(); return *this; }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const ComPtr& o) const { return p_ == o.p_; }
    bool operator!=(const ComPtr& o) const { return p_ != o.p_; }

    // Release current and return address for an out-parameter.
    T** put() { reset(); return &p_; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }

    void reset() {
        if (p_) {
            T* t = p_;
            p_ = nullptr;
            t->Release();
        }
    }

    template <class U>
    HRESULT as(ComPtr<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(__uuidof(U), out.putVoid());
    }

private:
    T* p_;
};
