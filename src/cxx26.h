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
// so the address of a pushed element stays valid.
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
#  include <inplace_vector>
template <class T, std::size_t N>
using FixedVector = std::inplace_vector<T, N>;
#else
// Minimal polyfill with the same stability guarantee, covering exactly the
// subset this project uses (it only ever holds plain Vulkan structs). Retire
// it once the standard library ships P0843.
#  include <array>
#  include <cassert>
#  include <type_traits>
template <class T, std::size_t N>
    requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class FixedVector {
public:
    constexpr T& push_back(const T& v) {
        assert(count_ < N && "FixedVector capacity exceeded");
        return items_[count_++] = v;
    }

    [[nodiscard]] constexpr T*          data()        noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T*    data()  const noexcept { return items_.data(); }
    [[nodiscard]] constexpr std::size_t size()  const noexcept { return count_; }
    [[nodiscard]] constexpr bool        empty() const noexcept { return count_ == 0; }

    [[nodiscard]] constexpr T*       begin()       noexcept { return items_.data(); }
    [[nodiscard]] constexpr T*       end()         noexcept { return items_.data() + count_; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T* end()   const noexcept { return items_.data() + count_; }

private:
    std::array<T, N> items_{};
    std::size_t      count_ = 0;
};
#endif
