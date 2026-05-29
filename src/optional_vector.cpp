#include "cbe/optional_vector.hpp"

#include <cstdlib>
#include <new>

namespace catalyst::detail {

// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
[[noreturn]] void throw_bad_alloc() {
    throw std::bad_alloc();
}

void* allocate_optional_vector(size_t bytes) {
    void* ptr = std::malloc(bytes);
    if (!ptr) {
        throw_bad_alloc();
    }
    return ptr;
}

void* reallocate_optional_vector(void* ptr, size_t bytes) {
    void* new_ptr = std::realloc(ptr, bytes);
    if (!new_ptr) {
        throw_bad_alloc();
    }
    return new_ptr;
}

void deallocate_optional_vector(void* ptr) noexcept {
    std::free(ptr);
}
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

} // namespace catalyst::detail
