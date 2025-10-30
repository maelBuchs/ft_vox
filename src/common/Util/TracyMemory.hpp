#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

#include <tracy/Tracy.hpp>

/**
 * Tracy Memory Profiling Integration
 *
 * This header overrides global C++ memory allocation operators to enable
 * memory tracking in Tracy profiler. Include this file ONCE in your main
 * translation unit (main.cpp) to enable global memory profiling.
 *
 * When TRACY_ENABLE is not defined, these operators still function normally
 * but without profiling overhead.
 *
 * Tracked allocations:
 * - operator new / delete (single object)
 * - operator new[] / delete[] (arrays)
 * - Sized delete variants (C++14)
 * - Aligned allocation (C++17)
 */

// ============================================================================
// Regular new/delete (single object)
// ============================================================================

void* operator new(std::size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
#ifdef TRACY_ENABLE
    TracyAlloc(ptr, size);
#endif
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
}

// ============================================================================
// Array new[]/delete[] (arrays)
// ============================================================================

void* operator new[](std::size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
#ifdef TRACY_ENABLE
    TracyAlloc(ptr, size);
#endif
    return ptr;
}

void operator delete[](void* ptr) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
}

// ============================================================================
// Sized delete (C++14) - provides size hint for better tracking
// ============================================================================

void operator delete(void* ptr, std::size_t size) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
    (void)size; // Size is primarily for optimization, not needed with malloc/free
}

void operator delete[](void* ptr, std::size_t size) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
    (void)size; // Size is primarily for optimization, not needed with malloc/free
}

// ============================================================================
// Aligned allocation (C++17) - for over-aligned types
// ============================================================================

#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)

void* operator new(std::size_t size, std::align_val_t alignment) {
#ifdef _WIN32
    void* ptr = _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    void* ptr = std::aligned_alloc(static_cast<std::size_t>(alignment), size);
#endif
    if (!ptr) {
        throw std::bad_alloc();
    }
#ifdef TRACY_ENABLE
    TracyAlloc(ptr, size);
#endif
    return ptr;
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)alignment;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
#ifdef _WIN32
    void* ptr = _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    void* ptr = std::aligned_alloc(static_cast<std::size_t>(alignment), size);
#endif
    if (!ptr) {
        throw std::bad_alloc();
    }
#ifdef TRACY_ENABLE
    TracyAlloc(ptr, size);
#endif
    return ptr;
}

void operator delete[](void* ptr, std::align_val_t alignment) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)alignment;
}

// Sized + aligned delete (C++17)
void operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)size;
    (void)alignment;
}

void operator delete[](void* ptr, std::size_t size, std::align_val_t alignment) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)size;
    (void)alignment;
}

#endif // C++17

// ============================================================================
// Nothrow variants - return nullptr instead of throwing on failure
// ============================================================================

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* ptr = std::malloc(size);
#ifdef TRACY_ENABLE
    if (ptr) {
        TracyAlloc(ptr, size);
    }
#endif
    return ptr;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* ptr = std::malloc(size);
#ifdef TRACY_ENABLE
    if (ptr) {
        TracyAlloc(ptr, size);
    }
#endif
    return ptr;
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
    std::free(ptr);
}

#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
#ifdef _WIN32
    void* ptr = _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    void* ptr = std::aligned_alloc(static_cast<std::size_t>(alignment), size);
#endif
#ifdef TRACY_ENABLE
    if (ptr) {
        TracyAlloc(ptr, size);
    }
#endif
    return ptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
#ifdef _WIN32
    void* ptr = _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    void* ptr = std::aligned_alloc(static_cast<std::size_t>(alignment), size);
#endif
#ifdef TRACY_ENABLE
    if (ptr) {
        TracyAlloc(ptr, size);
    }
#endif
    return ptr;
}

void operator delete(void* ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)alignment;
}

void operator delete[](void* ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    if (!ptr)
        return;
#ifdef TRACY_ENABLE
    TracyFree(ptr);
#endif
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
    (void)alignment;
}

#endif // C++17
