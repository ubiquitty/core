// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Ubiquitty {

/// Why a fallible storage request was refused.
enum class StorageError : std::uint8_t {
    OverCapacity = 1,
    AllocationRefused = 2,
};

/// Contiguous objects with explicit, exact-capacity fallible reservations.
///
/// The try operations report allocation failure and preserve existing elements
/// when reservation fails. Legacy vector-shaped operations retain the standard
/// container's fatal allocation-failure contract; untrusted decoders use only
/// the explicit fallible operations and fallible element-copy seams.
template<typename T>
class FallibleVector {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);

    struct StorageDeleter {
        void operator()(T* pointer) const noexcept
        {
            if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                ::operator delete(pointer, std::align_val_t { alignof(T) });
            else
                ::operator delete(pointer);
        }
    };

public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using iterator = T*;
    using const_iterator = const T*;

    FallibleVector() = default;
    explicit FallibleVector(std::size_t count) { require(tryResize(count)); }
    FallibleVector(std::size_t count, const T& value) { require(tryAssign(count, value)); }
    FallibleVector(std::initializer_list<T> values) { require(tryAssign(values.begin(), values.end())); }
    template<std::input_iterator Iterator>
    FallibleVector(Iterator first, Iterator last) { require(tryAssign(first, last)); }
    FallibleVector(const std::vector<T>& values) { require(tryAssign(values.begin(), values.end())); }
    FallibleVector(const FallibleVector& other) { require(tryAssign(other.begin(), other.end())); }
    FallibleVector(FallibleVector&& other) noexcept { swap(other); }
    ~FallibleVector() { clear(); }

    FallibleVector& operator=(FallibleVector&& other) noexcept
    {
        if (this != &other) {
            FallibleVector previous(std::move(other));
            swap(previous);
        }
        return *this;
    }
    FallibleVector& operator=(const FallibleVector& other)
    {
        if (this != &other)
            require(tryAssign(other.begin(), other.end()));
        return *this;
    }
    FallibleVector& operator=(std::initializer_list<T> values)
    {
        require(tryAssign(values.begin(), values.end()));
        return *this;
    }
    FallibleVector& operator=(const std::vector<T>& values)
    {
        require(tryAssign(values.begin(), values.end()));
        return *this;
    }

    [[nodiscard]] T* data() noexcept { return m_storage.get(); }
    [[nodiscard]] const T* data() const noexcept { return m_storage.get(); }
    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }
    [[nodiscard]] iterator end() noexcept { return m_size ? data() + m_size : data(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_size ? data() + m_size : data(); }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }
    [[nodiscard]] auto rbegin() noexcept { return std::reverse_iterator(end()); }
    [[nodiscard]] auto rbegin() const noexcept { return std::reverse_iterator(end()); }
    [[nodiscard]] auto rend() noexcept { return std::reverse_iterator(begin()); }
    [[nodiscard]] auto rend() const noexcept { return std::reverse_iterator(begin()); }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return !m_size; }
    [[nodiscard]] static constexpr std::size_t max_size() noexcept { return std::numeric_limits<std::size_t>::max() / sizeof(T); }
    [[nodiscard]] std::size_t allocationBytes() const noexcept { return m_capacity * sizeof(T); }
    [[nodiscard]] T& operator[](std::size_t index)
    {
        assert(index < m_size);
        return data()[index];
    }
    [[nodiscard]] const T& operator[](std::size_t index) const
    {
        assert(index < m_size);
        return data()[index];
    }
    [[nodiscard]] T& front() { return (*this)[0]; }
    [[nodiscard]] const T& front() const { return (*this)[0]; }
    [[nodiscard]] T& back() { return (*this)[m_size - 1]; }
    [[nodiscard]] const T& back() const { return (*this)[m_size - 1]; }

    /// Reserves exactly the requested capacity when growth is necessary.
    [[nodiscard]] std::expected<void, StorageError> tryReserve(std::size_t count)
    {
        if (count <= m_capacity)
            return {};
        if (count > max_size())
            return std::unexpected(StorageError::OverCapacity);
        void* allocation;
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            allocation = ::operator new(count * sizeof(T), std::align_val_t { alignof(T) }, std::nothrow);
        else
            allocation = ::operator new(count * sizeof(T), std::nothrow);
        if (!allocation)
            return std::unexpected(StorageError::AllocationRefused);
        std::unique_ptr<T, StorageDeleter> replacement(static_cast<T*>(allocation));
        for (std::size_t index = 0; index < m_size; ++index)
            std::construct_at(replacement.get() + index, std::move(data()[index]));
        std::destroy_n(data(), m_size);
        m_storage = std::move(replacement);
        m_capacity = count;
        return {};
    }

    [[nodiscard]] std::expected<void, StorageError> tryResize(std::size_t count)
    {
        if (auto reserved = tryReserve(count); !reserved)
            return reserved;
        while (m_size > count)
            pop_back();
        while (m_size < count) {
            std::construct_at(data() + m_size);
            ++m_size;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, StorageError> tryResize(std::size_t count, const T& value)
    {
        if (count <= m_size)
            return tryResize(count);
        if (count > m_capacity) {
            T snapshot;
            if constexpr (requires { snapshot.tryAssign(value); }) {
                if (!snapshot.tryAssign(value))
                    return std::unexpected(StorageError::AllocationRefused);
            } else
                snapshot = value;
            if (auto reserved = tryReserve(count); !reserved)
                return reserved;
            return tryResize(count, snapshot);
        }
        if (auto reserved = tryReserve(count); !reserved)
            return reserved;
        while (m_size < count) {
            if (auto copied = copyAt(data() + m_size, value); !copied)
                return copied;
            ++m_size;
        }
        return {};
    }

    template<std::input_iterator Iterator>
    [[nodiscard]] std::expected<void, StorageError> tryAssign(Iterator first, Iterator last)
    {
        if (first == last) {
            clear();
            return {};
        }
        if constexpr (std::is_trivially_copyable_v<T> && std::forward_iterator<Iterator>) {
            const auto distance = std::distance(first, last);
            if (distance < 0)
                return std::unexpected(StorageError::OverCapacity);
            const auto count = static_cast<std::size_t>(distance);
            if (count > m_size) {
                if (auto resized = tryResize(count); !resized)
                    return resized;
            }
            if constexpr (std::contiguous_iterator<Iterator> && std::is_same_v<std::remove_cv_t<std::iter_value_t<Iterator>>, T>)
                std::memmove(data(), std::to_address(first), count * sizeof(T));
            else
                std::copy(first, last, begin());
            if (auto resized = tryResize(count); !resized)
                return resized;
            return {};
        } else {
            FallibleVector candidate;
            if constexpr (std::forward_iterator<Iterator>) {
                const auto distance = std::distance(first, last);
                if (distance < 0)
                    return std::unexpected(StorageError::OverCapacity);
                if (auto reserved = candidate.tryReserve(static_cast<std::size_t>(distance)); !reserved)
                    return reserved;
            }
            for (; first != last; ++first) {
                if (auto appended = candidate.tryPushBack(*first); !appended)
                    return appended;
            }
            swap(candidate);
            return {};
        }
    }

    [[nodiscard]] std::expected<void, StorageError> tryAssign(const FallibleVector& other)
    {
        if (this == &other)
            return {};
        return tryAssign(other.begin(), other.end());
    }

    [[nodiscard]] std::expected<void, StorageError> tryAssign(std::size_t count, const T& value)
    {
        FallibleVector candidate;
        if (auto reserved = candidate.tryReserve(count); !reserved)
            return reserved;
        for (std::size_t index = 0; index < count; ++index) {
            if (auto appended = candidate.tryPushBack(value); !appended)
                return appended;
        }
        swap(candidate);
        return {};
    }

    /// Appends a prepared element or constructs one with its ordinary constructor.
    ///
    /// A single `T` uses the fallible copy/move seam and may alias this vector.
    /// Other arguments retain `T`'s constructor allocation policy; untrusted
    /// decoders default-construct or move an independently prepared element.
    template<typename... Arguments>
    [[nodiscard]] std::expected<T*, StorageError> tryEmplaceBack(Arguments&&... arguments)
    {
        if constexpr (sizeof...(Arguments) == 1 && (std::is_same_v<T, std::remove_cvref_t<Arguments>> && ...)) {
            if (const auto appended = tryPushBack(std::forward<Arguments>(arguments)...); !appended)
                return std::unexpected(appended.error());
            return data() + m_size - 1;
        } else {
            if constexpr (sizeof...(Arguments) > 0) {
                if (m_size == m_capacity) {
                    T value(std::forward<Arguments>(arguments)...);
                    if (auto reserved = reserveForAppend(); !reserved)
                        return std::unexpected(reserved.error());
                    T* appended = std::construct_at(data() + m_size, std::move(value));
                    ++m_size;
                    return appended;
                }
            }
            if (auto reserved = reserveForAppend(); !reserved)
                return std::unexpected(reserved.error());
            T* value = std::construct_at(data() + m_size, std::forward<Arguments>(arguments)...);
            ++m_size;
            return value;
        }
    }

    [[nodiscard]] std::expected<void, StorageError> tryPushBack(const T& value)
    {
        if (m_size == m_capacity) {
            T snapshot;
            if constexpr (requires { snapshot.tryAssign(value); }) {
                if (!snapshot.tryAssign(value))
                    return std::unexpected(StorageError::AllocationRefused);
            } else
                snapshot = value;
            if (auto reserved = reserveForAppend(); !reserved)
                return reserved;
            std::construct_at(data() + m_size, std::move(snapshot));
            ++m_size;
            return {};
        }
        if (auto reserved = reserveForAppend(); !reserved)
            return reserved;
        if (auto copied = copyAt(data() + m_size, value); !copied)
            return copied;
        ++m_size;
        return {};
    }
    [[nodiscard]] std::expected<void, StorageError> tryPushBack(T&& value)
    {
        std::optional<std::size_t> ownIndex;
        if (m_size == m_capacity) {
            for (std::size_t index = 0; index < m_size; ++index) {
                if (&data()[index] == &value) {
                    ownIndex = index;
                    break;
                }
            }
        }
        if (auto reserved = reserveForAppend(); !reserved)
            return reserved;
        std::construct_at(data() + m_size, std::move(ownIndex ? data()[*ownIndex] : value));
        ++m_size;
        return {};
    }

    void reserve(std::size_t count) { require(tryReserve(count)); }
    void resize(std::size_t count) { require(tryResize(count)); }
    void resize(std::size_t count, const T& value) { require(tryResize(count, value)); }
    template<std::input_iterator Iterator>
    void assign(Iterator first, Iterator last) { require(tryAssign(first, last)); }
    void assign(std::size_t count, const T& value) { require(tryAssign(count, value)); }
    void assign(std::initializer_list<T> values) { assign(values.begin(), values.end()); }
    void push_back(const T& value) { require(tryPushBack(value)); }
    void push_back(T&& value) { require(tryPushBack(std::move(value))); }
    template<typename... Arguments>
    T& emplace_back(Arguments&&... arguments)
    {
        auto appended = tryEmplaceBack(std::forward<Arguments>(arguments)...);
        if (!appended)
            std::abort();
        return **appended;
    }
    void pop_back() noexcept
    {
        assert(m_size);
        std::destroy_at(data() + --m_size);
    }
    void clear() noexcept
    {
        std::destroy_n(data(), m_size);
        m_size = 0;
    }
    void swap(FallibleVector& other) noexcept
    {
        m_storage.swap(other.m_storage);
        std::swap(m_size, other.m_size);
        std::swap(m_capacity, other.m_capacity);
    }
    iterator erase(const_iterator position) { return erase(position, position + 1); }
    iterator erase(const_iterator first, const_iterator last)
    {
        if (first == last)
            return first == begin() ? begin() : begin() + (first - begin());
        const auto start = first == begin() ? 0 : static_cast<std::size_t>(first - begin());
        const auto count = first == last ? 0 : static_cast<std::size_t>(last - first);
        assert(start <= m_size && count <= m_size - start);
        for (std::size_t index = start; index < m_size - count; ++index)
            data()[index] = std::move(data()[index + count]);
        const auto newSize = m_size - count;
        while (m_size > newSize)
            pop_back();
        return start ? data() + start : data();
    }
    template<std::input_iterator Iterator>
    iterator insert(const_iterator position, Iterator first, Iterator last)
    {
        const auto start = position == begin() ? 0 : static_cast<std::size_t>(position - begin());
        FallibleVector inserted(first, last);
        assert(start <= m_size);
        if (inserted.empty())
            return start ? begin() + start : begin();
        if (inserted.size() > max_size() - m_size)
            std::abort();
        const auto originalSize = m_size;
        reserve(m_size + inserted.size());
        for (std::size_t index = 0; index < inserted.size(); ++index)
            emplace_back();
        std::move_backward(begin() + start, begin() + originalSize, end());
        std::move(inserted.begin(), inserted.end(), begin() + start);
        return begin() + start;
    }
    iterator insert(const_iterator position, const T& value) { return insert(position, &value, &value + 1); }

    friend bool operator==(const FallibleVector& left, const FallibleVector& right)
    {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }
    friend bool operator==(const FallibleVector& left, const std::vector<T>& right)
    {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

private:
    static void require(const std::expected<void, StorageError>& result)
    {
        if (!result)
            std::abort();
    }

    [[nodiscard]] std::expected<void, StorageError> reserveForAppend()
    {
        if (m_size < m_capacity)
            return {};
        if (m_size == max_size())
            return std::unexpected(StorageError::OverCapacity);
        const auto capacity = m_capacity > max_size() / 2 ? max_size() : std::max<std::size_t>(1, 2 * m_capacity);
        return tryReserve(capacity);
    }

    [[nodiscard]] static std::expected<void, StorageError> copyAt(T* destination, const T& source)
    {
        if constexpr (requires(T& value) { value.tryAssign(source); }) {
            std::construct_at(destination);
            const auto copied = destination->tryAssign(source);
            if (!copied) {
                std::destroy_at(destination);
                return std::unexpected(StorageError::AllocationRefused);
            }
        } else {
            std::construct_at(destination, source);
        }
        return {};
    }

    std::unique_ptr<T, StorageDeleter> m_storage;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
};

} // namespace Ubiquitty
