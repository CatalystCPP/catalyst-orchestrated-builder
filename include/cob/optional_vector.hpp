#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

// NOLINTBEGIN(readability-identifier-naming)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
namespace catalyst {

namespace detail {
[[noreturn]] void throw_bad_alloc();
void *allocate_optional_vector(size_t bytes);
void *reallocate_optional_vector(void *ptr, size_t bytes);
void deallocate_optional_vector(void *ptr) noexcept;
} // namespace detail

/**
 * @brief A compact, single-indirection optional vector container.
 *
 * Occupies exactly 8 bytes (one pointer) inline when empty. When allocated,
 * packages both the capacity/size metadata and the elements inline in a single
 * heap allocation. This minimizes struct padding, heap fragmentation, and provides
 * direct single-indirection element access.
 */
template <typename Element_T> class optional_vector {
private:
    struct Header {
        uint32_t capacity;
        uint32_t size;
    };

    Header *header_ = nullptr;

    static constexpr size_t header_size() noexcept {
        return (sizeof(Header) + alignof(Element_T) - 1) & ~(alignof(Element_T) - 1);
    }

    Element_T *elements() const noexcept {
        if (!header_)
            return nullptr;
        return reinterpret_cast<Element_T *>(reinterpret_cast<char *>(header_) + header_size());
    }

public:
    using value_type = Element_T;
    using size_type = size_t;
    using reference = Element_T &;
    using const_reference = const Element_T &;
    using pointer = Element_T *;
    using const_pointer = const Element_T *;
    using iterator = Element_T *;
    using const_iterator = const Element_T *;

    optional_vector() noexcept = default;

    ~optional_vector() {
        clear();
    }

    // Move semantics
    optional_vector(optional_vector &&other) noexcept : header_(other.header_) {
        other.header_ = nullptr;
    }

    optional_vector &operator=(optional_vector &&other) noexcept {
        if (this != &other) {
            clear();
            header_ = other.header_;
            other.header_ = nullptr;
        }
        return *this;
    }

    // Disable copy semantics to prevent accidental expensive copies (prefer explicit cloning)
    optional_vector(const optional_vector &) = delete;
    optional_vector &operator=(const optional_vector &) = delete;

    [[nodiscard]] bool has_value() const noexcept {
        return header_ != nullptr;
    }
    [[nodiscard]] size_type size() const noexcept {
        return header_ ? header_->size : 0;
    }
    [[nodiscard]] size_type capacity() const noexcept {
        return header_ ? header_->capacity : 0;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    void emplace() {
        if (!header_) {
            reserve(4);
        }
    }

    void reserve(size_type new_cap) {
        if (header_ && new_cap <= header_->capacity)
            return;

        size_t bytes = header_size() + (new_cap * sizeof(Element_T));
        Header *new_header = nullptr;

        if (header_) {
            new_header = reinterpret_cast<Header *>(detail::reallocate_optional_vector(header_, bytes));
            new_header->capacity = static_cast<uint32_t>(new_cap);
        } else {
            new_header = reinterpret_cast<Header *>(detail::allocate_optional_vector(bytes));
            new_header->capacity = static_cast<uint32_t>(new_cap);
            new_header->size = 0;
        }
        header_ = new_header;
    }

    template <typename... Args> reference emplace_back(Args &&...args) {
        if (!header_) {
            emplace();
        }
        if (header_->size >= header_->capacity) {
            reserve(header_->capacity == 0 ? 4 : header_->capacity * 2);
        }
        Element_T *elem_ptr = elements() + header_->size;
        new (elem_ptr) Element_T(std::forward<Args>(args)...);
        header_->size++;
        return *elem_ptr;
    }

    void push_back(const Element_T &val) {
        emplace_back(val);
    }

    void push_back(Element_T &&val) {
        emplace_back(std::move(val));
    }

    void pop_back() noexcept {
        if (header_ && header_->size > 0) {
            header_->size--;
            elements()[header_->size].~Element_T();
        }
    }

    void clear() noexcept {
        if (header_) {
            Element_T *el = elements();
            for (uint32_t i = 0; i < header_->size; ++i) {
                el[i].~Element_T();
            }
            detail::deallocate_optional_vector(header_);
            header_ = nullptr;
        }
    }

    reference operator[](size_type index) noexcept {
        return elements()[index];
    }
    const_reference operator[](size_type index) const noexcept {
        return elements()[index];
    }

    reference at(size_type index) {
        if (index >= size()) {
            throw std::out_of_range("optional_vector index out of range");
        }
        return elements()[index];
    }

    const_reference at(size_type index) const {
        if (index >= size()) {
            throw std::out_of_range("optional_vector index out of range");
        }
        return elements()[index];
    }

    reference front() noexcept {
        return elements()[0];
    }
    const_reference front() const noexcept {
        return elements()[0];
    }
    reference back() noexcept {
        return elements()[header_->size - 1];
    }
    const_reference back() const noexcept {
        return elements()[header_->size - 1];
    }

    pointer data() noexcept {
        return elements();
    }
    const_pointer data() const noexcept {
        return elements();
    }

    iterator begin() noexcept {
        return elements();
    }
    iterator end() noexcept {
        return elements() + size();
    }
    const_iterator begin() const noexcept {
        return elements();
    }
    const_iterator end() const noexcept {
        return elements() + size();
    }
    const_iterator cbegin() const noexcept {
        return elements();
    }
    const_iterator cend() const noexcept {
        return elements() + size();
    }
};

} // namespace catalyst
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTEND(readability-identifier-naming)
