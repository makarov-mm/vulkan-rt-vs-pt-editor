#pragma once
// -----------------------------------------------------------------------------
//  C++26 adoption shims
//
//  The project builds with /std:c++latest (MSVC's C++26-track mode), but as of
//  mid-2026 MSVC has implemented almost none of the C++26 language or library
//  papers. Each feature below is therefore used through its standard feature
//  test macro: the code is written against C++26 and lights up automatically
//  as the toolchain catches up, while remaining buildable today. This is the
//  adoption pattern the uneven C++26 landscape currently requires.
// -----------------------------------------------------------------------------
#include <cstddef>
#include <version>

// P2573R2: = delete("reason"). Until the compiler implements it, the reason
// string is dropped and a plain = delete remains.
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
#  define CXX26_DELETED(reason) delete(reason)
#else
#  define CXX26_DELETED(reason) delete
#endif

// P1383R2: constexpr <cmath>. std::sqrt becomes constexpr in C++26; until the
// standard library ships it, the function stays a plain inline.
#if defined(__cpp_lib_constexpr_cmath) && __cpp_lib_constexpr_cmath >= 202306L
#  define CXX26_CONSTEXPR_MATH constexpr
#else
#  define CXX26_CONSTEXPR_MATH inline
#endif

// P0843R14: std::inplace_vector -- fixed capacity, in-object storage, and,
// crucially for Vulkan descriptor updates, pointer stability: elements never
// relocate, so taking the address of the last pushed element is always safe.
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
#  include <inplace_vector>
template <class T, std::size_t N>
using FixedVector = std::inplace_vector<T, N>;
#else
// Minimal polyfill with the same stability guarantee (in-object array, no
// reallocation) covering exactly the subset this project uses. Retire it the
// day MSVC STL ships P0843.
#  include <cassert>
#  include <new>
#  include <utility>
template <class T, std::size_t N>
class FixedVector {
public:
    FixedVector() = default;
    FixedVector(const FixedVector&)            = CXX26_DELETED("not needed by this project; the polyfill stays minimal");
    FixedVector& operator=(const FixedVector&) = CXX26_DELETED("not needed by this project; the polyfill stays minimal");
    ~FixedVector() {
        for (std::size_t i = 0; i < count; ++i) ptr(i)->~T();
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        assert(count < N && "FixedVector capacity exceeded");
        T* p = new (ptr(count)) T(std::forward<Args>(args)...);
        ++count;
        return *p;
    }
    void push_back(const T& v) { emplace_back(v); }

    T&       back()       { assert(count > 0); return *ptr(count - 1); }
    const T& back() const { assert(count > 0); return *ptr(count - 1); }

    T*          data()       { return ptr(0); }
    const T*    data() const { return ptr(0); }
    std::size_t size()  const { return count; }

private:
    T*       ptr(std::size_t i)       { return reinterpret_cast<T*>(storage) + i; }
    const T* ptr(std::size_t i) const { return reinterpret_cast<const T*>(storage) + i; }

    alignas(T) unsigned char storage[sizeof(T) * N];
    std::size_t count = 0;
};
#endif
