#pragma once
// -----------------------------------------------------------------------------
//  C++26 adoption shims
//
//  The project builds with /std:c++latest, but MSVC implements only part of
//  the C++26 paper set so far. Each feature below is used through its standard
//  feature-test macro: the code is written against C++26 and lights up
//  automatically as the toolchain catches up, while staying buildable today.
// -----------------------------------------------------------------------------
#include <cstddef>
#include <version>

// P2573R2: = delete("reason"). Without compiler support the reason is dropped.
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
#  define CXX26_DELETED(reason) delete(reason)
#else
#  define CXX26_DELETED(reason) delete
#endif

// P1383R2: constexpr <cmath>. Until std::sqrt is constexpr, functions using it
// stay plain inline.
#if defined(__cpp_lib_constexpr_cmath) && __cpp_lib_constexpr_cmath >= 202306L
#  define CXX26_CONSTEXPR_MATH constexpr
#else
#  define CXX26_CONSTEXPR_MATH inline
#endif

// P0843R14: std::inplace_vector -- fixed capacity, in-object storage and, what
// Vulkan descriptor updates need, pointer stability: elements never relocate,
// so the address of the last pushed element stays valid.
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
#  include <inplace_vector>
template <class T, std::size_t N>
using FixedVector = std::inplace_vector<T, N>;
#else
// Minimal polyfill with the same stability guarantee, covering exactly the
// subset this project uses. Retire it once the standard library ships P0843.
#  include <cassert>
#  include <memory>
#  include <utility>
template <class T, std::size_t N>
class FixedVector {
public:
    FixedVector() = default;
    FixedVector(const FixedVector&)            = CXX26_DELETED("not needed by this project; the polyfill stays minimal");
    FixedVector& operator=(const FixedVector&) = CXX26_DELETED("not needed by this project; the polyfill stays minimal");
    ~FixedVector() { std::destroy_n(data(), count_); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        assert(count_ < N && "FixedVector capacity exceeded");
        T* p = std::construct_at(data() + count_, std::forward<Args>(args)...);
        ++count_;
        return *p;
    }
    T& push_back(const T& v) { return emplace_back(v); }

    [[nodiscard]] T&       back()       { assert(count_ > 0); return data()[count_ - 1]; }
    [[nodiscard]] const T& back() const { assert(count_ > 0); return data()[count_ - 1]; }

    [[nodiscard]] T*          data()       noexcept { return reinterpret_cast<T*>(storage_); }
    [[nodiscard]] const T*    data() const noexcept { return reinterpret_cast<const T*>(storage_); }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    alignas(T) std::byte storage_[sizeof(T) * N];
    std::size_t count_ = 0;
};
#endif
