#pragma once
// Qt 6.5's QVarLengthArray (via QT_MAKE_CHECKED_ARRAY_ITERATOR / QT_MAKE_UNCHECKED_ARRAY_ITERATOR in qcompilerdetection.h) calls
// stdext::make_checked_array_iterator and stdext::make_unchecked_array_iterator. The MSVC STL of Visual Studio 2026 (toolset 14.50+)
// no longer has them, so every translation unit that includes Qt would fail to compile.
//
// This header supplies them for those toolsets only (plain pointers: the checked variant only added debug-time bounds checks).
// It is force-included (/FI) by the build files; nothing in the sources includes it. Older toolsets and other compilers see nothing.
#if defined(_MSC_VER) && _MSC_VER >= 1950
#include <cstddef>
namespace stdext {
template <class T>
inline T* make_checked_array_iterator(T* p, std::size_t) { return p; }
template <class T>
inline T* make_checked_array_iterator(T* p, std::size_t, std::size_t) { return p; }
template <class T>
inline T* make_unchecked_array_iterator(T* p) { return p; }
}  // namespace stdext
#endif
